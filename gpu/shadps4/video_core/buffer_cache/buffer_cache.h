// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>

#include "bbport_copy.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

/// bbport diagnosis (BB_ASYNC_TRACE=1): a GPU-side access to guest memory; uploads = it copies
/// guest data to the GPU.
void NoteAsyncAccess(const char* kind, VAddr address, u64 size, bool written, bool uploads);
/// bbport: BB_ASYNC_PAUSE=2 and the windows of async side queue read-backs in flight.
bool AsyncPauseTargeted();
bool AsyncWindowsInFlight();
bool AsyncWindowOverlaps(VAddr address, u64 size);

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// bbport: guest memory may have changed (a write in the command stream, a CPU write the
    /// page tracking saw, a wait for the CPU): read-only ranges are streamed again.
    void ForgetStreamCopies() noexcept {
        stream_memo_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport diagnosis (BB_FORCE_VB_UPLOAD): the next ObtainBuffer copies this range from guest
    /// memory again (regions the GPU did not write).
    void ForceUpload(VAddr device_addr, u64 size);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// bbport: GPU thread. Copies the GPU's `size` bytes at `device_addr` to staging without
    /// waiting; CompleteReadBacks writes them into guest memory once that work has run (the
    /// tracker state stays: for data the guest only reads, such as an auto-exposure value).
    void ReadBackLater(VAddr device_addr, u32 size);
    void CompleteReadBacks();

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false,
                                                             u64 stream_limit = STREAM_THRESHOLD);

    /// bbport: the 64-dword copy of the game's mesh raycasters moves GPU results into a record
    /// array the guest CPU reads. Instead of waiting for the GPU at the dispatch, the guest part
    /// of the copy is made now and the GPU-written bytes of the source land on a worker thread
    /// once the GPU work that wrote them has completed; the labels the guest learns of the GPU's
    /// progress from wait for it (landing_gate.h). False when not deferred (no side queue,
    /// BB_SCALAR_COPY64_DEFER=0): the caller reads back as before.
    bool DeferGuestCopy(VAddr src, VAddr dst, u64 size);
    u64 PendingLanding() const;
    bool LandingGateActive() const;
    void WaitLanding(u64 seq);
    void RunAfterLanding(u64 seq, std::function<void()> fn);
    /// Ends the landing thread: nothing more is deferred, every wait on it lets go, what is
    /// pending is dropped (~BufferCache; idempotent).
    void StopLanding();

    /// bbport diagnosis (BB_STABLE_CHECK=1): at an end-of-pipe fence, reports read-only
    /// bindings since the last one whose guest data changed meanwhile.
    void CheckStableBindings();

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// bbport: whether buffers the GPU wrote cover any byte of the range. The page tracker
    /// (IsRegionGpuModified) flags whole 4 KiB pages, and so the bytes next to GPU data.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, size_t size) const {
        return gpu_modified_ranges.Intersects(addr, size);
    }

    /// bbport: writes the image that starts at `addr` (its current GPU contents, tiled as in
    /// guest memory) into the buffer of that range, before a shader copies the range as plain
    /// memory. False when no image starts there.
    bool SyncImageToBuffer(VAddr addr, u32 size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport (VRAM give-back, after BilalReloaded's port, GPL): once the arenas hold more than
    /// their limit, gives back the memory of 1 MiB windows unused for a while (GPU thread, from
    /// Rasterizer::OnSubmit; at most every 2 s). See buffer_cache.cpp.
    void ReclaimArenaMemory();

    /// bbport: the arenas' guest and GPU address ranges and the resident block ranges (GPU
    /// crash report).
    void DumpArenas() const;

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    /// bbport: read-only ranges streamed into the command buffer being recorded (ObtainBuffer).
    /// U2's skinning dispatches (~1400 a frame) bind the same mesh data over and over: each bind
    /// copied it again. An entry is reused while the tick (command buffer), the stream buffer's
    /// lap and the epoch (ForgetStreamCopies) are unchanged.
    struct StreamMemo {
        VAddr address;
        u32 size;
        u64 offset;
        u64 tick;
        u64 lap;
        u64 epoch;
    };
    std::array<StreamMemo, 256> stream_memo{};
    std::atomic<u64> stream_memo_epoch{1};
    Buffer gds_buffer;
    /// bbport: the GPU-written ranges, read by the GPU command thread (resource table walks,
    /// SyncSrtReads) while the draw recording thread changes them (bindings that write, and
    /// since fault resolves run there, read-backs and landings). Unlocked, a walk iterating
    /// the tree while it was rebalanced looped forever (U3, chapter 1's pub: the game froze).
    class LockedRangeSet {
    public:
        void Add(VAddr address, size_t size) {
            std::unique_lock lock{mutex};
            set.Add(address, size);
        }
        void Subtract(VAddr address, size_t size) {
            std::unique_lock lock{mutex};
            set.Subtract(address, size);
        }
        bool Intersects(VAddr address, size_t size) const {
            std::shared_lock lock{mutex};
            return set.Intersects(address, size);
        }
        bool Contains(VAddr address, size_t size) const {
            std::shared_lock lock{mutex};
            return set.Contains(address, size);
        }
        /// `func` must not change this set (the lock is held).
        template <typename Func>
        void ForEachInRange(VAddr address, size_t size, Func&& func) const {
            std::shared_lock lock{mutex};
            set.ForEachInRange(address, size, std::forward<Func>(func));
        }

    private:
        mutable std::shared_mutex mutex;
        RangeSet set;
    };
    LockedRangeSet gpu_modified_ranges;
    struct LateReadBack {
        Vulkan::StagingBufferRef staging;
        u64 tick;
        VAddr address;
        u32 size;
    };
    std::vector<LateReadBack> late_read_backs; ///< GPU thread only (ReadBackLater)
    /// bbport: GPU data into guest memory (the read-backs), leaving out the heap's own bytes of
    /// blocks U3 freed recently and that are still free (bbgpu_note_guest_free).
    void WriteBackGpuData(u8* dst, const u8* data, u64 size);

    /// bbport: GPU data a guest write lands on, read back on a second queue of the graphics
    /// family before the page becomes accessible. That queue waits on the GPU for the work that
    /// wrote the faulting pages only (Finish waits for the whole queue and the read-back after
    /// it); neighbouring pages come along when their data is complete. False when unavailable.
    struct SideQueue;
    std::unique_ptr<SideQueue> side_queue;
    bool DownloadOnSideQueue(VAddr page, u64 length);

    /// bbport: deferred raycaster copies (DeferGuestCopy).
    struct Landing;
    struct GatedLabel {
        u64 seq;
        std::function<void()> fn;
    };
    std::mutex landing_mutex;
    std::condition_variable landing_cv, landing_done_cv;
    std::deque<std::shared_ptr<Landing>> landing_queue; ///< registered, not yet landed
    std::deque<GatedLabel> gated_labels;                 ///< labels waiting for a landing
    u64 landing_registered_seq{0}, landing_done_seq{0};
    std::atomic<u64> landing_registered{0}, landing_done{0};
    bool landing_stop{false}, gate_running{false};
    std::atomic<u32> gate_outstanding{0}; ///< labels queued or running on the landing thread
    std::thread landing_thread;
    void LandingWorker();
    bool ExecuteLanding(Landing& landing);
    bool WaitLandingTick(u64 tick);
    std::atomic<bool> landing_exited{false};

    /// bbport (BB_SHARED_WRITES=0: off; after BilalReloaded's shared pages): a guest write next to
    /// GPU-written bytes whose GPU work is still running goes ahead at once instead of waiting
    /// for that work (U3's Cartagena square: 86 -> 97 FPS, the GPU thread waited ~3.5 ms for
    /// each of ~50 such writes a second); the GPU bytes land in guest memory once it has
    /// finished (a landing, so the labels written meanwhile wait for them), except where the
    /// guest has rewritten them since, and uploads take only the bytes the guest has changed
    /// there until then. A write onto GPU bytes still being written goes ahead the same way
    /// (once a frame in the square, 3.5 ms each). False: the caller reads back as before.
    bool ShareGpuPage(VAddr page, u64 length);
    /// GPU side: the shared pages whose GPU bytes have landed stop being GPU data.
    void DrainSharedLandings();
    RangeSet shared_gpu_ranges; ///< GPU bytes of shared pages not landed yet
    /// The guest's bytes in each piece of shared_gpu_ranges when its page was shared (by start).
    std::map<VAddr, std::vector<u8>> shared_before;
    struct SharedLanded {
        struct Range {
            VAddr start, end;
            bool guest_kept; ///< the guest had rewritten some of its bytes: they stay
        };
        boost::container::small_vector<Range, 4> ranges;
        u64 tick;
    };
    std::mutex shared_landed_mutex;
    std::vector<SharedLanded> shared_landed;
    std::atomic<bool> shared_landed_pending{false};

    struct SideTicket;
    bool BeginSideDownload(VAddr page, u64 length, SideTicket& ticket);
    void WaitSideDownload(SideTicket& ticket);
    bool FinishSideDownload(SideTicket& ticket, VAddr fault_page, u64 fault_length);

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset; ///< into `memory`, in blocks (like start and end: SubRange and CanMergeWith
                    ///< mixed bytes and blocks and gave a release misaligned memory offsets)
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    /// bbport (ReclaimArenaMemory): the 2 s epoch in which each 1 MiB window of guest GPU memory
    /// was last used through an arena (0: never), the scheduler tick at the start of each recent
    /// epoch, and arena memory given back: bound again before new memory is allocated, held back
    /// until the unbinds are submitted.
    static constexpr u32 ReclaimWindowBits = 20;
    static constexpr u32 ReclaimEpochRing = 64;
    std::unique_ptr<std::atomic<u16>[]> window_epoch{
        new std::atomic<u16>[u64(1) << (ADDRESS_SPACE_BITS - ReclaimWindowBits)]{}};
    u16 reclaim_epoch = 1;
    std::chrono::steady_clock::time_point next_reclaim_epoch{};
    std::array<u64, ReclaimEpochRing> epoch_start_tick{};
    struct FreeRun {
        vk::DeviceMemory memory;
        u64 offset; ///< bytes into `memory`
        u64 blocks;
    };
    std::vector<FreeRun> free_runs, quarantined_runs;
    /// Unbinds (and their memory) of ranges released while the command buffer with scheduler
    /// tick `tick` was recorded: held until the GPU has finished that tick (see ReleaseArenaBlocks).
    struct DeferredUnbinds {
        u64 tick;
        std::vector<ArenaBinds> binds;
        std::vector<Backing> pieces; ///< their memory, still bound there until the unbinds
    };
    std::vector<DeferredUnbinds> deferred_unbinds;
    u64 deferred_unbinds_submitted = 0;
    /// Windows on probation before they are given back: their page table entries carry the watch
    /// bit, so a shader reading them through a pointer reports them (and they are used again).
    struct Probation {
        u64 window;
        u16 since;      ///< reclaim epoch the watch started
        u16 last_epoch; ///< window_epoch then
        u64 blocks;     ///< resident blocks then
    };
    std::vector<Probation> probation;
    u64 probation_rescued = 0;
    /// Rewrites the page table entries of the resident blocks in [first_block, end_block), with
    /// the watch bit (bit 0) or without; returns the resident blocks.
    u64 WatchArenaBlocks(u64 first_block, u64 end_block, bool watch);
    u64 resident_in_use_blocks = 0;
    bool has_pending_unbinds = false;
    /// Moves deferred unbinds whose tick the GPU has finished into pending_binds.
    void QueueFinishedUnbinds();
    void TouchArenaWindows(u64 first_block, u64 last_block) {
        const u16 epoch = reclaim_epoch;
        const u64 last = std::min<u64>(((last_block << block_shift) >> ReclaimWindowBits),
                                       (u64(1) << (ADDRESS_SPACE_BITS - ReclaimWindowBits)) - 1);
        for (u64 w = (first_block << block_shift) >> ReclaimWindowBits; w <= last; ++w) {
            if (window_epoch[w].load(std::memory_order_relaxed) != epoch) {
                window_epoch[w].store(epoch, std::memory_order_relaxed);
            }
        }
    }
    /// Releases the resident blocks in [first_block, end_block); returns how many.
    u64 ReleaseArenaBlocks(u64 first_block, u64 end_block);
    /// Drops pending unbinds of `arena` inside [first_block, end_block) (bound again).
    void TrimPendingUnbinds(const Buffer* arena, u64 first_block, u64 end_block);
    /// bbport: arena memory chunks (see AllocateArenaMemory).
    struct ArenaChunk {
        vk::DeviceMemory memory;
        u64 size;
        u64 used;
        bool host;
    };
    std::vector<ArenaChunk> arena_chunks;
    bool host_arena_active{};
    u64 AllocateArenaMemory(u64 size, vk::DeviceMemory& memory);

    u32 arena_memory_type_index{};
    u32 arena_host_memory_type_index{~0u}; ///< bbport: system memory fallback
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
