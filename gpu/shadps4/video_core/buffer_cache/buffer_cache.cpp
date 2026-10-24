// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>
#include <array>
#include <map>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include <xxhash.h>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "common/thread.h"
#include "video_core/landing_gate.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>
#include <cstdio>
#include <cstring>
// src/runtime_memory.c: guest memory through the backing mapping (no protection faults).
extern "C" int runtime_memory_read_backing(uintptr_t address, void* out, uint64_t size);
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
extern void (*g_gpu_crash_context)();
}

extern "C" char __ImageBase; // the executable's load address (PE linkers define it)

namespace Vulkan {
extern void (*g_drain_draw_pipe)(); // vk_scheduler.cpp
}

namespace VideoCore {

namespace {
// bbport: deferred raycaster copies (landing_gate.h). The instance is cleared and the stopping
// flag set first thing in ~BufferCache: every wait on the landing thread is released then.
std::atomic<BufferCache*> land_instance{nullptr};
std::atomic<bool> land_stopping{false};
} // namespace

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

struct BufferCache::SideQueue {
    static constexpr u64 StagingSize = 4_MB;
    /// Read-backs in flight at once: guest threads wait for theirs while the GPU thread goes on
    /// (BeginSideDownload / WaitSideDownload / FinishSideDownload).
    static constexpr u32 NumSlots = 4;

    struct Slot {
        Slot(const Vulkan::Instance& instance, vk::Device device)
            : staging{instance, 0, StagingSize, MemoryType::HostCached, "Side queue download"} {
            pool = Vulkan::Check(device.createCommandPoolUnique({
                .flags = vk::CommandPoolCreateFlagBits::eTransient,
                .queueFamilyIndex = instance.GetSideQueueFamilyIndex(),
            }));
            cmdbuf = Vulkan::Check(device.allocateCommandBuffers({
                                       .commandPool = *pool,
                                       .level = vk::CommandBufferLevel::ePrimary,
                                       .commandBufferCount = 1,
                                   }))
                         .front();
            fence = Vulkan::Check(device.createFenceUnique({}));
        }
        Buffer staging;
        vk::UniqueCommandPool pool;
        vk::CommandBuffer cmdbuf;
        vk::UniqueFence fence;
        bool busy = false;
    };

    SideQueue(const Vulkan::Instance& instance, vk::Queue queue_)
        : device{instance.GetDevice()}, queue{queue_} {
        for (u32 i = 0; i < NumSlots; ++i) {
            slots.push_back(std::make_unique<Slot>(instance, device));
        }
    }

    Slot* Acquire() {
        std::scoped_lock lk{mutex};
        for (auto& slot : slots) {
            if (!slot->busy) {
                slot->busy = true;
                return slot.get();
            }
        }
        return nullptr;
    }
    void Release(Slot* slot) {
        std::scoped_lock lk{mutex};
        slot->busy = false;
    }

    vk::Device device;
    vk::Queue queue;
    std::vector<std::unique_ptr<Slot>> slots;
    std::mutex mutex;
    std::mutex submit_mutex; ///< the queue: read-backs (GPU thread) and landings (bb:Landing)
};

/// A side queue read-back between its submission (GPU thread) and its completion.
struct BufferCache::SideTicket {
    SideQueue::Slot* slot = nullptr;
    const Buffer* arena = nullptr;
    boost::container::small_vector<VAddr, 16> pages;
    boost::container::small_vector<vk::BufferCopy, 16> copies;
    u64 total = 0;
    u64 write_tick = 0;
    std::chrono::steady_clock::time_point start;
    bool async = false; // BB_ASYNC_TRACE
};

/// bbport: one command buffer's copies of recurring read-back pages (PrefetchRecurringPages).
struct BufferCache::PrefetchBatch {
    static constexpr u64 StagingSize = 2_MB;
    static constexpr u32 NumBatches = 8;

    explicit PrefetchBatch(const Vulkan::Instance& instance)
        : staging{instance, 0, StagingSize, MemoryType::HostCached, "Prefetched read-backs"} {}

    struct Entry {
        VAddr page;
        boost::container::small_vector<std::pair<VAddr, VAddr>, 4> parts; ///< GPU bytes copied
        u64 offset; ///< of the first part in staging; the others follow, 16-byte aligned
    };
    Buffer staging;
    std::vector<Entry> entries;
    u64 tick = 0; ///< the command buffer that copies them (0: unused)
    bool invalidated = false;
};

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const std::array families = {instance.GetGraphicsQueueFamilyIndex(),
                                 instance.GetSideQueueFamilyIndex()};
    const bool shared = families[0] != families[1];
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = shared ? 2u : 0u,
        .pQueueFamilyIndices = shared ? families.data() : nullptr,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    static BufferCache* crash_report_cache;
    crash_report_cache = this;
    Vulkan::g_gpu_crash_context = [] { crash_report_cache->DumpArenas(); };

    // A GPU write marked now is in the command buffer being recorded or, if that is submitted
    // before the command lands, in the next one.
    static Vulkan::Scheduler* write_scheduler;
    write_scheduler = &scheduler;
    g_gpu_write_done_tick = [] { return write_scheduler->CurrentTick() + 1; };
    land_stopping.store(false, std::memory_order_release);
    land_instance.store(this, std::memory_order_release);
    const char* side_env = std::getenv("BB_SIDE_DOWNLOAD");
    if (instance.GetSideQueue() && !(side_env && side_env[0] == '0')) {
        side_queue = std::make_unique<SideQueue>(instance, instance.GetSideQueue());
    }
}

// bbport diagnosis: BB_ASYNC_TRACE=1 — GPU-side accesses (buffer bindings, constant ring copies,
// texture uploads) to pages whose side queue read-back is in flight, and uploads of pages
// shortly after a read-back handed them back (async or synchronous, for comparison).
namespace {
struct TraceRange {
    std::atomic<u64> start{0}, end{0}, time_ns{0};
    std::atomic<bool> async{false};
    std::atomic<bool> touched{false}; // a GPU-side access overlapped it while in flight
};
std::array<TraceRange, 16> trace_in_flight;
std::array<TraceRange, 64> trace_handed_back;
std::atomic<u64> trace_handed_head{0};
std::atomic<u64> trace_counts[7]; // in-flight access, handback upload async/sync, finish skips x3,
                                  // strict discards
std::atomic<u32> in_flight_count{0};
std::atomic<u32> trace_prints{0};

bool AsyncTraceEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_ASYNC_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

u64 TraceNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void TracePrintSummary() {
    static std::atomic<u64> last{0};
    const u64 now = TraceNowNs();
    u64 prev = last.load(std::memory_order_relaxed);
    if (now - prev < 5'000'000'000ull || !last.compare_exchange_strong(prev, now)) {
        return;
    }
    std::printf("Async trace (5 s): %llu in-flight accesses, %llu uploads after async handback, "
                "%llu after sync handback; finish skipped %llu handed back, %llu cpu-modified, "
                "%llu newer GPU write; %llu touched read-backs redone\n",
                (unsigned long long)trace_counts[0].exchange(0),
                (unsigned long long)trace_counts[1].exchange(0),
                (unsigned long long)trace_counts[2].exchange(0),
                (unsigned long long)trace_counts[3].exchange(0),
                (unsigned long long)trace_counts[4].exchange(0),
                (unsigned long long)trace_counts[5].exchange(0),
                (unsigned long long)trace_counts[6].exchange(0));
}

void TraceBegin(VAddr start, VAddr end, bool async) {
    for (auto& r : trace_in_flight) {
        u64 expected = 0;
        if (r.start.compare_exchange_strong(expected, start)) {
            r.touched.store(false);
            r.end.store(end);
            r.async.store(async);
            in_flight_count.fetch_add(1);
            r.time_ns.store(TraceNowNs());
            return;
        }
    }
}

void TraceFinish(VAddr start, std::span<const VAddr> handed, bool async) {
    for (auto& r : trace_in_flight) {
        if (start && r.start.load() == start) {
            r.end.store(0);
            r.start.store(0);
            in_flight_count.fetch_sub(1);
            break;
        }
    }
    const u64 now = TraceNowNs();
    for (const VAddr page : handed) {
        auto& h = trace_handed_back[trace_handed_head.fetch_add(1) % trace_handed_back.size()];
        h.start.store(page);
        h.end.store(page + 4096);
        h.async.store(async);
        h.time_ns.store(now);
    }
}
/// BB_ASYNC_STRICT (default on): an async read-back whose pages a GPU-side access touched while
/// it was in flight is discarded and redone synchronously.
bool AsyncStrict() {
    static const bool strict = [] {
        const char* env = std::getenv("BB_ASYNC_STRICT");
        return !(env && env[0] == '0');
    }();
    return strict;
}

bool TicketTouched(VAddr start) {
    for (auto& r : trace_in_flight) {
        if (start && r.start.load() == start) {
            return r.touched.load();
        }
    }
    return false;
}
} // namespace

/// BB_ASYNC_PAUSE=2: a draw or dispatch reading a window whose async read-back is in flight
/// waits for it (Rasterizer::WaitAsyncWindows).
bool AsyncPauseTargeted() {
    static const bool targeted = [] {
        const char* env = std::getenv("BB_ASYNC_PAUSE");
        return env && env[0] == '2';
    }();
    return targeted;
}

bool AsyncWindowsInFlight() {
    return in_flight_count.load(std::memory_order_relaxed) != 0;
}

