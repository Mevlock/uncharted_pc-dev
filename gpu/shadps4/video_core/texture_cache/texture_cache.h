// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "bbport_toggles.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    /// bbport: changes whenever an image is registered or unregistered.
    [[nodiscard]] u64 RegistryGeneration() const noexcept {
        return registry_generation.load(std::memory_order_acquire);
    }

    /// bbport: FindImage's access tick for an image found by a memoized lookup. The LRU touch
    /// FindImage also does happens in UpdateImage (FindTexture), which every binding reaches.
    void MarkFound(ImageId image_id) {
        slot_images[image_id].tick_accessed_last = scheduler.CurrentTick();
    }

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range. before_write (bbport): a write fault,
    /// the memory still holds what the CPU is about to overwrite.
    void InvalidateMemory(VAddr addr, size_t size, bool before_write = false);

    /// Marks an image as dirty if it exists at the provided address.
    /// whole_range (bbport): the GPU wrote exactly [address, address + max_size), so every image
    /// overlapping it is dirty, not only one starting at the address (copies moving textures).
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size, bool whole_range = false);

    /// bbport: after the button prompts switched between the game's pictures, the keyboard's and
    /// Xbox, the button prompt pictures are uploaded again (button_prompts.h). GPU thread, once per submission.
    void UpdateButtonPrompts();

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();
    /// bbport diagnosis: images queued for download at the next end-of-pipe event.
    bool HasQueuedDownloads() {
        if (!downloads_queued.load(std::memory_order_acquire)) {
            return false;
        }
        std::unique_lock lk{download_images_mutex};
        return !download_images.empty() || !small_download_images.empty();
    }

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);
    /// BB_PERF_DIAG: counts the images FindImage creates (texture_cache.cpp).
    void NoteImageCreate(const ImageInfo& info, BindingType binding);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// bbport: a FindView result remembered by the caller for the same image and view info.
    struct ViewMemo {
        ImageId image_id{};
        const void* backing = nullptr;
        ImageViewId view_id{};
    };

    /// Retrieves an image view with the properties of the specified image id.
    /// `refresh` false: an image found out of date is used as is (the texture binding helper
    /// checked it beforehand; only a guest write racing with the draw can make it so).
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc,
                                         ViewMemo* memo = nullptr, bool refresh = true);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// bbport: whether UpdateImage has nothing to do for this image (its fast path).
    [[nodiscard]] bool IsUpToDate(ImageId image_id) const {
        const Image& image = slot_images[image_id];
        const u32 flags = std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags)))
                              .load(std::memory_order_acquire);
        constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
        constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
        return (flags & (Dirty | Registered)) == Registered &&
               image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
               image.lru_touched_tick == gc_tick;
    }

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        // bbport: a clean image already tracked and touched in this GC period needs nothing.
        // Every texture binding comes here; the mutex (shared with the fault handlers of the
        // guest threads) was ~3% of the GPU thread. Flags are read atomically: an invalidation
        // racing with this check races the same way with the locked path.
        if (!BbToggle::Disabled(BbToggle::UpdateImageFastPath)) {
            const Image& image = slot_images[image_id];
            const u32 flags = std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags)))
                                  .load(std::memory_order_acquire);
            constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
            constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
            if ((flags & (Dirty | Registered)) == Registered &&
                image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
                image.lru_touched_tick == gc_tick) {
                return;
            }
        }
        std::scoped_lock lock{mutex};
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    /// extra_lod_bias: bbport, added to the S#'s bias (reduced scene rendering).
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth, float extra_lod_bias = 0.0f);

    /// Retrieves the image with the specified id.
    Image* TryGetImage(ImageId id, u64 uid) {
        if (!slot_images.is_allocated(id)) return nullptr;
        auto& image = slot_images[id];
        return !uid || image.image_uid == uid ? &image : nullptr;
    }
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    /// bbport: only for a stencil read (8-bit view format). The link is made for whatever image
    /// starts at the stencil address; once the game reuses that memory for another texture
    /// (Uncharted at level loads), redirecting e.g. an RGBA16F read to the D32S8 image created
    /// an invalid view and the GPU read past the image (device lost).
    ImageId GetAssociatedDepth(Image& image, vk::Format view_format) {
        if (!image.depth_id) {
            return {};
        }
        switch (view_format) {
        case vk::Format::eS8Uint:
        case vk::Format::eR8Uint:
        case vk::Format::eR8Unorm:
            break;
        default:
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// bbport: a bit per hashed address of every registered metadata surface. Every compute
    /// dispatch asks IsMeta/ClearMeta for each of its buffers (U3's skinning: ~8 per dispatch,
    /// ~2.4M a second in a crowd) and nearly all miss; a clear bit answers without the map.
    /// Bits are only set on registration and rebuilt after removals. BB_META_FILTER=0: off.
    static u32 MetaFilterBit(VAddr address) {
        return u32((address >> 8) * 0x9E3779B97F4A7C15ull >> 52);
    }
    bool MaybeMeta(VAddr address) const {
        static const bool on = [] {
            const char* env = std::getenv("BB_META_FILTER");
            return !(env && env[0] == '0');
        }();
        return !on || meta_filter.test(MetaFilterBit(address));
    }
    /// Registrations run on the GPU thread; a deletion (also from UnmapMemory on a guest thread)
    /// only marks the filter stale, and the rebuild happens here, before the next bit is set: a
    /// rebuild racing a registration lost that bit (review 2026-11-01). Until then the filter
    /// keeps extra bits, which only costs a map lookup. Called with meta_mutex held.
    void NoteMeta(VAddr address) {
        if (meta_filter_stale.load(std::memory_order_relaxed) &&
            meta_filter_stale.exchange(false, std::memory_order_acq_rel)) {
            RebuildMetaFilter();
        }
        meta_filter.set(MetaFilterBit(address));
    }
    /// bbport: metadata clears so far. A draw that reuses the previous draw's render state
    /// (Rasterizer::BeginRendering's memo) does not look at the clear masks: a clear made in
    /// between without recording anything (a DMA fill) was skipped, and the draw went on with the
    /// old contents (code review 2026-11-01). The memo holds only while this is unchanged.
    [[nodiscard]] u64 MetaClears() const noexcept {
        return meta_clears.load(std::memory_order_relaxed);
    }
    void MarkMetaFilterStale() {
        meta_filter_stale.store(true, std::memory_order_release);
    }
    void RebuildMetaFilter() {
        // Built aside and copied in: a reader on another thread (the texture helper) sees each
        // word old or new, never cleared for a surface still registered.
        std::bitset<4096> rebuilt;
        for (const auto& [address, meta] : surface_metas) {
            rebuilt.set(MetaFilterBit(address));
        }
        meta_filter = rebuilt;
    }

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        if (!MaybeMeta(address)) {
            return std::nullopt;
        }
        std::scoped_lock lock{meta_mutex};
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        // Every draw asks this and TouchMeta for each target, mostly with no metadata surface
        // (address 0 or a clear filter bit): those answer without the lock (slow CPU A/B
        // 2026-11-01: -2.3% with four locked lookups per draw on the recording thread).
        if (!address || !MaybeMeta(address)) {
            return false;
        }
        std::scoped_lock lock{meta_mutex};
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        if (!MaybeMeta(address) && !HtileLogOn()) {
            return false;
        }
        std::scoped_lock lock{meta_mutex};
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            meta_clears.fetch_add(1, std::memory_order_relaxed);
            if (HtileLogOn()) {
                static std::unordered_set<VAddr> seen_matched;
                if (seen_matched.insert(address).second) {
                    HtileLog("meta clear %#llx: matched\n", static_cast<unsigned long long>(address));
                }
            }
            return true;
        }
        if (HtileLogOn()) {
            // The registered metadata surface just below the address, if any (a clear of part of one).
            VAddr below = 0;
            for (const auto& [meta_address, meta] : surface_metas) {
                if (meta_address < address && meta_address > below) {
                    below = meta_address;
                }
            }
            static std::unordered_set<VAddr> seen_missed;
            if (below && address - below < 0x4000000 && seen_missed.insert(address).second) {
                HtileLog("meta clear %#llx: not registered, %#llx bytes past the surface at %#llx\n",
                         static_cast<unsigned long long>(address),
                         static_cast<unsigned long long>(address - below),
                         static_cast<unsigned long long>(below));
            }
        }
        return false;
    }

    /// bbport diagnosis: BB_HTILE_LOG=1 prints depth metadata clears and layered depth binds
    /// (first 400 lines).
    static bool HtileLogOn() {
        static const bool on = std::getenv("BB_HTILE_LOG") != nullptr;
        return on;
    }
    template <typename... Args>
    static void HtileLog(const char* format, Args... args) {
        static std::atomic<int> lines{0};
        if (HtileLogOn() && lines.fetch_add(1) < 3000) {
            std::printf(format, args...);
        }
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        if (!address || !MaybeMeta(address)) {
            return false; // see IsMetaCleared
        }
        std::scoped_lock lock{meta_mutex};
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    /// bbport (BB_VRAM_STATS): the cached images' video memory by kind (GPU thread).
    struct ImageStats {
        u64 target_bytes = 0, target_count = 0;   ///< used as a render target or depth buffer
        u64 texture_bytes = 0, texture_count = 0; ///< the others
        u64 gpu_written_bytes = 0;                ///< of both: GPU-modified, only in video memory
        u64 pending_free_bytes = 0;               ///< deleted, not destroyed yet
    };
    ImageStats GetImageStats();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);
    /// bbport: see Image::watch_holes. PunchWatchHole returns false when the whole image has to be
    /// untracked instead.
    bool PunchWatchHole(Image& image, VAddr addr);
    void RestoreWatchHoles(Image& image);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    std::unordered_set<ImageId> small_download_images; ///< bbport: ReadbackSmallTargets
    u64 total_used_memory = 0;
    u64 registered_image_bytes = 0; ///< bbport: guest bytes of registered images (diagnostics)
    bool tight_budget = false;                  ///< bbport: card under ~5 GB
    std::atomic<u64> pending_free_bytes{0};     ///< bbport: deleted, not yet destroyed
    u64 gc_evictions = 0, gc_downloads = 0; ///< bbport: pressure report
    std::chrono::steady_clock::time_point gc_report_time{};
    u64 gc_kept = 0, gc_kept_bytes = 0; ///< bbport: old GPU-written tiled images kept by one walk
    /// bbport: gc_tick at each of the last 64 seconds (GarbageCollectImages).
    std::array<u64, 64> gc_tick_at_second{};
    u64 gc_second = 0;
    /// bbport: gc_tick at the start of each 50 ms slot (BB_GC_MIN_AGE_MS: images used within
    /// that time stay under memory pressure).
    std::array<u64, 128> gc_tick_at_slot{};
    u64 gc_slot = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    const bool readback_linear_images;
    PageTable page_table;
    std::mutex mutex;
    // bbport: FindImage results for unchanged image registrations (guarded by `mutex`).
    struct FindImageCacheEntry {
        VAddr address = 0;
        u64 size = 0;
        Extent3D extent{};
        vk::Format format{};
        AmdGpu::ImageType type{};
        bool exact_fmt = false;
        BindingType binding{};
        u32 levels = 0;
        u32 layers = 0;
        u64 generation = ~0ULL;
        ImageId image_id{};
        int view_mip = -1;
        int view_slice = -1;
    };
    std::array<FindImageCacheEntry, 1024> find_image_cache{};
    std::atomic<u64> registry_generation{0};
    u32 button_prompt_switches = 0;
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    /// perf10: set with the mutex held when an image is queued for download; lets every
    /// fence (ProcessDownloadImages, ~300k/s in U3's crowd) skip the mutex when none is.
    std::atomic<bool> downloads_queued{false};
    /// bbport: small-target read-backs queued to land after the GPU (DownloadImageMemory without
    /// sync). UnmapMemory cancels those in its range, and a landing writes under the mutex, so a
    /// VA mapped again never gets the old target's bytes (code review A4).
    struct SmallReadback {
        VAddr addr;
        u64 size;
        u64 id;
        bool cancelled;
    };
    std::mutex small_readback_mutex;
    std::vector<SmallReadback> small_readbacks;
    u64 small_readback_id = 0;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
        /// bbport: the depth image that last bound this HTILE surface; only it erases the record
        /// when deleted (an older image over the same memory doesn't).
        ImageId owner{};
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
    /// bbport: guards surface_metas. Deletions run on guest threads (UnmapMemory -> FreeImage),
    /// registrations and lookups on the GPU thread; a lookup or the filter rebuild racing an
    /// erase read a map being moved under it. Dispatches mostly stop at the filter first.
    mutable std::mutex meta_mutex;
    std::bitset<4096> meta_filter; ///< see MaybeMeta
    std::atomic<bool> meta_filter_stale{false};
    std::atomic<u64> meta_clears{0}; ///< see MetaClears ///< a deletion since the last rebuild (NoteMeta)
    /// bbport: most layers, and most bytes, a request for a depth array at an address asked for
    /// (PadDepthLayers).
    struct DepthArraySeen {
        u32 layers = 0;
        u64 bytes = 0;
    };
    tsl::robin_map<VAddr, DepthArraySeen> depth_array_layers;
    void PadDepthLayers(ImageInfo& info, const ImageInfo& requested);
};

} // namespace VideoCore