bool AsyncWindowOverlaps(VAddr address, u64 size) {
    const VAddr end = address + size;
    for (auto& r : trace_in_flight) {
        const VAddr rs = r.start.load(std::memory_order_acquire);
        if (rs && rs < end && address < r.end.load(std::memory_order_relaxed) &&
            r.async.load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool AsyncAccessTracking() {
    return AsyncTraceEnabled() || in_flight_count.load(std::memory_order_relaxed) != 0;
}

/// Called for GPU-side accesses to guest memory (kind names the path).
void NoteAsyncAccess(const char* kind, VAddr address, u64 size, bool written, bool uploads) {
    const bool trace = AsyncTraceEnabled();
    if (!trace && in_flight_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    if (trace) {
        TracePrintSummary();
    }
    const VAddr end = address + size;
    for (auto& r : trace_in_flight) {
        const VAddr rs = r.start.load(std::memory_order_relaxed);
        if (rs && rs < end && address < r.end.load(std::memory_order_relaxed)) {
            r.touched.store(true);
            trace_counts[0].fetch_add(1);
            if (trace && trace_prints.fetch_add(1) < 300) {
                std::printf("Async trace: %s %#llx+%#llx%s during %s read-back of %#llx-%#llx "
                            "(%.2f ms in) thread '%s'\n",
                            kind, (unsigned long long)address, (unsigned long long)size,
                            written ? " (written)" : "", r.async.load() ? "async" : "sync",
                            (unsigned long long)rs, (unsigned long long)r.end.load(),
                            (TraceNowNs() - r.time_ns.load()) / 1e6,
                            Common::GetCurrentThreadName().c_str());
            }
        }
    }
    if (!uploads || !trace) {
        return;
    }
    const u64 now = TraceNowNs();
    for (auto& h : trace_handed_back) {
        const VAddr hs = h.start.load(std::memory_order_relaxed);
        const u64 t = h.time_ns.load(std::memory_order_relaxed);
        if (hs && hs < end && address < h.end.load(std::memory_order_relaxed) &&
            now - t < 5'000'000ull) {
            const bool async = h.async.load();
            trace_counts[async ? 1 : 2].fetch_add(1);
            if (async && trace_prints.fetch_add(1) < 300) {
                std::printf("Async trace: %s %#llx+%#llx uploads page %#llx %.2f ms after its "
                            "async handback, thread '%s'\n",
                            kind, (unsigned long long)address, (unsigned long long)size,
                            (unsigned long long)hs, (now - t) / 1e6,
                            Common::GetCurrentThreadName().c_str());
            }
            return;
        }
    }
}

/// bbport: guest threads waiting for an async side queue read-back (BB_ASYNC_PAUSE: the GPU
/// thread decodes no draw or dispatch meanwhile, see Liverpool).
std::atomic<u32> g_async_stalled{0};

bool BufferCache::DownloadOnSideQueue(VAddr page, u64 length) {
    SideTicket ticket;
    // bbport diagnosis (BB_PERF_DIAG=1): where a synchronous read-back's time goes. U3's
    // Cartagena square: ~78% find the writing work done (~50 us); the rest wait ~4 ms for the GPU
    // to catch up, nearly all on two regions: GDS counters copied next to records the guest
    // writes (and then reads the counters: skipping the wait crashed it), and a ring of mixed
    // guest/GPU blocks whose pages move every frame.
    static const bool diag = [] {
        const char* env = std::getenv("BB_PERF_DIAG");
        return env && env[0] == '1';
    }();
    if (TryPrefetched(page, length)) {
        return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const u64 tick_before = scheduler.CurrentTick();
    if (!BeginSideDownload(page, length, ticket)) {
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const bool was_done = ticket.write_tick == 0 || scheduler.IsFree(ticket.write_tick);
    const u64 flushes = scheduler.CurrentTick() - tick_before;
    WaitSideDownload(ticket);
    const auto t2 = std::chrono::steady_clock::now();
    const u64 pages = ticket.pages.size(), bytes = ticket.total;
    const bool ok = FinishSideDownload(ticket, page, length); // nothing came in between here
    if (ok && was_done) {
        NotePrefetchCandidate(page, length);
    }
    if (diag) {
        const auto t3 = std::chrono::steady_clock::now();
        static u64 n = 0, done = 0, flush_count = 0, page_count = 0, byte_count = 0;
        static double begin_us = 0, wait_us = 0, wait_done_us = 0, finish_us = 0;
        static auto window = t3;
        const auto us = [](auto d) { return std::chrono::duration<double, std::micro>(d).count(); };
        ++n;
        done += was_done;
        flush_count += flushes;
        page_count += pages;
        byte_count += bytes;
        begin_us += us(t1 - t0);
        wait_us += us(t2 - t1);
        if (was_done) {
            wait_done_us += us(t2 - t1);
        }
        finish_us += us(t3 - t2);
        if (t3 - window >= std::chrono::seconds(5)) {
            std::printf("Sync read-back breakdown: %llu, writing work already done for %.0f%% "
                        "(wait %.0f us then), %.2f flushes each; begin %.0f us, wait %.0f us, "
                        "store %.0f us; %.1f pages, %.1f KiB each\n",
                        (unsigned long long)n, 100.0 * done / n,
                        done ? wait_done_us / done : 0.0, double(flush_count) / n, begin_us / n,
                        wait_us / n, finish_us / n, double(page_count) / n,
                        byte_count / 1024.0 / n);
            n = done = flush_count = page_count = byte_count = 0;
            begin_us = wait_us = wait_done_us = finish_us = 0;
            window = t3;
        }
    }
    return ok;
}

/// GPU thread: chooses the pages and submits their copy on the side queue (which waits there
/// for the work that wrote them). False: nothing submitted, the caller takes another path.
bool BufferCache::BeginSideDownload(VAddr page, u64 length, SideTicket& ticket) {
    if (!side_queue) {
        return false;
    }
    ticket.start = std::chrono::steady_clock::now();
    // The pages around the fault whose GPU data is complete come along: the guest fills its
    // rings page after page, and each would fault and wait for a round trip of its own.
    // BB_SIDE_WINDOW_KB (diagnosis): the window size, 4 = the faulting page only.
    static const u64 Window = [] {
        const char* env = std::getenv("BB_SIDE_WINDOW_KB");
        const u64 kb = env ? std::strtoull(env, nullptr, 10) : 64;
        return std::max<u64>(kb, 4) * 1_KB;
    }();
    const u64 first_block = page >> block_shift;
    const u64 last_block = (page + length - 1) >> block_shift;
    const Buffer* arena = GetArena(first_block, last_block);
    ticket.arena = arena;
    const VAddr window_start = std::max(Common::AlignDown(page, Window), arena->cpu_addr);
    const VAddr window_end = std::min(
        std::max(Common::AlignDown(page, Window) + Window, page + length),
        arena->cpu_addr + arena->size_bytes);
    auto& pages = ticket.pages;
    u64& write_tick = ticket.write_tick;
    {
        for (VAddr at = window_start; at < window_end; at += TRACKER_BYTES_PER_PAGE) {
            const bool faulting = at >= page && at < page + length;
            if (!memory_tracker->IsRegionGpuModified(at, TRACKER_BYTES_PER_PAGE)) {
                continue;
            }
            const u64 tick = memory_tracker->LatestGpuWriteTick(at, TRACKER_BYTES_PER_PAGE);
            // The faulting pages are read back whatever their GPU work's state (the side queue
            // waits for it); neighbours only when complete, and not while the guest writes them.
            const bool usable = faulting || (scheduler.IsFree(tick) &&
                                             !memory_tracker->IsRegionCpuModified(
                                                 at, TRACKER_BYTES_PER_PAGE));
            if (!usable) {
                if (faulting) {
                    return false;
                }
                continue;
            }
            pages.push_back(at);
            write_tick = std::max(write_tick, tick);
        }
    }
    auto& copies = ticket.copies;
    u64& total = ticket.total;
    for (const VAddr at : pages) {
        gpu_modified_ranges.ForEachInRange(at, TRACKER_BYTES_PER_PAGE, [&](VAddr start, VAddr end) {
            if (!copies.empty() &&
                copies.back().srcOffset + copies.back().size == start - arena->cpu_addr &&
                copies.back().dstOffset + copies.back().size == total) {
                copies.back().size += end - start;
                total += end - start;
                return;
            }
            total = Common::AlignUp(total, 64);
            copies.push_back(vk::BufferCopy{
                .srcOffset = start - arena->cpu_addr,
                .dstOffset = total,
                .size = end - start,
            });
            total += end - start;
        });
    }
    if (total > SideQueue::StagingSize) {
        return false;
    }
    if (total == 0) {
        return true; // nothing to copy: Finish only updates the tracking
    }
    // The side queue waits on the timeline for the writing work, which must be submitted to
    // signal it: a write is marked for the command buffer being recorded or the next one
    // (g_gpu_write_done_tick), so up to two flushes. Nothing waits for the main queue.
    for (int flushes = 0; scheduler.CurrentTick() <= write_tick; ++flushes) {
        if (flushes == 2) {
            return false; // not a tick of this scheduler: the read-back path waits instead
        }
        scheduler.Flush();
    }
    auto& side = *side_queue;
    auto* slot = side.Acquire();
    if (!slot) {
        return false; // every slot in flight: the caller reads back synchronously
    }
    ticket.slot = slot;
    Vulkan::Check(side.device.resetCommandPool(*slot->pool));
    Vulkan::Check(slot->cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    slot->cmdbuf.copyBuffer(arena->Handle(), slot->staging.Handle(), copies);
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    slot->cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    Vulkan::Check(slot->cmdbuf.end());
    // Waiting on the timeline orders the writing work's writes before the copy.
    const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline_info = {
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &write_tick,
    };
    const vk::SubmitInfo submit = {
        .pNext = &timeline_info,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &timeline,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->cmdbuf,
    };
    {
        std::scoped_lock submit_lock{side.submit_mutex};
        Vulkan::Check(side.queue.submit(submit, *slot->fence));
    }
    if ((AsyncTraceEnabled() || (ticket.async && (AsyncStrict() || AsyncPauseTargeted()))) &&
        !pages.empty()) {
        TraceBegin(pages.front(), pages.back() + TRACKER_BYTES_PER_PAGE, ticket.async);
    }
    return true;
}

/// Any thread: waits for the copy. Storing it in guest memory is FinishSideDownload's: only it
/// knows which pages are still the GPU's (another path may have handed one back meanwhile, and
/// the guest may have written it since; storing the older GPU data over that left stale vertex
/// data: spikes and tears in U3's pub).
void BufferCache::WaitSideDownload(SideTicket& ticket) {
    auto* slot = ticket.slot;
    if (!slot) {
        return;
    }
    Vulkan::Check(side_queue->device.waitForFences(*slot->fence, true, UINT64_MAX));
    Vulkan::Check(side_queue->device.resetFences(*slot->fence));
    slot->staging.Invalidate(0, ticket.total);
}

/// GPU thread: stores the read-back of the pages that are still the GPU's and hands them back.
/// A page the GPU wrote again after the submission keeps its GPU data; if that is a page the
/// fault needs, false: the caller reads back again, synchronously. A page handed back by
/// another path meanwhile is left alone (the guest's data there is newer).
bool BufferCache::FinishSideDownload(SideTicket& ticket, VAddr fault_page, u64 fault_length) {
    boost::container::small_vector<VAddr, 16> owned;
    bool fault_ok = true;
    const bool discard = ticket.async && AsyncStrict() && ticket.slot && !ticket.pages.empty() &&
                         TicketTouched(ticket.pages.front());
    if (discard) {
        trace_counts[6].fetch_add(1, std::memory_order_relaxed);
        fault_ok = false;
    }
    for (const VAddr at : discard ? std::span<const VAddr>{} :
                                    std::span<const VAddr>(ticket.pages.data(), ticket.pages.size())) {
        if (!memory_tracker->IsRegionGpuModified(at, TRACKER_BYTES_PER_PAGE)) {
            trace_counts[3].fetch_add(1, std::memory_order_relaxed);
            continue; // handed back meanwhile
        }
        const bool faulting = at >= fault_page && at < fault_page + fault_length;
        if (!faulting && memory_tracker->IsRegionCpuModified(at, TRACKER_BYTES_PER_PAGE)) {
            trace_counts[4].fetch_add(1, std::memory_order_relaxed);
            continue; // a neighbour with guest data waiting for upload: not overwritten
        }
        if (memory_tracker->LatestGpuWriteTick(at, TRACKER_BYTES_PER_PAGE) > ticket.write_tick) {
            trace_counts[5].fetch_add(1, std::memory_order_relaxed);
            if (at >= fault_page && at < fault_page + fault_length) {
                fault_ok = false;
            }
            continue; // newer GPU data than the copy
        }
        owned.push_back(at);
    }
    // bbport diagnosis: BB_ASYNC_VERIFY=1 — before an async read-back hands pages back, a second
    // copy of them, taken after all work recorded so far, is compared with it: a difference is
    // GPU data the tracking missed (stale data about to reach the guest).
    static const bool verify = [] {
        const char* env = std::getenv("BB_ASYNC_VERIFY");
        return env && env[0] == '1';
    }();
    if (verify && ticket.async && ticket.slot && ticket.total && !owned.empty()) {
        if (auto* check = side_queue->Acquire()) {
            scheduler.Flush();
            const u64 wait_tick = scheduler.CurrentTick() - 1;
            auto& side = *side_queue;
            Vulkan::Check(side.device.resetCommandPool(*check->pool));
            Vulkan::Check(
                check->cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
            check->cmdbuf.copyBuffer(ticket.arena->Handle(), check->staging.Handle(),
                                     ticket.copies);
            const vk::MemoryBarrier2 to_host = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            };
            check->cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
            Vulkan::Check(check->cmdbuf.end());
            const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
            const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
            const vk::TimelineSemaphoreSubmitInfo timeline_info = {
                .waitSemaphoreValueCount = 1,
                .pWaitSemaphoreValues = &wait_tick,
            };
            const vk::SubmitInfo submit = {
                .pNext = &timeline_info,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &timeline,
                .pWaitDstStageMask = &wait_stage,
                .commandBufferCount = 1,
                .pCommandBuffers = &check->cmdbuf,
            };
            {
                std::scoped_lock submit_lock{side.submit_mutex};
                Vulkan::Check(side.queue.submit(submit, *check->fence));
            }
            Vulkan::Check(side.device.waitForFences(*check->fence, true, UINT64_MAX));
            Vulkan::Check(side.device.resetFences(*check->fence));
            check->staging.Invalidate(0, ticket.total);
            const u8* first = ticket.slot->staging.mapped_data.data();
            const u8* second = check->staging.mapped_data.data();
            const bool touched = TicketTouched(ticket.pages.front());
            static std::atomic<u64> checked{0}, stale{0};
            static std::atomic<u32> prints{0};
            for (const auto& copy : ticket.copies) {
                const VAddr start = ticket.arena->cpu_addr + copy.srcOffset;
                const VAddr end = start + copy.size;
                for (VAddr at = start; at < end;) {
                    const VAddr page = Common::AlignDown(at, TRACKER_BYTES_PER_PAGE);
                    const VAddr part_end = std::min<VAddr>(end, page + TRACKER_BYTES_PER_PAGE);
                    if (std::ranges::find(owned, page) != owned.end()) {
                        const u64 offset = copy.dstOffset + (at - start);
                        u64 differ = 0, first_diff = ~0ull;
                        for (u64 i = 0; i < part_end - at; ++i) {
                            if (first[offset + i] != second[offset + i]) {
                                ++differ;
                                first_diff = std::min(first_diff, i);
                            }
                        }
                        checked.fetch_add(1, std::memory_order_relaxed);
                        if (differ) {
                            stale.fetch_add(1, std::memory_order_relaxed);
                            if (prints.fetch_add(1, std::memory_order_relaxed) < 400) {
                                const bool faulting = page >= fault_page &&
                                                      page < fault_page + fault_length;
                                std::printf(
                                    "Async verify: STALE %s page %#llx: %llu of %llu bytes differ "
                                    "from %#llx, touched %d, write tick %llu, page tick now %llu, "
                                    "%.2f ms after submit (%llu stale of %llu pages so far)\n",
                                    faulting ? "fault" : "neighbour",
                                    (unsigned long long)page, (unsigned long long)differ,
                                    (unsigned long long)(part_end - at),
                                    (unsigned long long)(at + first_diff), int(touched),
                                    (unsigned long long)ticket.write_tick,
                                    (unsigned long long)memory_tracker->LatestGpuWriteTick(
                                        page, TRACKER_BYTES_PER_PAGE),
                                    std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - ticket.start)
                                        .count(),
                                    (unsigned long long)stale.load(),
                                    (unsigned long long)checked.load());
                            }
                        }
                    }
                    at = part_end;
                }
            }
            side_queue->Release(check);
        }
    }
    if (auto* slot = ticket.slot; slot && ticket.total) {
        const u8* staging = slot->staging.mapped_data.data();
        for (const auto& copy : ticket.copies) {
            // A copy can span pages: store the parts in the pages still owned.
            const VAddr start = ticket.arena->cpu_addr + copy.srcOffset;
            const VAddr end = start + copy.size;
            for (VAddr at = start; at < end;) {
                const VAddr page = Common::AlignDown(at, TRACKER_BYTES_PER_PAGE);
                const VAddr part_end = std::min<VAddr>(end, page + TRACKER_BYTES_PER_PAGE);
                if (std::ranges::find(owned, page) != owned.end()) {
                    WriteBackGpuData(std::bit_cast<u8*>(at), staging + copy.dstOffset + (at - start),
                                     part_end - at);
                }
                at = part_end;
            }
        }
    }
    for (const VAddr at : owned) {
        gpu_modified_ranges.Subtract(at, TRACKER_BYTES_PER_PAGE);
        memory_tracker->UnmarkRegionAsGpuModified(at, TRACKER_BYTES_PER_PAGE);
    }
    if (AsyncAccessTracking()) {
        TraceFinish(ticket.slot && !ticket.pages.empty() ? ticket.pages.front() : 0,
                    std::span<const VAddr>(owned.data(), owned.size()), ticket.async);
    }
    if (ticket.slot) {
        side_queue->Release(ticket.slot);
        ticket.slot = nullptr;
    }
    BbStats::gpu_side_download_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             ticket.start)
            .count(),
        std::memory_order_relaxed);
    return fault_ok;
}

static bool PrefetchEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PREFETCH_READBACK");
        return env && env[0] == '1';
    }();
    return enabled;
}

static std::atomic<u64> prefetch_served{0}, prefetch_missed{0}, prefetch_copied_pages{0},
    prefetch_copied_bytes{0};

/// GPU thread, after a side queue read-back of these pages: they count as recurring once seen
/// twice within a few seconds.
void BufferCache::NotePrefetchCandidate(VAddr page, u64 length) {
    if (!PrefetchEnabled()) {
        return;
    }
    std::scoped_lock lock{prefetch_mutex};
    for (VAddr at = page; at < page + length; at += TRACKER_BYTES_PER_PAGE) {
        if (prefetch_hits.size() >= 4096 && !prefetch_hits.contains(at)) {
            break;
        }
        auto& hits = prefetch_hits[at];
        hits = static_cast<u8>(std::min<u32>(hits + 1u, 8u));
    }
}

/// Draw scheduler submission (Rasterizer::OnSubmit, the command buffer still open): copies the
/// GPU bytes of the recurring pages written by this command buffer (or before it, not copied
/// yet) to a staging buffer after all its work.
void BufferCache::PrefetchRecurringPages() {
    if (!PrefetchEnabled()) {
        return;
    }
    static const bool diag = [] {
        const char* env = std::getenv("BB_PERF_DIAG");
        return env && env[0] == '1';
    }();
    std::scoped_lock lock{prefetch_mutex};
    const auto now = std::chrono::steady_clock::now();
    if (now >= prefetch_decay_at) {
        // Every 2 s the counts halve: pages that stopped faulting drop out.
        prefetch_decay_at = now + std::chrono::seconds(2);
        std::erase_if(prefetch_hits, [](auto& entry) {
            entry.second >>= 1;
            return entry.second == 0;
        });
        if (diag) {
            static auto window = now;
            if (now - window >= std::chrono::seconds(5)) {
                const double s = std::chrono::duration<double>(now - window).count();
                std::printf("Prefetched read-backs: %.0f/s served, %.0f/s missed; %.0f pages/s "
                            "(%.0f KiB/s) copied; %zu recurring pages\n",
                            prefetch_served.exchange(0) / s, prefetch_missed.exchange(0) / s,
                            prefetch_copied_pages.exchange(0) / s,
                            prefetch_copied_bytes.exchange(0) / 1024.0 / s, prefetch_hits.size());
                window = now;
            }
        }
    }
    if (prefetch_hits.empty()) {
        return;
    }
    // The command buffer being submitted signals this tick (the scheduler took the next one).
    const u64 tick = scheduler.CurrentTick() - 1;
    if (prefetch_batches.empty()) {
        for (u32 i = 0; i < PrefetchBatch::NumBatches; ++i) {
            prefetch_batches.push_back(std::make_unique<PrefetchBatch>(instance));
        }
    }
    // The oldest batch whose copies the GPU has run (only the GPU thread reads them).
    PrefetchBatch* batch = nullptr;
    for (auto& candidate : prefetch_batches) {
        if ((candidate->tick == 0 || scheduler.IsFree(candidate->tick)) &&
            (!batch || candidate->tick < batch->tick)) {
            batch = candidate.get();
        }
    }
    if (!batch) {
        return; // all in flight
    }
    for (const auto& entry : batch->entries) {
        if (auto it = prefetch_latest.find(entry.page);
            it != prefetch_latest.end() && it->second.batch == batch) {
            prefetch_latest.erase(it);
        }
    }
    batch->entries.clear();
    batch->tick = 0;
    batch->invalidated = false;

    struct ArenaCopies {
        const Buffer* arena;
        boost::container::small_vector<vk::BufferCopy, 16> copies;
    };
    boost::container::small_vector<ArenaCopies, 4> by_arena;
    u64 total = 0;
    for (const auto& [page, hits] : prefetch_hits) {
        if (hits < 2 || !memory_tracker->IsRegionGpuModified(page, TRACKER_BYTES_PER_PAGE)) {
            continue;
        }
        const u64 write_tick = memory_tracker->LatestGpuWriteTick(page, TRACKER_BYTES_PER_PAGE);
        if (write_tick > tick) {
            continue; // marked for the next command buffer
        }
        if (auto it = prefetch_latest.find(page);
            it != prefetch_latest.end() && it->second.batch->tick >= write_tick) {
            continue; // a copy at least as new is on its way or ready
        }
        PrefetchBatch::Entry entry{.page = page, .parts = {}, .offset = total};
        u64 size = 0;
        gpu_modified_ranges.ForEachInRange(page, TRACKER_BYTES_PER_PAGE,
                                           [&](VAddr start, VAddr end) {
                                               entry.parts.emplace_back(start, end);
                                               size += Common::AlignUp(end - start, 16);
                                           });
        if (entry.parts.empty() || total + size > PrefetchBatch::StagingSize) {
            continue;
        }
        const u64 block = page >> block_shift;
        const Buffer* arena = GetArena(block, block);
        ArenaCopies* target = nullptr;
        for (auto& candidate : by_arena) {
            if (candidate.arena == arena) {
                target = &candidate;
            }
        }
        if (!target) {
            target = &by_arena.emplace_back(ArenaCopies{arena, {}});
        }
        u64 dst = total;
        for (const auto& [start, end] : entry.parts) {
            target->copies.push_back(vk::BufferCopy{
                .srcOffset = start - arena->cpu_addr,
                .dstOffset = dst,
                .size = end - start,
            });
            dst += Common::AlignUp(end - start, 16);
        }
        total = dst;
        prefetch_latest[page] = PrefetchRef{batch, static_cast<u32>(batch->entries.size())};
        batch->entries.push_back(std::move(entry));
    }
    if (batch->entries.empty()) {
        return;
    }
    batch->tick = tick;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::MemoryBarrier2 before = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& [arena, copies] : by_arena) {
        cmdbuf.copyBuffer(arena->Handle(), batch->staging.Handle(), copies);
    }
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    prefetch_copied_pages.fetch_add(batch->entries.size(), std::memory_order_relaxed);
    prefetch_copied_bytes.fetch_add(total, std::memory_order_relaxed);
}

/// GPU thread, a guest write fault on pages with GPU data: true when every such page has a
/// finished copy newer than its last GPU write; their bytes are stored and they stop being GPU
/// data. False: nothing changed, the side queue reads them back.
bool BufferCache::TryPrefetched(VAddr page, u64 length) {
    if (!PrefetchEnabled()) {
        return false;
    }
    std::scoped_lock lock{prefetch_mutex};
    boost::container::small_vector<std::pair<VAddr, PrefetchRef>, 4> found;
    for (VAddr at = page; at < page + length; at += TRACKER_BYTES_PER_PAGE) {
        if (!memory_tracker->IsRegionGpuModified(at, TRACKER_BYTES_PER_PAGE)) {
            continue;
        }
        const auto it = prefetch_latest.find(at);
        if (it == prefetch_latest.end()) {
            if (prefetch_hits.contains(at)) {
                prefetch_missed.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        PrefetchBatch* batch = it->second.batch;
        if (memory_tracker->LatestGpuWriteTick(at, TRACKER_BYTES_PER_PAGE) > batch->tick ||
            !scheduler.IsFree(batch->tick)) {
            prefetch_missed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        found.emplace_back(at, it->second);
    }
    if (found.empty()) {
        return false;
    }
    for (const auto& [at, ref] : found) {
        PrefetchBatch* batch = ref.batch;
        if (!batch->invalidated) {
            batch->staging.Invalidate(0, PrefetchBatch::StagingSize);
            batch->invalidated = true;
        }
        const auto& entry = batch->entries[ref.entry];
        const u8* staging = batch->staging.mapped_data.data();
        u64 offset = entry.offset;
        for (const auto& [start, end] : entry.parts) {
            // Only the bytes that are still the GPU's: a part handed back meanwhile holds newer
            // guest data.
            gpu_modified_ranges.ForEachInRange(start, end - start, [&](VAddr s, VAddr e) {
                WriteBackGpuData(std::bit_cast<u8*>(s), staging + offset + (s - start), e - s);
            });
            offset += Common::AlignUp(end - start, 16);
        }
        gpu_modified_ranges.Subtract(at, TRACKER_BYTES_PER_PAGE);
        memory_tracker->UnmarkRegionAsGpuModified(at, TRACKER_BYTES_PER_PAGE);
        prefetch_latest.erase(at);
        auto& hits = prefetch_hits[at];
        hits = static_cast<u8>(std::min<u32>(hits + 1u, 8u));
    }
    prefetch_served.fetch_add(found.size(), std::memory_order_relaxed);
    return true;
}

// bbport: deferred raycaster copies (see landing_gate.h; after drakolordx7's upstream PR #397). The
// guest part of the copy is made at the dispatch; the GPU-written bytes of the source land in the
// destination on a worker thread once the GPU work that wrote them has completed, in registration
// order.
struct BufferCache::Landing {
    u64 seq{};
    u64 tick{};
    vk::Buffer buffer;
    boost::container::small_vector<vk::BufferCopy, 4> copies; ///< dstOffset: in the side staging
    boost::container::small_vector<VAddr, 4> targets;         ///< guest address of each copy
    /// A shared page's GPU bytes (ShareGpuPage): the guest's bytes there when the page was
    /// handed over (same layout as the staging); a byte that differs at landing is the guest's.
    bool shared{false};
    std::vector<u8> before;
};

// bbport: a shared page's guest and GPU bytes are told apart by comparing with the guest's
// bytes at the handover, and a rewritten value can match them in some bytes: merged byte by
// byte, a pointer or T# came out half the guest's, half the GPU's (U3 chapter 2, leaving the
// museum: texture addresses with a stray top byte, then a crash). They are not mixed within
// an aligned 32-byte block (a T#; V#s, pointers and matrix rows fit too): a block with any
// byte changed is the guest's. Calls f(offset, bytes) for each run of blocks whose state is
// `changed`.
template <typename F>
static void ForEachSharedBlockRun(VAddr base, const u8* now, const u8* before, u64 size,
                                  bool changed, F&& f) {
    static constexpr u64 Block = 32;
    const auto block_end = [&](u64 at) {
        return std::min<u64>(size, Common::AlignUp(base + at + 1, Block) - base);
    };
    const auto differs = [&](u64 at, u64 end) {
        return std::memcmp(now + at, before + at, end - at) != 0;
    };
    for (u64 at = 0; at < size;) {
        const u64 end = block_end(at);
        if (differs(at, end) != changed) {
            at = end;
            continue;
        }
        u64 run_end = end;
        while (run_end < size) {
            const u64 next = block_end(run_end);
            if (differs(run_end, next) != changed) {
                break;
            }
            run_end = next;
        }
        f(at, run_end - at);
        at = run_end;
    }
}

static bool SharedWritesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHARED_WRITES");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

bool BufferCache::ShareGpuPage(VAddr page, u64 length) {
    if (!SharedWritesEnabled() || !side_queue || land_stopping.load(std::memory_order_acquire)) {
        return false;
    }
    const u64 first_block = page >> block_shift;
    const u64 last_block = (page + length - 1) >> block_shift;
    const Buffer* arena = GetArena(first_block, last_block);
    if (!arena || page < arena->cpu_addr || page + length > arena->cpu_addr + arena->size_bytes) {
        return false;
    }
    auto landing = std::make_shared<Landing>();
    landing->buffer = arena->Handle();
    landing->shared = true;
    u64 total = 0;
    gpu_modified_ranges.ForEachInRange(page, length, [&](VAddr start, VAddr end) {
        // Bytes already on their way back (an earlier write into the page) are left out.
        shared_gpu_ranges.ForEachNotInRange(start, end - start, [&](VAddr lo, u64 size) {
            total = Common::AlignUp(total, 64);
            landing->copies.push_back(vk::BufferCopy{
                .srcOffset = lo - arena->cpu_addr,
                .dstOffset = total,
                .size = size,
            });
            landing->targets.push_back(lo);
            total += size;
        });
    });
    if (landing->copies.empty()) {
        // Every GPU byte of the page is on its way back already.
        return shared_gpu_ranges.Intersects(page, length);
    }
    if (total > SideQueue::StagingSize) {
        return false;
    }
    // The landing waits on the timeline for the writing work, which must be submitted to signal
    // it (as in BeginSideDownload).
    const u64 tick = memory_tracker->LatestGpuWriteTick(page, length);
    if (tick == 0) {
        return false;
    }
    for (int flushes = 0; scheduler.CurrentTick() <= tick; ++flushes) {
        if (flushes == 2) {
            return false;
        }
        scheduler.Flush();
    }
    landing->tick = tick;
    landing->before.resize(total);
    for (size_t i = 0; i < landing->copies.size(); ++i) {
        memory->CopySparseMemory(landing->targets[i],
                                 landing->before.data() + landing->copies[i].dstOffset,
                                 landing->copies[i].size);
    }
    {
        std::scoped_lock lk{landing_mutex};
        if (landing_stop) {
            return false;
        }
        landing->seq = ++landing_registered_seq;
        landing_registered.store(landing->seq, std::memory_order_release);
        for (size_t i = 0; i < landing->copies.size(); ++i) {
            shared_gpu_ranges.Add(landing->targets[i], landing->copies[i].size);
            const u8* before = landing->before.data() + landing->copies[i].dstOffset;
            shared_before[landing->targets[i]].assign(before, before + landing->copies[i].size);
        }
        landing_queue.push_back(std::move(landing));
        if (!landing_thread.joinable()) {
            landing_thread = std::thread([this] { LandingWorker(); });
        }
    }
    landing_cv.notify_one();
    return true;
}

/// bbport diagnosis (BB_PERF_DIAG=1): landed shared pages, those the GPU wrote again after the
/// landing's work, and of those the ones marked CPU-modified (an upload takes the whole page
/// from guest memory, over the GPU's newer bytes), every 5 s. nullopt: a page not written again.
/// bbport: video memory bound into the buffer arenas so far (never given back: diagnosis of
/// memory growth over long sessions, printed with the shared landings).
static std::atomic<u64> g_resident_buffer_bytes{0};

static void NoteSharedRewrite(std::optional<bool> cpu_modified) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PERF_DIAG");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static u64 landed = 0, rewritten = 0, at_risk = 0;
    static auto window = std::chrono::steady_clock::now();
    ++landed;
    rewritten += cpu_modified.has_value();
    at_risk += cpu_modified.value_or(false);
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    std::printf("Shared landings: %llu pages in 5 s, %llu written again by the GPU, %llu of them "
                "CPU-modified; buffer memory resident %llu MiB\n",
                static_cast<unsigned long long>(landed), static_cast<unsigned long long>(rewritten),
                static_cast<unsigned long long>(at_risk),
                static_cast<unsigned long long>(g_resident_buffer_bytes.load() >> 20));
    landed = rewritten = at_risk = 0;
}

void BufferCache::DrainSharedLandings() {
    if (!shared_landed_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<SharedLanded> landed;
    {
        std::scoped_lock lk{shared_landed_mutex};
        landed.swap(shared_landed);
        shared_landed_pending.store(false, std::memory_order_relaxed);
    }
    for (const auto& done : landed) {
        for (const auto& [start, end, guest_kept] : done.ranges) {
            shared_gpu_ranges.Subtract(start, end - start);
            shared_before.erase(start);
            for (VAddr page = Common::AlignDown(start, TRACKER_BYTES_PER_PAGE); page < end;
                 page += TRACKER_BYTES_PER_PAGE) {
                // The GPU wrote the page again after the landing's work: its data stays the GPU's.
                // Not marked CPU-modified either: uploads take whole pages, and guest memory
                // holds the landing's older GPU bytes (the guest's own writes there reached the
                // GPU through the shared uploads, or fault again).
                if (memory_tracker->LatestGpuWriteTick(page, TRACKER_BYTES_PER_PAGE) > done.tick) {
                    NoteSharedRewrite(
                        memory_tracker->IsRegionCpuModified(page, TRACKER_BYTES_PER_PAGE));
                    continue;
                }
                NoteSharedRewrite(std::nullopt);
                const VAddr lo = std::max(start, page);
                const VAddr hi = std::min<VAddr>(end, page + TRACKER_BYTES_PER_PAGE);
                // Guest bytes kept over the GPU's: an upload since the handover left them out.
                if (guest_kept) {
                    memory_tracker->MarkRegionAsCpuModified(lo, hi - lo);
                }
                gpu_modified_ranges.Subtract(lo, hi - lo);
                if (!gpu_modified_ranges.Intersects(page, TRACKER_BYTES_PER_PAGE)) {
                    memory_tracker->UnmarkRegionAsGpuModified(page, TRACKER_BYTES_PER_PAGE);
                }
            }
        }
    }
}

bool BufferCache::DeferGuestCopy(VAddr src, VAddr dst, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SCALAR_COPY64_DEFER");
        return !(env && env[0] == '0');
    }();
    if (!enabled || !side_queue || land_stopping.load(std::memory_order_acquire) || size == 0 ||
        size > SideQueue::StagingSize / 2 || !memory->IsValidGpuMapping(src, size) ||
        !memory->IsValidGpuMapping(dst, size)) {
        return false;
    }
    const u64 first_block = src >> block_shift;
    const u64 last_block = (src + size - 1) >> block_shift;
    const Buffer* arena = GetArena(first_block, last_block);
    if (!arena || src < arena->cpu_addr || src + size > arena->cpu_addr + arena->size_bytes) {
        return false;
    }
    auto landing = std::make_shared<Landing>();
    landing->buffer = arena->Handle();
    u64 total = 0;
    gpu_modified_ranges.ForEachInRange(src, size, [&](VAddr start, VAddr end) {
        total = Common::AlignUp(total, 64);
        landing->copies.push_back(vk::BufferCopy{
            .srcOffset = start - arena->cpu_addr,
            .dstOffset = total,
            .size = end - start,
        });
        landing->targets.push_back(dst + (start - src));
        total += end - start;
    });
    if (landing->copies.empty() || total > SideQueue::StagingSize) {
        return false;
    }
    // The tick of the work that wrote the source. Everything is recorded by now (the dispatch
    // follows a drain of the draw pipe), so a write marked for the next tick is in the open
    // command buffer; that one is submitted here, so the tick completes without anybody waiting
    // for something else to submit it.
    u64 tick = memory_tracker->LatestGpuWriteTick(src, size);
    if (tick != 0) {
        tick = std::min(tick, scheduler.CurrentTick());
        if (tick >= scheduler.CurrentTick()) {
            scheduler.Flush();
        }
    }
    if (tick == 0) {
        return false; // page flags and written ranges disagree: nothing says when it lands
    }
    landing->tick = tick;
    // The guest part of the copy now; the GPU-written bytes follow when they have landed.
    memory->TryWriteBacking(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), size);
    {
        std::scoped_lock lk{landing_mutex};
        if (landing_stop) {
            return false;
        }
        landing->seq = ++landing_registered_seq;
        landing_registered.store(landing->seq, std::memory_order_release);
        landing_queue.push_back(std::move(landing));
        if (!landing_thread.joinable()) {
            landing_thread = std::thread([this] { LandingWorker(); });
        }
    }
    landing_cv.notify_one();
    return true;
}

/// Waits for the GPU to reach `tick`, in slices, so that the shutdown can cancel it. False when
/// cancelled or when the wait failed for a reason other than a lost device (which ends the game,
/// like the other GPU waits do).
bool BufferCache::WaitLandingTick(u64 tick) {
    auto& semaphore = *scheduler.GetWorkSemaphore();
    if (semaphore.IsFree(tick)) {
        return true;
    }
    semaphore.Refresh();
    if (semaphore.IsFree(tick)) {
        return true;
    }
    const vk::Device device = instance.GetDevice();
    const vk::Semaphore handle = semaphore.Handle();
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &tick,
    };
    const auto begin = std::chrono::steady_clock::now();
    bool reported = false;
    while (!land_stopping.load(std::memory_order_acquire)) {
        const vk::Result result = device.waitSemaphores(&wait_info, 20'000'000ull);
        if (result == vk::Result::eSuccess) {
            semaphore.Refresh();
            return true;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Vulkan::DeviceLost(device);
        }
        if (result != vk::Result::eTimeout) {
            std::printf("Landing: waiting for tick %llu failed (%s); the copy lands without data\n",
                        static_cast<unsigned long long>(tick), vk::to_string(result).c_str());
            return false;
        }
        if (!reported && std::chrono::steady_clock::now() - begin > std::chrono::seconds(5)) {
            reported = true;
            std::printf("GPU stall: the landing thread waits 5 s for tick %llu\n",
                        static_cast<unsigned long long>(tick));
        }
    }
    return false;
}

bool BufferCache::ExecuteLanding(Landing& landing) {
    auto& side = *side_queue;
    if (!WaitLandingTick(landing.tick)) {
        return false;
    }
    u64 total = 0;
    for (const auto& copy : landing.copies) {
        total = std::max(total, copy.dstOffset + copy.size);
    }
    // A slot of the side queue (the read-backs of guest write faults use the others).
    SideQueue::Slot* slot = nullptr;
    while (!(slot = side.Acquire())) {
        if (land_stopping.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::yield();
    }
    Vulkan::Check(side.device.resetCommandPool(*slot->pool));
    Vulkan::Check(slot->cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    slot->cmdbuf.copyBuffer(landing.buffer, slot->staging.Handle(), landing.copies);
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    slot->cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
    Vulkan::Check(slot->cmdbuf.end());
    const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const u64 wait_value = landing.tick;
    const vk::TimelineSemaphoreSubmitInfo timeline_info = {
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &wait_value,
    };
    const vk::SubmitInfo submit = {
        .pNext = &timeline_info,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &timeline,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->cmdbuf,
    };
    {
        std::scoped_lock submit_lock{side.submit_mutex};
        Vulkan::Check(side.queue.submit(submit, *slot->fence));
    }
    // The tick has been reached, so the copy is short. In slices anyway: at shutdown it gets
    // half a second more.
    u32 slices_after_stop = 0;
    for (;;) {
        const vk::Result result = side.device.waitForFences(*slot->fence, true, 20'000'000ull);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Vulkan::DeviceLost(side.device);
        }
        if (result != vk::Result::eTimeout) {
            Vulkan::Check(result);
        }
        if (land_stopping.load(std::memory_order_acquire) && ++slices_after_stop > 25) {
            return false; // the slot stays taken: its copy may still be running
        }
    }
    Vulkan::Check(side.device.resetFences(*slot->fence));
    slot->staging.Invalidate(0, total);
    if (landing.shared) {
        // Only the bytes the guest has not rewritten since the page was handed over.
        SharedLanded done{.tick = landing.tick};
        std::vector<u8> now;
        for (size_t i = 0; i < landing.copies.size(); ++i) {
            const u64 size = landing.copies[i].size;
            const VAddr target = landing.targets[i];
            const u8* gpu = slot->staging.mapped_data.data() + landing.copies[i].dstOffset;
            const u8* before = landing.before.data() + landing.copies[i].dstOffset;
            now.resize(size);
            memory->CopySparseMemory(target, now.data(), size);
            const bool guest_kept = std::memcmp(now.data(), before, size) != 0;
            // The guest keeps writing these pages: each word is replaced only if it still holds
            // the handover's value (compared and written at once).
            ForEachSharedBlockRun(target, now.data(), before, size, false, [&](u64 at, u64 bytes) {
                memory->TryMergeBacking(reinterpret_cast<void*>(target + at), before + at, gpu + at,
                                        bytes);
            });
            done.ranges.push_back({target, target + size, guest_kept});
        }
        std::scoped_lock lk{shared_landed_mutex};
        shared_landed.push_back(std::move(done));
        shared_landed_pending.store(true, std::memory_order_release);
    } else {
        for (size_t i = 0; i < landing.copies.size(); ++i) {
            memory->TryWriteBacking(reinterpret_cast<void*>(landing.targets[i]),
                                    slot->staging.mapped_data.data() + landing.copies[i].dstOffset,
                                    landing.copies[i].size);
        }
    }
    side.Release(slot);
    return true;
}

void BufferCache::LandingWorker() {
    Common::SetCurrentThreadName("bb:Landing");
    std::unique_lock lk{landing_mutex};
    for (;;) {
        landing_cv.wait(lk, [&] {
            return landing_stop || !landing_queue.empty() ||
                   (!gated_labels.empty() && gated_labels.front().seq <= landing_done_seq);
        });
        if (landing_stop) {
            break;
        }
        if (!gated_labels.empty() && gated_labels.front().seq <= landing_done_seq) {
            auto fn = std::move(gated_labels.front().fn);
            gated_labels.pop_front();
            gate_running = true;
            lk.unlock();
            fn();
            lk.lock();
            gate_running = false;
            gate_outstanding.fetch_sub(1, std::memory_order_release);
            continue;
        }
        const auto landing = std::move(landing_queue.front());
        landing_queue.pop_front();
        lk.unlock();
        // Lands without data when cancelled at shutdown or when the wait for the GPU failed:
        // the labels behind it go on either way.
        ExecuteLanding(*landing);
        lk.lock();
        landing_done_seq = landing->seq;
        landing_done.store(landing->seq, std::memory_order_release);
        landing_done_cv.notify_all();
    }
    // Shutdown: what is still pending is dropped, nobody waits for it any more.
    landing_queue.clear();
    gated_labels.clear();
    gate_outstanding.store(0, std::memory_order_release);
    landing_done_seq = landing_registered_seq;
    landing_done.store(landing_done_seq, std::memory_order_release);
    landing_done_cv.notify_all();
    landing_exited.store(true, std::memory_order_release);
}

BufferCache::~BufferCache() {
    StopLanding();
}

void BufferCache::StopLanding() {
    // Stop first: nothing new goes to the landing thread, and the waits on it (its wait for the
    // GPU, the guest-visible labels waiting behind a landing) let go.
    const auto stop_begin = std::chrono::steady_clock::now();
    land_stopping.store(true, std::memory_order_release);
    land_instance.store(nullptr, std::memory_order_release);
    u64 dropped_copies = 0;
    size_t dropped_labels = 0;
    {
        std::scoped_lock lk{landing_mutex};
        landing_stop = true;
        dropped_copies = landing_registered_seq - landing_done_seq;
        dropped_labels = gated_labels.size();
    }
    landing_cv.notify_all();
    landing_done_cv.notify_all();
    if (landing_thread.joinable()) {
        // The thread leaves its waits within a slice (20 ms). If it is stuck in the driver
        // anyway the process must still end: this runs from the exit of the game.
        for (int ms = 0; ms < 3000 && !landing_exited.load(std::memory_order_acquire); ++ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (landing_exited.load(std::memory_order_acquire)) {
            landing_thread.join();
            if (dropped_copies != 0 || dropped_labels != 0) {
                std::printf("Landing thread stopped: %llu copies and %zu labels were still pending "
                            "(%lld ms)\n",
                            static_cast<unsigned long long>(dropped_copies), dropped_labels,
                            static_cast<long long>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - stop_begin)
                                    .count()));
            }
        } else {
            std::printf("Landing thread did not stop within 3 s: ending the process\n");
            std::fflush(nullptr);
            std::_Exit(0);
        }
    }
}

bool BufferCache::LandingGateActive() const {
    return PendingLanding() != 0 || gate_outstanding.load(std::memory_order_acquire) != 0;
}

u64 BufferCache::PendingLanding() const {
    const u64 registered = landing_registered.load(std::memory_order_acquire);
    return registered > landing_done.load(std::memory_order_acquire) ? registered : 0;
}

void BufferCache::WaitLanding(u64 seq) {
    if (seq == 0 || landing_done.load(std::memory_order_acquire) >= seq) {
        return;
    }
    std::unique_lock lk{landing_mutex};
    // Released at shutdown as well; a stall is reported, not ended (the landing thread waits for
    // the GPU, which ends the game when the device is lost).
    const auto done = [&] { return landing_stop || landing_done_seq >= seq; };
    if (!landing_done_cv.wait_for(lk, std::chrono::seconds(5), done)) {
        std::printf("GPU stall: a label waits 5 s for the landing of copy %llu\n",
                    static_cast<unsigned long long>(seq));
        landing_done_cv.wait(lk, done);
    }
}

void BufferCache::RunAfterLanding(u64 seq, std::function<void()> fn) {
    {
        std::unique_lock lk{landing_mutex};
        if (landing_stop || (seq <= landing_done_seq && gated_labels.empty() && !gate_running)) {
            lk.unlock();
            fn();
            return;
        }
        gated_labels.push_back({seq, std::move(fn)});
        gate_outstanding.fetch_add(1, std::memory_order_release);
    }
    landing_cv.notify_one();
}

namespace LandingGate {
u64 Pending() {
    auto* cache = land_instance.load(std::memory_order_acquire);
    return cache ? cache->PendingLanding() : 0;
}
bool Active() {
    auto* cache = land_instance.load(std::memory_order_acquire);
    return cache && cache->LandingGateActive();
}
void Wait(u64 seq) {
    if (auto* cache = land_instance.load(std::memory_order_acquire)) {
        cache->WaitLanding(seq);
    }
}
void Run(u64 seq, std::function<void()> fn) {
    if (auto* cache = land_instance.load(std::memory_order_acquire)) {
        cache->RunAfterLanding(seq, std::move(fn));
    } else {
        fn();
    }
}
bool Stopped() {
    return land_stopping.load(std::memory_order_acquire);
}
} // namespace LandingGate

void BufferCache::ForceUpload(VAddr device_addr, u64 size) {
    if (!IsRegionGpuModified(device_addr, size)) {
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
    }
}

namespace {
/// bbport: the guest frame (g_guest_frames) in which the GPU last bound each 4 KiB page of the
/// GPU heap window, read or written (BufferCache::ObtainBuffer). GPU data still in use, such as
/// buffers a compute shader made once and every frame draws from, is never taken for dead.
constexpr VAddr GpuUseLow = 0x1000000000ull, GpuUseHigh = 0x1200000000ull;
std::unique_ptr<u32[]> gpu_use_frame{new u32[(GpuUseHigh - GpuUseLow) >> 12]{}};

void NoteGpuUse(VAddr address, u64 size) {
    const VAddr begin = std::max(address, GpuUseLow);
    const VAddr end = std::min(address + size, GpuUseHigh);
    if (begin >= end) {
        return;
    }
    const u32 frame = g_guest_frames.load(std::memory_order_relaxed);
    for (u64 page = (begin - GpuUseLow) >> 12, last = (end - 1 - GpuUseLow) >> 12; page <= last;
         ++page) {
        gpu_use_frame[page] = frame;
    }
}

// bbport diagnosis: the latest resolutions of CPU writes to pages holding GPU data, printed on
// a GPU crash for the buffers the crashed command read (BB_GPU_CHECKPOINTS=1).
struct FaultEvent {
    VAddr page;
    u32 length;
    u32 frame;
    u32 gpu_write;
    u32 gpu_use;
    u32 kind; // 1 dropped, 3 read back, 4 read back on the side queue (2: merged, removed)
};
constexpr u64 FaultHistorySize = 1u << 16;
std::unique_ptr<FaultEvent[]> fault_history;
std::atomic<u64> fault_events{0};

/// bbport diagnosis (BB_PERF_DIAG=1): side queue read-backs of write faults split by whether the
/// write lands on GPU-written bytes or only next to them, and whether the GPU work that wrote the
/// page had finished at the fault.
void NoteFaultSplit(bool on_gpu_bytes, bool work_done, std::chrono::steady_clock::duration waited,
                    bool shared = false) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PERF_DIAG");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::array<u64, 5> count{}, ns{};
    static auto window = std::chrono::steady_clock::now();
    const u32 kind = shared ? 4 : (on_gpu_bytes ? 2 : 0) | (work_done ? 1 : 0);
    ++count[kind];
    ns[kind] += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - window).count();
    window = now;
    std::printf("Fault split: next to GPU bytes: work done %.0f/s %.1f ms/s, in flight %.0f/s "
                "%.1f ms/s; on GPU bytes: work done %.0f/s %.1f ms/s, in flight %.0f/s %.1f ms/s; shared (no wait) %.0f/s "
                "%.1f ms/s\n",
                count[1] / seconds, ns[1] / 1e6 / seconds, count[0] / seconds,
                ns[0] / 1e6 / seconds, count[3] / seconds, ns[3] / 1e6 / seconds,
                count[2] / seconds, ns[2] / 1e6 / seconds, count[4] / seconds,
                ns[4] / 1e6 / seconds);
    count = {};
    ns = {};
}

/// bbport diagnosis (BB_PERF_DIAG=1): every 5 s, the 64 KiB regions whose guest writes waited
/// for a side queue read-back most often, with the time waited and how many guest frames ago the
/// GPU wrote / last used the faulting page.
void NoteSideDownload(VAddr page, std::chrono::steady_clock::duration waited, u32 write_age,
                      u32 use_age) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PERF_DIAG");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    struct Stat {
        VAddr region;
        u32 count;
        u64 ns;
        u32 min_write_age, max_write_age, min_use_age;
    };
    static std::array<Stat, 64> stats{};
    static u64 total = 0, total_ns = 0;
    static auto window = std::chrono::steady_clock::now();
    const VAddr region = page & ~VAddr{0xFFFF};
    const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    ++total;
    total_ns += ns;
    auto slot = std::ranges::find_if(stats, [&](const Stat& s) { return s.count && s.region == region; });
    if (slot == stats.end()) {
        slot = std::ranges::min_element(stats, {}, &Stat::count);
        *slot = {region, 0, 0, write_age, write_age, use_age};
    }
    ++slot->count;
    slot->ns += ns;
    slot->min_write_age = std::min(slot->min_write_age, write_age);
    slot->max_write_age = std::max(slot->max_write_age, write_age);
    slot->min_use_age = std::min(slot->min_use_age, use_age);
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - window).count();
    window = now;
    std::printf("Side read-backs: %.0f/s, %.1f ms/s waited\n", total / seconds,
                total_ns / 1e6 / seconds);
    std::ranges::sort(stats, std::greater{}, &Stat::count);
    for (u32 i = 0; i < 10 && stats[i].count; ++i) {
        const auto& s = stats[i];
        std::printf("  %#llx: %.1f/s, %.0f us each, GPU wrote %u-%u frames ago, used %u ago\n",
                    static_cast<unsigned long long>(s.region), s.count / seconds,
                    s.ns / 1e3 / s.count, s.min_write_age, s.max_write_age, s.min_use_age);
    }
    stats = {};
    total = total_ns = 0;
}

void NoteFault(VAddr page, u64 length, u32 frame, u32 gpu_write, u32 gpu_use, u32 kind) {
    if (!fault_history) {
        return;
    }
    const u64 n = fault_events.fetch_add(1, std::memory_order_relaxed);
    fault_history[n % FaultHistorySize] = {page, static_cast<u32>(length), frame, gpu_write,
                                           gpu_use, kind};
}

/// The latest frame the GPU bound a page of the range; outside the window: now (in use).
u32 LastGpuUse(VAddr address, u64 size) {
    if (address < GpuUseLow || address + size > GpuUseHigh) {
        return g_guest_frames.load(std::memory_order_relaxed);
    }
    u32 latest = 0;
    for (u64 page = (address - GpuUseLow) >> 12, last = (address + size - 1 - GpuUseLow) >> 12;
         page <= last; ++page) {
        latest = std::max(latest, gpu_use_frame[page]);
    }
    return latest;
}
} // namespace

} // namespace VideoCore

// bbport: U3 frees a cloth simulation's buffers right after its last dispatch and its heap reuses
// the memory at once, keeping its free list in the freed block. The buffers' GPU data stayed
// recorded (a writable binding marks the whole bound range), and a later fault on the page read
// it back over the list where the guest's new bytes equalled the old ones (the merge, since
// removed, kept the GPU's bytes there): the next allocation crashed (image+0x4944cb). With exact
// read-backs before a page opens this should not happen any more; the guard stays as a safety
// net (BB_U3_FREE_HOOK=0: off). Frees are noted in a
// lock-free ring (a level change frees thousands of blocks; a lock per free stalled the game
// thread and characters fell through the floor); read-backs walk the noted blocks and leave out
// the header and list links of those still free. The rest of a freed block keeps the GPU's
// data, as on the console: U3 still reads some freed buffers on the GPU.
namespace {
struct FreedNote {
    std::atomic<u64> sequence{0}; // index + 1 once written
    VAddr start = 0, end = 0;
    u32 frame = 0;
};
constexpr u64 FreedNoteCount = 4096;
constexpr u32 FreedNoteFrames = 8;
std::array<FreedNote, FreedNoteCount> freed_notes;
std::atomic<u64> freed_head{0};
std::atomic<u32> freed_last_frame{0};
} // namespace

extern "C" void bbgpu_note_guest_free(uintptr_t address, uint64_t size) {
    const u64 index = freed_head.fetch_add(1, std::memory_order_relaxed);
    auto& note = freed_notes[index % FreedNoteCount];
    const u32 frame = VideoCore::g_guest_frames.load(std::memory_order_relaxed);
    note.sequence.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    note.start = address;
    note.end = address + size;
    note.frame = frame;
    note.sequence.store(index + 1, std::memory_order_release);
    freed_last_frame.store(frame, std::memory_order_relaxed);
}

namespace VideoCore {

void BufferCache::WriteBackGpuData(u8* dst, const u8* data, u64 size) {
    const u32 frame = g_guest_frames.load(std::memory_order_relaxed);
    const u64 head = freed_head.load(std::memory_order_acquire);
    if (head == 0 || frame - freed_last_frame.load(std::memory_order_relaxed) > FreedNoteFrames) {
        memory->TryWriteBacking(dst, data, size);
        return;
    }
    const VAddr start = reinterpret_cast<VAddr>(dst);
    const VAddr end = start + size;
    boost::container::small_vector<std::pair<VAddr, VAddr>, 4> skip;
    // U3's heap blocks: a 16-byte header with the block's size at +4 (header included;
    // positive while free, negative while allocated), the next block right after. A noted
    // range is walked block by block (a freed block may have been split or merged since); the
    // header and list links (next, previous) of the free blocks in it are the heap's.
    constexpr s32 MaxBlock = 64 << 20;
    for (u64 index = head > FreedNoteCount ? head - FreedNoteCount : 0; index < head; ++index) {
        const auto& note = freed_notes[index % FreedNoteCount];
        if (note.sequence.load(std::memory_order_acquire) != index + 1) {
            continue;
        }
        const VAddr note_start = note.start, note_end = note.end;
        const u32 note_frame = note.frame;
        if (note.sequence.load(std::memory_order_acquire) != index + 1 ||
            frame - note_frame > FreedNoteFrames || note_start - 0x10 >= end ||
            note_end + 0x10 <= start) {
            continue;
        }
        for (VAddr at = note_start - 0x10; at <= note_end && at < end;) {
            s32 block_size;
            if (!runtime_memory_read_backing(at + 4, &block_size, 4)) {
                break;
            }
            const s32 length = block_size < 0 ? -block_size : block_size;
            if (length < 0x20 || length > MaxBlock || (length & 0xf) != 0) {
                break;
            }
            if (block_size > 0 && at + 0x20 > start && at < end) {
                skip.emplace_back(std::max(at, start), std::min<VAddr>(at + 0x20, end));
            }
            at += length;
        }
    }
    if (skip.empty()) {
        memory->TryWriteBacking(dst, data, size);
        return;
    }
    std::sort(skip.begin(), skip.end());
    VAddr cursor = start;
    for (const auto& [skip_start, skip_end] : skip) {
        if (skip_start > cursor) {
            memory->TryWriteBacking(reinterpret_cast<void*>(cursor), data + (cursor - start),
                                    skip_start - cursor);
        }
        cursor = std::max(cursor, skip_end);
    }
    if (end > cursor) {
        memory->TryWriteBacking(reinterpret_cast<void*>(cursor), data + (cursor - start), end - cursor);
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    ForgetStreamCopies();
    {
        // bbport diagnosis: the thread of guest writes over BB_GPU_WRITE_WATCH's 64 KiB.
        static const VAddr watch = [] {
            const char* env = std::getenv("BB_GPU_WRITE_WATCH");
            return env ? static_cast<VAddr>(std::strtoull(env, nullptr, 16)) : VAddr{0};
        }();
        static std::atomic<u32> thread_prints{0};
        if (watch && device_addr < watch + 0x10000 && watch < device_addr + size &&
            thread_prints.fetch_add(1, std::memory_order_relaxed) < 12) {
            std::printf("GPU write watch: write at %#llx from thread '%s'%s\n",
                        static_cast<unsigned long long>(device_addr),
                        Common::GetCurrentThreadName().c_str(), assume_locks ? " (GPU side)" : "");
        }
    }
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        // bbport: a guest write to a page holding GPU data read that data back first, which
        // waits for the whole GPU queue (U3's Cartagena square: ~70 times a second, 5-20 ms each,
        // the writing game thread stopped). Now the GPU's
        // data is read back on a second queue, which waits on the GPU for the work that
        // wrote the page only (DownloadOnSideQueue, BB_SIDE_DOWNLOAD=0: off). The page becomes
        // accessible only with the GPU's data in it: the guest may read that data next (the same
        // rings hand out neighbouring blocks, and its fences are written when the commands are
        // recorded, before the GPU ran them). Making the page writable at once and merging at
        // the next upload left those bytes stale until then (U3's London bar fight crashed on
        // one) and dropped guest writes of a byte's old value.
        // BB_DEAD_GPU_FRAMES=N (diagnosis): GPU data written and not bound for N guest frames is
        // dropped as dead. Wrong for state a solver keeps between runs and idles over a few
        // frames (U3's cloth at ~80 FPS: the GPU hung in its solver, device lost).
        static const u32 dead_frames = [] {
            const char* env = std::getenv("BB_DEAD_GPU_FRAMES");
            return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
        }();
        const VAddr page = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
        const u64 length = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE) - page;
        // Decided on the GPU thread, where GPU writes are recorded: none can come in between.
        auto resolve = [this, page, length, device_addr, size] {
            // bbport diagnosis: guest writes over BB_GPU_WRITE_WATCH's 64 KiB (see ObtainBuffer).
            static const VAddr watch = [] {
                const char* env = std::getenv("BB_GPU_WRITE_WATCH");
                return env ? static_cast<VAddr>(std::strtoull(env, nullptr, 16)) : VAddr{0};
            }();
            static std::atomic<u32> fault_prints{0};
            if (watch && device_addr < watch + 0x10000 && watch < device_addr + size &&
                fault_prints.fetch_add(1, std::memory_order_relaxed) < 40) {
                std::printf("GPU write watch: guest write fault at %#llx+%#llx\n",
                            static_cast<unsigned long long>(device_addr),
                            static_cast<unsigned long long>(size));
            }
            const u32 frames = g_guest_frames.load(std::memory_order_relaxed);
            const u32 gpu_write = memory_tracker->LatestGpuWriteFrame(page, length);
            const u32 gpu_use = LastGpuUse(page, length);
            const auto side_start = std::chrono::steady_clock::now();
            // bbport diagnosis (BB_FAULT_REGIONS=1): faults by 1 MiB region every 5 s, with
            // how many frames ago the GPU last wrote the page.
            static const bool fault_regions = [] {
                const char* env = std::getenv("BB_FAULT_REGIONS");
                return env && env[0] == '1';
            }();
            if (fault_regions) {
                static std::map<VAddr, std::array<u64, 3>> regions; // count, gpu written this/last frame
                static auto window = std::chrono::steady_clock::now();
                auto& r = regions[page >> 20];
                ++r[0];
                r[1] += frames == gpu_write;
                r[2] += frames == gpu_write + 1;
                if (side_start - window >= std::chrono::seconds(5)) {
                    std::string line;
                    for (const auto& [region, c] : regions) {
                        if (c[0] >= 50) {
                            line += fmt::format(" {:#x}:{:.0f}/s({:.0f}% same frame,{:.0f}% previous)",
                                                region << 20, c[0] / 5.0, 100.0 * c[1] / c[0],
                                                100.0 * c[2] / c[0]);
                        }
                    }
                    std::printf("Fault regions:%s\n", line.c_str());
                    regions.clear();
                    window = side_start;
                }
            }
            const bool on_gpu_bytes = gpu_modified_ranges.Intersects(device_addr, size);
            const bool work_done =
                scheduler.IsFree(memory_tracker->LatestGpuWriteTick(page, length));
            DrainSharedLandings();
            // Shared: GPU bytes still being written, or on GPU bytes already on their way back
            // (the landing keeps what the guest writes there and uploads take it).
            bool on_pending_only = true;
            gpu_modified_ranges.ForEachInRange(device_addr, size, [&](VAddr start, VAddr end) {
                on_pending_only = on_pending_only && shared_gpu_ranges.Contains(start, end - start);
            });
            if ((!work_done || (on_gpu_bytes && on_pending_only)) && ShareGpuPage(page, length)) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                NoteFaultSplit(false, false, std::chrono::steady_clock::now() - side_start, true);
                return;
            }
            if (DownloadOnSideQueue(page, length)) {
                NoteFaultSplit(on_gpu_bytes, work_done,
                               std::chrono::steady_clock::now() - side_start);
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                BbStats::gpu_side_downloads.fetch_add(1, std::memory_order_relaxed);
                NoteFault(page, length, frames, gpu_write, gpu_use, 4);
                NoteSideDownload(page, std::chrono::steady_clock::now() - side_start,
                                 frames - gpu_write, frames - gpu_use);
                return;
            }
            if (dead_frames && frames >= dead_frames && gpu_write <= frames - dead_frames &&
                gpu_use <= frames - dead_frames) {
                gpu_modified_ranges.Subtract(page, length);
                memory_tracker->UnmarkRegionAsGpuModified(page, length);
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                BbStats::dead_gpu_drops.fetch_add(1, std::memory_order_relaxed);
                NoteFault(page, length, frames, gpu_write, gpu_use, 1);
                return;
            }
            ReadMemory(device_addr, size, true, true);
            BbStats::gpu_read_backs.fetch_add(1, std::memory_order_relaxed);
            NoteFault(page, length, frames, gpu_write, gpu_use, 3);
        };
        // bbport: a guest thread waits for its own side queue read-back (BB_ASYNC_SIDE_DOWNLOAD=1;
        // default off: the GPU thread waits, as before. While a read-back is in flight the GPU
        // thread read stale guest memory for later draws: one-frame geometry spikes in U3's pub). The GPU thread submits the copy and
        // goes on; the guest thread waits for it and stores the data; the GPU thread then hands
        // the pages back unless it wrote one of them meanwhile (then the synchronous path). In
        // U3's Cartagena square the GPU thread spent ~40% waiting for these read-backs.
        static const bool async_side = [] {
            const char* env = std::getenv("BB_ASYNC_SIDE_DOWNLOAD");
            return env && env[0] == '1';
        }();
        static const bool ordered = [] {
            const char* env = std::getenv("BB_ORDERED_FAULTS");
            return !(env && env[0] == '0');
        }();
        if (assume_locks) {
            resolve();
        } else if (!async_side && ordered) {
            liverpool->SendOrderedCommand(std::move(resolve));
        } else if (!async_side) {
            liverpool->SendCommand<true>(std::move(resolve));
        } else {
            SideTicket ticket;
            ticket.async = true;
            bool started = false;
            u32 frames = 0, gpu_write = 0, gpu_use = 0;
            const auto side_start = std::chrono::steady_clock::now();
            // BB_ASYNC_DRAIN_BEGIN=1 (diagnosis): the draws already queued run first, so writes
            // they make to these pages are recorded before the read-back is chosen.
            static const bool drain_begin = [] {
                const char* env = std::getenv("BB_ASYNC_DRAIN_BEGIN");
                return env && env[0] == '1';
            }();
            liverpool->SendCommand<true>([&] {
                if (drain_begin && Vulkan::g_drain_draw_pipe) {
                    Vulkan::g_drain_draw_pipe();
                }
                frames = g_guest_frames.load(std::memory_order_relaxed);
                gpu_write = memory_tracker->LatestGpuWriteFrame(page, length);
                gpu_use = LastGpuUse(page, length);
                started = BeginSideDownload(page, length, ticket);
                if (!started) {
                    resolve();
                } else {
                    g_async_stalled.fetch_add(1, std::memory_order_relaxed);
                }
            });
            if (started) {
                WaitSideDownload(ticket);
                liverpool->SendCommand<true>([&] {
                    // Draws queued for the recording thread while the guest waited expect the
                    // GPU's data in these pages: they run before the pages are the guest's
                    // again (else they upload what the guest writes next: one-frame spikes).
                    if (Vulkan::g_drain_draw_pipe) {
                        Vulkan::g_drain_draw_pipe();
                    }
                    struct Unstall {
                        ~Unstall() { g_async_stalled.fetch_sub(1, std::memory_order_relaxed); }
                    } unstall;
                    if (!FinishSideDownload(ticket, page, length)) {
                        resolve(); // the GPU wrote a page meanwhile: decide again
                        return;
                    }
                    memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                    BbStats::gpu_side_downloads.fetch_add(1, std::memory_order_relaxed);
                    NoteFault(page, length, frames, gpu_write, gpu_use, 4);
                    NoteSideDownload(page, std::chrono::steady_clock::now() - side_start,
                                     frames - gpu_write, frames - gpu_use);
                });
            }
        }
    });
}

/// bbport diagnosis: starts the fault history (BB_GPU_CHECKPOINTS=1).
void EnableFaultHistory() {
    fault_history = std::make_unique<FaultEvent[]>(FaultHistorySize);
}

/// bbport diagnosis: the recorded fault resolutions on pages of [address, address + size),
/// newest first.
void PrintFaultHistory(VAddr address, u64 size) {
    if (!fault_history) {
        return;
    }
    static constexpr const char* kinds[] = {"?", "dropped", "merged", "read back",
                                            "read back on the side queue"};
    const u64 total = fault_events.load();
    const u64 oldest = total > FaultHistorySize ? total - FaultHistorySize : 0;
    const u32 now = g_guest_frames.load();
    u32 printed = 0;
    for (u64 n = total; n > oldest && printed < 12; --n) {
        const auto& e = fault_history[(n - 1) % FaultHistorySize];
        if (e.page < address + size && address < e.page + e.length) {
            std::printf("GPU crash:       page 0x%llx+0x%x %s %u frames ago (GPU write frame %u, "
                        "last bound %u, fault frame %u)\n",
                        static_cast<unsigned long long>(e.page), e.length, kinds[std::min<u32>(e.kind, 4)],
                        now - e.frame, e.gpu_write, e.gpu_use, e.frame);
            ++printed;
        }
    }
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    BbCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::ReadBackLater(VAddr device_addr, u32 size) {
    if (!IsRegionGpuModified(device_addr, size)) {
        return; // guest memory holds the latest data
    }
    // Copies still waiting are dropped past this many (the GPU ran far behind).
    if (late_read_backs.size() >= 32) {
        staging_pool.FreeDeferred(late_read_backs.front().staging);
        late_read_backs.erase(late_read_backs.begin());
    }
    const u32 first_block = device_addr >> block_shift;
    const u32 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostCached, 0, true);
    const vk::BufferCopy copy{
        .srcOffset = device_addr - arena->cpu_addr,
        .dstOffset = staging.offset,
        .size = size,
    };
    runtime.CopyBuffer(arena, staging.buffer, std::span(&copy, 1));
    late_read_backs.push_back({staging, scheduler.CurrentTick(), device_addr, size});
}

void BufferCache::CompleteReadBacks() {
    std::erase_if(late_read_backs, [this](const LateReadBack& entry) {
        if (!scheduler.IsFree(entry.tick)) {
            return false;
        }
        entry.staging.buffer->Invalidate(entry.staging.offset, entry.size);
        // BB_READBACK_SMALL_LOG=1: the first values with what guest memory held before.
        static const bool log = [] {
            const char* env = std::getenv("BB_READBACK_SMALL_LOG");
            return env && env[0] == '1';
        }();
        static int logged = 0;
        if (log && logged < 12 && entry.size >= 16) {
            ++logged;
            float was[4], now[4];
            std::memcpy(was, std::bit_cast<const void*>(entry.address), sizeof(was));
            std::memcpy(now, entry.staging.mapped, sizeof(now));
            std::printf("Late read-back: %#llx was %g %g %g %g, now %g %g %g %g\n",
                        static_cast<unsigned long long>(entry.address), was[0], was[1], was[2],
                        was[3], now[0], now[1], now[2], now[3]);
        }
        WriteBackGpuData(std::bit_cast<u8*>(entry.address), entry.staging.mapped, entry.size);
        staging_pool.FreeDeferred(entry.staging);
        return true;
    });
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    // bbport diagnosis: BB_DOWNLOAD_CHECK=1 — GPU data written back over guest bytes that differ
    // and lie in pages the guest may have written since the last upload (CPU-modified).
    static const bool download_check = [] {
        const char* env = std::getenv("BB_DOWNLOAD_CHECK");
        return env && env[0] == '1';
    }();
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        const u8* data = download.mapped + (copy.dstOffset - download.offset);
        if (download_check) {
            const VAddr address = reinterpret_cast<VAddr>(dst_addr);
            const bool cpu_modified = memory_tracker->IsRegionCpuModified(address, copy.size);
            const bool differs = std::memcmp(dst_addr, data, copy.size) != 0;
            static std::atomic<u32> reports{0};
            if (differs && cpu_modified && reports.fetch_add(1) < 300) {
                std::printf("Download check: %#llx (%llu bytes) written back over CPU-modified "
                            "guest data that differs (request %#llx, %llu bytes)\n",
                            static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(copy.size),
                            static_cast<unsigned long long>(device_addr),
                            static_cast<unsigned long long>(size));
            }
        }
        WriteBackGpuData(dst_addr, data, copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

namespace {
// bbport diagnosis: BB_STABLE_CHECK=1 — guest data bound read-only should not change before the
// end-of-pipe fence after the draws that read it. A sample of each binding (four 64-byte
// pieces) is hashed when it is bound and again at that fence; ranges that changed are printed:
// the guest wrote them while the GPU could still read them, or finished them only after.
struct StableBinding {
    u32 size;
    u64 hash;
};
std::mutex stable_mutex;
std::unordered_map<VAddr, StableBinding> stable_bindings;
u64 stable_checked = 0, stable_changed = 0, stable_printed = 0;
auto stable_window = std::chrono::steady_clock::now();

bool StableCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_STABLE_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

u64 SampleHash(VAddr address, u32 size) {
    const auto* bytes = reinterpret_cast<const u8*>(address);
    const u32 piece = std::min<u32>(size, 64);
    const u32 offsets[4] = {0, size / 3, size / 3 * 2, size - piece};
    u64 hash = 1469598103934665603ull;
    for (const u32 offset : offsets) {
        for (u32 i = 0; i < piece; ++i) {
            hash = (hash ^ bytes[offset + i]) * 1099511628211ull;
        }
    }
    return hash;
}
} // namespace

// bbport diagnosis: BB_CLOBBER_CHECK=1 — guest memory copied to the GPU over bytes the GPU wrote
// that were never read back (skinned vertices: guest memory holds zeros there).
static bool ClobberCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_CLOBBER_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

static void ReportClobber(const char* path, VAddr address, u64 size, VAddr binding, u64 binding_size,
                          bool is_written) {
    static std::atomic<u32> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 300) {
        std::printf("Clobber check: %s %#llx (%llu bytes) over GPU-written data, binding %#llx "
                    "(%llu bytes, %s)\n",
                    path, static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(binding),
                    static_cast<unsigned long long>(binding_size), is_written ? "written" : "read");
    }
}

void BufferCache::CheckStableBindings() {
    if (!StableCheckEnabled()) {
        return;
    }
    std::scoped_lock lk{stable_mutex};
    for (const auto& [address, binding] : stable_bindings) {
        ++stable_checked;
        if (!memory->IsValidGpuMapping(address, binding.size) ||
            IsRegionGpuModified(address, binding.size)) {
            continue;
        }
        if (SampleHash(address, binding.size) != binding.hash) {
            ++stable_changed;
            if (stable_printed++ < 300) {
                std::printf("Stable check: %#llx (%u bytes) changed before its fence\n",
                            static_cast<unsigned long long>(address), binding.size);
            }
        }
    }
    stable_bindings.clear();
    const auto now = std::chrono::steady_clock::now();
    if (now - stable_window >= std::chrono::seconds(5)) {
        std::printf("Stable check: %llu of %llu bindings changed before their fence (5 s)\n",
                    static_cast<unsigned long long>(stable_changed),
                    static_cast<unsigned long long>(stable_checked));
        stable_checked = stable_changed = 0;
        stable_window = now;
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer,
                                                        u64 stream_limit) {
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    NoteGpuUse(device_addr, size);
    if (StableCheckEnabled() && !is_written && size >= 16 &&
        memory->IsValidGpuMapping(device_addr, size) && !IsRegionGpuModified(device_addr, size)) {
        std::scoped_lock lk{stable_mutex};
        stable_bindings.try_emplace(device_addr, StableBinding{size, SampleHash(device_addr, size)});
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= stream_limit && !IsRegionGpuModified(device_addr, size)) {
        NoteAsyncAccess("stream buffer", device_addr, size, false, true);
        if (ClobberCheckEnabled() && gpu_modified_ranges.Intersects(device_addr, size)) {
            ReportClobber("stream copy", device_addr, size, device_addr, size, false);
        }
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the same range streamed earlier into this command buffer, with nothing known
        // to have written guest memory since: that copy is still there (the stream buffer keeps
        // a region until the GPU has passed its tick, and this use has the same tick).
        const bool memo = !BbToggle::Disabled(BbToggle::StreamCopyMemo);
        const u64 tick = scheduler.CurrentTick();
        const u64 epoch = stream_memo_epoch.load(std::memory_order_relaxed);
        auto& entry = stream_memo[((device_addr >> 4) ^ (device_addr >> 12) ^ size) & 255];
        if (memo && entry.address == device_addr && entry.size == size && entry.tick == tick &&
            entry.lap == stream_buffer.Laps() && entry.epoch == epoch) {
            return {&stream_buffer, entry.offset};
        }
        const auto remember = [&](u64 offset) {
            if (memo) {
                entry = {device_addr, size, offset, tick, stream_buffer.Laps(), epoch};
            }
        };
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                remember(*offset);
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        remember(offset);
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    if (device_addr < arena->cpu_addr || device_addr + size > arena->cpu_addr + arena->size_bytes) {
        std::printf("Buffer cache: request 0x%llx+0x%x outside its arena 0x%llx-0x%llx\n",
                    static_cast<unsigned long long>(device_addr), size,
                    static_cast<unsigned long long>(arena->cpu_addr),
                    static_cast<unsigned long long>(arena->cpu_addr + arena->size_bytes));
    }
    EnsureResident(arena, first_block, last_block);
    if (AsyncAccessTracking()) {
        NoteAsyncAccess(is_texel_buffer ? "texel buffer" : "buffer", device_addr, size, is_written,
                        memory_tracker->IsRegionCpuModified(device_addr, size));
    }
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    // bbport diagnosis: BB_MISSED_WRITE_CHECK=1 — read-only buffers whose guest bytes changed
    // since their last use while the page tracker saw no CPU write (the GPU keeps old data).
    static const bool missed_write_check = [] {
        const char* env = std::getenv("BB_MISSED_WRITE_CHECK");
        return env && env[0] == '1';
    }();
    // Per 4 KiB page, the part inside the binding (edge pages too): pages the tracker holds
    // CPU-modified are uploaded now, the others must still match their bytes at the last check.
    const bool check_missed = missed_write_check && !is_written && size <= 4_MB &&
                              memory->IsValidGpuMapping(device_addr, size) &&
                              !IsRegionGpuModified(device_addr, size);
    const VAddr check_begin = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
    const VAddr check_end = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE);
    std::vector<bool> clean_pages;
    if (check_missed) {
        for (VAddr page = check_begin; page < check_end; page += TRACKER_BYTES_PER_PAGE) {
            clean_pages.push_back(!memory_tracker->IsRegionCpuModified(page, TRACKER_BYTES_PER_PAGE));
        }
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (check_missed) {
        static std::mutex missed_mutex;
        static std::unordered_map<VAddr, std::pair<u64, u64>> part_hash; // start -> (length, hash)
        static u64 checked = 0, missed = 0, missed_window = 0, missed_edge = 0;
        static auto window = std::chrono::steady_clock::now();
        std::scoped_lock lk{missed_mutex};
        size_t index = 0;
        for (VAddr page = check_begin; page < check_end; page += TRACKER_BYTES_PER_PAGE, ++index) {
            const VAddr part_start = std::max<VAddr>(page, device_addr);
            const VAddr part_end = std::min<VAddr>(page + TRACKER_BYTES_PER_PAGE, device_addr + size);
            const u64 length = part_end - part_start;
            const u64 hash = XXH3_64bits(reinterpret_cast<const void*>(part_start), length);
            ++checked;
            auto [it, inserted] = part_hash.try_emplace(part_start, length, hash);
            if (!inserted && it->second.first == length && it->second.second != hash &&
                clean_pages[index]) {
                const bool edge = length != TRACKER_BYTES_PER_PAGE;
                ++missed_window;
                missed_edge += edge;
                if (missed++ < 300) {
                    std::printf("Missed write: %#llx (%llu bytes, %s page, offset %llu) of "
                                "read-only buffer %#llx (%u bytes) changed with no CPU write "
                                "tracked\n",
                                static_cast<unsigned long long>(part_start),
                                static_cast<unsigned long long>(length), edge ? "edge" : "whole",
                                static_cast<unsigned long long>(part_start - device_addr),
                                static_cast<unsigned long long>(device_addr), size);
                }
            }
            it->second = {length, hash};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5)) {
            std::printf("Missed write: %llu of %llu page checks, %llu on edge pages so far (5 s)\n",
                        static_cast<unsigned long long>(missed_window),
                        static_cast<unsigned long long>(checked),
                        static_cast<unsigned long long>(missed_edge));
            checked = missed_window = 0;
            window = now;
        }
    }
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        // bbport diagnosis: BB_GPU_WRITE_WATCH=<hex address>: who marks GPU writes over it
        // (bb-probe.exe offsets of the two callers; llvm-symbolizer at 0x140000000 + offset).
        static const VAddr watch = [] {
            const char* env = std::getenv("BB_GPU_WRITE_WATCH");
            return env ? static_cast<VAddr>(std::strtoull(env, nullptr, 16)) : VAddr{0};
        }();
        static std::atomic<u32> watch_prints{0};
        if (watch && device_addr < watch + 0x10000 && watch < device_addr + size &&
            watch_prints.fetch_add(1, std::memory_order_relaxed) < 40) {
            const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
            std::printf("GPU write watch: %#llx+%#x written; callers %#llx %#llx\n",
                        static_cast<unsigned long long>(device_addr), size,
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - base),
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(__builtin_return_address(1)) - base));
        }
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    std::printf("Buffer cache: migrating arena for blocks 0x%llx-0x%llx\n",
                static_cast<unsigned long long>(first_block << block_shift),
                static_cast<unsigned long long>((last_block + 1) << block_shift));

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));
    static const bool arena_log = [] {
        const char* env = std::getenv("BB_ARENA_LOG");
        return env && env[0] == '1';
    }();
    if (arena_log) {
        char thread_name[64] = "?";
#ifdef _WIN32
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name), nullptr,
                                nullptr);
            LocalFree(description);
        }
#endif
        std::printf("Arena: thread '%s'\n", thread_name);
        std::printf("Arena: memory 0x%llx (%u blocks) for guest 0x%llx-0x%llx, arena 0x%llx, %s, "
                    "pending binds %zu\n",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                        static_cast<VkDeviceMemory>(device_memory))),
                    resident_blocks, static_cast<unsigned long long>(first_block << block_shift),
                    static_cast<unsigned long long>((last_block + 1) << block_shift),
                    static_cast<unsigned long long>(arena->cpu_addr),
                    AmdGpu::Liverpool::OnProcessorThread() ? "GPU thread" : "OTHER THREAD",
                    pending_binds.size());
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);
        g_resident_buffer_bytes.fetch_add((range.end - range.start) << block_shift,
                                          std::memory_order_relaxed);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        if ((range.start << block_shift) < arena->cpu_addr ||
            bind.resourceOffset + bind.size > arena->size_bytes) {
            std::printf("Buffer cache: resident range 0x%llx-0x%llx outside its arena 0x%llx-0x%llx "
                        "(request blocks 0x%llx-0x%llx)\n",
                        static_cast<unsigned long long>(range.start << block_shift),
                        static_cast<unsigned long long>(range.end << block_shift),
                        static_cast<unsigned long long>(arena->cpu_addr),
                        static_cast<unsigned long long>(arena->cpu_addr + arena->size_bytes),
                        static_cast<unsigned long long>(first_block << block_shift),
                        static_cast<unsigned long long>((last_block + 1) << block_shift));
        }
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    DrainSharedLandings();
    const auto add_upload = [&](VAddr addr, u64 size) {
        if (ClobberCheckEnabled() && gpu_modified_ranges.Intersects(addr, size)) {
            ReportClobber("upload", addr, size, device_addr, size, is_written);
        }
        // A shared page's GPU bytes are not uploaded before they have landed (ShareGpuPage),
        // except those the guest has changed since: the GPU gets them now, as it got the whole
        // page after a read-back.
        if (shared_gpu_ranges.Intersects(addr, size)) {
            const auto add = [&](VAddr lo, u64 part) {
                copies.emplace_back(total_size_bytes, lo, part);
                total_size_bytes += part;
            };
            shared_gpu_ranges.ForEachNotInRange(addr, size, add);
            std::vector<u8> now;
            shared_gpu_ranges.ForEachInRange(addr, size, [&](VAddr lo, VAddr hi) {
                auto it = shared_before.upper_bound(lo);
                if (it != shared_before.begin()) {
                    --it;
                }
                for (; it != shared_before.end() && it->first < hi; ++it) {
                    const VAddr piece_end = it->first + it->second.size();
                    const VAddr from = std::max(lo, it->first);
                    const VAddr to = std::min<VAddr>(hi, piece_end);
                    if (from >= to) {
                        continue;
                    }
                    now.resize(to - from);
                    memory->CopySparseMemory(from, now.data(), to - from);
                    const u8* before = it->second.data() + (from - it->first);
                    ForEachSharedBlockRun(from, now.data(), before, to - from, true,
                                          [&](u64 at, u64 bytes) { add(from + at, bytes); });
                }
            });
            return;
        }
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    };
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            add_upload(addr, size);
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SyncImageToBuffer(VAddr addr, u32 size) {
    if (!texture_cache.FindImageFromRange(addr, size)) {
        return false;
    }
    const auto [arena, offset] = ObtainBuffer(addr, size, true);
    return SynchronizeMemoryFromImage(arena, addr, size);
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::DumpArenas() const {
    std::printf("GPU crash: BDA page table at GPU 0x%llx, %zu arenas, block %u bytes\n",
                static_cast<unsigned long long>(bda_pagetable_buffer->BufferDeviceAddress()),
                arenas.size(), 1u << block_shift);
    for (const auto& arena : arenas) {
        const u64 gpu = arena.BufferDeviceAddress();
        std::printf("GPU crash: arena guest 0x%llx-0x%llx = GPU 0x%llx-0x%llx\n",
                    static_cast<unsigned long long>(arena.cpu_addr),
                    static_cast<unsigned long long>(arena.cpu_addr + arena.size_bytes),
                    static_cast<unsigned long long>(gpu),
                    static_cast<unsigned long long>(gpu + arena.size_bytes));
    }
    u64 ranges = 0;
    resident_ranges.ForEachInRange(0, ~u64{0} >> block_shift, [&](const Backing& backing) {
        if (ranges++ < 64) {
            std::printf("GPU crash: resident guest 0x%llx-0x%llx\n",
                        static_cast<unsigned long long>(backing.start << block_shift),
                        static_cast<unsigned long long>(backing.end << block_shift));
        }
    });
    std::printf("GPU crash: %llu resident ranges\n", static_cast<unsigned long long>(ranges));
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    // bbport: BB_SPARSE_IDLE=1: the arena's page mappings change only while the GPU runs none of
    // the work submitted before (diagnosis of device-lost faults in freshly bound memory).
    static const bool sparse_idle = [] {
        const char* env = std::getenv("BB_SPARSE_IDLE");
        return env && env[0] == '1';
    }();
    if (sparse_idle) {
        // Called inside Scheduler::SubmitExecution (queue lock held), so the queue is ours.
        (void)instance.GetGraphicsQueue().waitIdle();
    }
    vk::Result submit_result;
    {
        std::scoped_lock queue_lk{Vulkan::g_graphics_queue_mutex};
        submit_result = instance.GetSparseQueue().bindSparse(sparse_info);
    }
    if (submit_result == vk::Result::eErrorDeviceLost) {
        std::printf("Device lost during a sparse bind\n");
        Vulkan::DeviceLost(instance.GetDevice());
    }
    static const bool arena_log = [] {
        const char* env = std::getenv("BB_ARENA_LOG");
        return env && env[0] == '1';
    }();
    if (arena_log) {
        for (const auto& binds : pending_binds) {
            for (const auto& bind : binds.binds) {
                std::printf("Arena: bound memory 0x%llx+0x%llx at arena 0x%llx offset 0x%llx size 0x%llx "
                            "(tick %llu, %s)\n",
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                                static_cast<VkDeviceMemory>(bind.memory))),
                            static_cast<unsigned long long>(bind.memoryOffset),
                            static_cast<unsigned long long>(binds.arena->cpu_addr),
                            static_cast<unsigned long long>(bind.resourceOffset),
                            static_cast<unsigned long long>(bind.size),
                            static_cast<unsigned long long>(signal_tick),
                            vk::to_string(submit_result).c_str());
            }
        }
    }

    pending_binds.clear();
}

} // namespace VideoCore
