// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_trace.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <x86intrin.h>
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <boost/container/small_vector.hpp>
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
extern "C" int runtime_thread_is_guest(void); // runtime_thread.c

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

/// BB_FRAME_TRACE diagnosis: 1 while the texture cache changes watches (image pages).
static thread_local u64 t_protect_origin = 0;

// bbport: write revokes of pages an upload clears (RegionManager::ForEachModifiedRange) are
// queued instead of made on the calling thread, and a thread of their own (bb:Protect) makes
// them, adjacent ones in one call: a new area's first frame (U3's desert canyon) made ~760 calls
// of a few pages each, ~3-4 ms of the draw recording thread, the frame's longest chain (moved to
// the copies, they cost the Vulkan recording thread the same, the next in that chain). A page
// stays writable until then, which is safe: the copy of it starts later and takes what the guest
// wrote meanwhile; a copy that reads queued or in-flight pages (MemoryManager::CopySparseMemory)
// first waits for them. Every other change (a grant after a fault, read protection, unmap) first
// makes the queued ones, under the same lock, so the host's protection follows the order the
// caches asked for it. BB_DEFER_REVOKES=1: on. Off by default: the desert ride's hitches were not
// shorter with it in clean rides (2026-11-18).
thread_local bool t_defer_revoke = false;
static bool DeferRevokes() {
    static const bool on = [] {
        const char* env = std::getenv("BB_DEFER_REVOKES");
        return env && env[0] == '1';
    }();
    return on;
}
struct DeferredRevokes {
    std::mutex queue_mutex;                      ///< ranges, bounds, count
    std::mutex apply_mutex;                      ///< held while host protection changes
    std::condition_variable queue_cv;            ///< wakes bb:Protect
    std::vector<std::pair<VAddr, VAddr>> ranges; ///< [begin, end), in request order
    std::atomic<u32> count{0};
    std::atomic<VAddr> lo{~VAddr{0}}, hi{0};     ///< bounds of the queued ranges
    std::atomic<VAddr> flight_lo{~VAddr{0}}, flight_hi{0}; ///< ...of the ones being made
    bool thread_started = false;                 ///< under queue_mutex
};
static DeferredRevokes g_deferred;
static void ApplyRanges(std::vector<std::pair<VAddr, VAddr>>& ranges);

/// With apply_mutex held: makes every queued revoke.
static void DrainDeferredLocked() {
    std::vector<std::pair<VAddr, VAddr>> ranges;
    {
        std::scoped_lock lk{g_deferred.queue_mutex};
        if (g_deferred.ranges.empty()) {
            return;
        }
        ranges.swap(g_deferred.ranges);
        // In flight before no longer queued: a copy that checks in the other order sees one.
        g_deferred.flight_lo.store(g_deferred.lo.load(std::memory_order_relaxed),
                                   std::memory_order_release);
        g_deferred.flight_hi.store(g_deferred.hi.load(std::memory_order_relaxed),
                                   std::memory_order_release);
        g_deferred.lo.store(~VAddr{0}, std::memory_order_release);
        g_deferred.hi.store(0, std::memory_order_release);
        g_deferred.count.store(0, std::memory_order_release);
    }
    ApplyRanges(ranges);
    g_deferred.flight_lo.store(~VAddr{0}, std::memory_order_release);
    g_deferred.flight_hi.store(0, std::memory_order_release);
}

void FlushDeferredProtects() {
    if (!DeferRevokes() || (g_deferred.count.load(std::memory_order_acquire) == 0 &&
                            g_deferred.flight_hi.load(std::memory_order_acquire) == 0)) {
        return;
    }
    std::scoped_lock lk{g_deferred.apply_mutex};
    DrainDeferredLocked();
}

void FlushDeferredProtects(VAddr address, u64 size) {
    // Only a copy that reads queued or in-flight pages waits: the copies of other memory (the
    // constant ring's, the recording thread's small ones) go on.
    if (!DeferRevokes()) {
        return;
    }
    const VAddr end = address + size;
    const bool queued = g_deferred.count.load(std::memory_order_acquire) != 0 &&
                        address < g_deferred.hi.load(std::memory_order_acquire) &&
                        g_deferred.lo.load(std::memory_order_acquire) < end;
    const bool flying = address < g_deferred.flight_hi.load(std::memory_order_acquire) &&
                        g_deferred.flight_lo.load(std::memory_order_acquire) < end;
    if (!queued && !flying) {
        return;
    }
    std::scoped_lock lk{g_deferred.apply_mutex};
    DrainDeferredLocked();
}

static void DeferredRevokeThread() {
    Common::SetCurrentThreadName("bb:Protect");
    for (;;) {
        {
            std::unique_lock lk{g_deferred.queue_mutex};
            g_deferred.queue_cv.wait(lk, [] { return !g_deferred.ranges.empty(); });
        }
        // At once: ahead of the copies (the Vulkan recording thread runs them a chunk behind),
        // with whatever queued meanwhile.
        std::scoped_lock lk{g_deferred.apply_mutex};
        DrainDeferredLocked();
    }
}

/// Queues a revoke (of a range the caller's upload copies next).
static void QueueRevoke(VAddr address, u64 size) {
    bool wake;
    {
        std::scoped_lock lk{g_deferred.queue_mutex};
        auto& ranges = g_deferred.ranges;
        wake = ranges.empty();
        if (!ranges.empty() && ranges.back().second == address) {
            ranges.back().second = address + size;
        } else {
            ranges.push_back({address, address + size});
        }
        if (address < g_deferred.lo.load(std::memory_order_relaxed)) {
            g_deferred.lo.store(address, std::memory_order_release);
        }
        if (address + size > g_deferred.hi.load(std::memory_order_relaxed)) {
            g_deferred.hi.store(address + size, std::memory_order_release);
        }
        g_deferred.count.store(static_cast<u32>(ranges.size()), std::memory_order_release);
        if (!g_deferred.thread_started) {
            g_deferred.thread_started = true;
            std::thread(DeferredRevokeThread).detach();
        }
    }
    if (wake) {
        g_deferred.queue_cv.notify_one();
    }
}

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    // bbport diagnosis: BB_READ_PROBE=1 (with BB_READBACKS=2, which denies reads of GPU-written
    // pages): a read is not served with GPU data (that waited for the GPU thread and froze at
    // load) but recorded, and the page made readable (writes stay tracked); the guest reads
    // what its memory holds, as without BB_READBACKS. Every 5 s the most read pages and the
    // code reading them are printed.
    static bool ReadProbe() {
        static const bool enabled = [] {
            const char* env = std::getenv("BB_READ_PROBE");
            return env && env[0] == '1';
        }();
        return enabled;
    }

    static void NoteProbedRead(void* context, VAddr addr, bool port_thread) {
        const VAddr page = addr & ~VAddr{4095};
        // Writes stay as the tracker keeps them: allowed on pages it holds CPU-modified (it
        // expects no fault there), trapped on the others.
        const bool writable = rasterizer->GetBufferCache().IsRegionCpuModified(page, 4096);
        FlushDeferredProtects();
        runtime_memory_gpu_protect(page, 4096, 1, writable ? 1 : 0);
        const VAddr rip = reinterpret_cast<VAddr>(Common::GetRip(context));
#ifdef _WIN32
        // The caller of a library copy (memcpy is a leaf: its return address is at rsp), and
        // the thread's name, read once per thread.
        const VAddr caller =
            *reinterpret_cast<const VAddr*>(reinterpret_cast<EXCEPTION_POINTERS*>(context)
                                                ->ContextRecord->Rsp);
        static thread_local std::string thread_name = [] {
            std::string name = "?";
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
                char buffer[64];
                WideCharToMultiByte(CP_UTF8, 0, description, -1, buffer, sizeof(buffer), nullptr,
                                    nullptr);
                name = buffer;
                LocalFree(description);
            }
            return name;
        }();
#else
        const VAddr caller = 0;
        static thread_local std::string thread_name = "?";
#endif
        static std::mutex mutex;
        static std::unordered_map<VAddr, u32> pages, code;
        static std::map<std::pair<std::string, VAddr>, u32> callers;
        static u64 reads = 0, port_reads = 0;
        static auto window = std::chrono::steady_clock::now();
        // Never waits: a thread holding the runtime's region lock may fault here as well.
        std::unique_lock lk{mutex, std::try_to_lock};
        if (!lk.owns_lock()) {
            return;
        }
        ++reads;
        port_reads += port_thread;
        ++pages[page];
        ++code[rip];
        ++callers[{thread_name, caller}];
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(5)) {
            return;
        }
        window = now;
        const auto top = [](const std::unordered_map<VAddr, u32>& map, size_t n) {
            std::vector<std::pair<VAddr, u32>> v(map.begin(), map.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            v.resize(std::min(v.size(), n));
            return v;
        };
        std::printf("Read probe: %llu reads of GPU-written pages (%llu by port threads), %zu "
                    "pages, %zu code sites (5 s)\n",
                    static_cast<unsigned long long>(reads),
                    static_cast<unsigned long long>(port_reads), pages.size(), code.size());
        for (const auto& [where, n] : top(code, 12)) {
            std::printf("Read probe:   code %#llx: %u\n", static_cast<unsigned long long>(where), n);
        }
        for (const auto& [where, n] : top(pages, 12)) {
            std::printf("Read probe:   page %#llx: %u\n", static_cast<unsigned long long>(where), n);
        }
        std::vector<std::pair<std::pair<std::string, VAddr>, u32>> by_caller(callers.begin(),
                                                                             callers.end());
        std::sort(by_caller.begin(), by_caller.end(),
                  [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < std::min<size_t>(by_caller.size(), 12); ++i) {
            const auto& [key, n] = by_caller[i];
            char module[MAX_PATH] = "?";
#ifdef _WIN32
            HMODULE handle{};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(key.second), &handle)) {
                GetModuleFileNameA(handle, module, sizeof(module));
            } else {
                std::snprintf(module, sizeof(module), "guest or JIT");
            }
#endif
            std::printf("Read probe:   caller %#llx [%s] thread '%s': %u\n",
                        static_cast<unsigned long long>(key.second), module, key.first.c_str(), n);
        }
        callers.clear();
        pages.clear();
        code.clear();
        reads = port_reads = 0;
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        // bbport: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            BbStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (Common::IsWriteError(context)) {
            BbStats::Timer timer{BbStats::t_write_faults};
            BbTrace::Record(BbTrace::FaultBegin, addr);
            BbTrace::Scope trace_end{BbTrace::FaultEnd};
            return rasterizer->OnWriteFault(addr, is_gpu_thread);
        } else {
            BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
            if (ReadProbe()) {
                // Reads outside GPU-mapped memory are not ours (speculative reads of the draw
                // preparation, SRT walkers): they go on to their own handlers.
                if (!rasterizer->IsMapped(addr, 8)) {
                    return false;
                }
                NoteProbedRead(context, addr, is_gpu_thread);
                return true;
            }
            // bbport: with precise read-backs (BB_READBACKS=2) the port's own worker threads read
            // guest memory too (the Vulkan recording thread's small guest copies): a read-back
            // there waited for the GPU command thread, which was draining the draw pipe, whose
            // recording thread waited for those copies (U2 froze). They read guest memory as it
            // is, as without precise read-backs; the game's threads get the GPU data.
            if (!is_gpu_thread && !runtime_thread_is_guest()) {
                // A read of memory that isn't mapped (any more) is not a tracked page: re-protecting
                // it changes nothing and the thread faulted again forever (review 2026-11-01). It
                // goes on to the speculative-read recovery or the crash report instead.
                if (!rasterizer->IsMapped(addr, 1)) {
                    return false;
                }
                const VAddr page = addr & ~VAddr{4095};
                const bool writable = rasterizer->GetBufferCache().IsRegionCpuModified(page, 4096);
                FlushDeferredProtects();
                runtime_memory_gpu_protect(page, 4096, 1, writable ? 1 : 0);
                return true;
            }
            BbStats::Timer timer{BbStats::t_read_faults};
            return rasterizer->ReadMemory(addr, 8, is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// bbport: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. BB_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("bb:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                BbStats::Timer timer{BbStats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        // bbport diagnosis (perf10, BB_PROTECT_STATS=1): protection changes per second, on the
        // draw recording thread (stage B) and elsewhere, with their pages and time.
        static const bool stats = [] {
            const char* env = std::getenv("BB_PROTECT_STATS");
            return env && env[0] == '1';
        }();
        const u64 t0 = stats ? __rdtsc() : 0;
        if (DeferRevokes()) {
            if (t_defer_revoke && perms == Core::MemoryPermission::Read) {
                QueueRevoke(address, size);
                return;
            }
            // Every other change comes after the revokes queued before it.
            std::scoped_lock lk{g_deferred.apply_mutex};
            DrainDeferredLocked();
            ProtectTraced(address, size, perms);
        } else {
            ProtectTraced(address, size, perms);
        }
        if (stats) {
            static std::atomic<u64> calls[2][2]{}, pages[2]{}, cycles[2]{};
            static std::atomic<u64> window{0};
            const u32 b = Vulkan::DrawPipe::OnStageB() ? 1 : 0;
            const u32 w = True(perms & Core::MemoryPermission::Write) ? 1 : 0;
            calls[b][w].fetch_add(1, std::memory_order_relaxed);
            pages[b].fetch_add((size + 4095) >> 12, std::memory_order_relaxed);
            const u64 now = __rdtsc();
            cycles[b].fetch_add(now - t0, std::memory_order_relaxed);
            u64 start = window.load(std::memory_order_relaxed);
            if (start == 0) {
                window.compare_exchange_strong(start, now);
            } else if (now - start > 5'000'000'000ull && window.compare_exchange_strong(start, now)) {
                // Per 5e9 TSC ticks (~1.3-1.7 s on 3-4 GHz parts): rates are per window, the
                // cycle share = cycles / window ticks.
                const double ticks = double(now - start);
                double v[2][4];
                for (u32 t = 0; t < 2; ++t) {
                    v[t][0] = double(calls[t][0].exchange(0));
                    v[t][1] = double(calls[t][1].exchange(0));
                    v[t][2] = double(pages[t].exchange(0));
                    v[t][3] = 100.0 * double(cycles[t].exchange(0)) / ticks;
                }
                std::printf("Protect stats (per 5e9 TSC ticks): stage B %.0f protect + %.0f unprotect, "
                            "%.0f pages, %.2f%% of a core; other threads %.0f protect + %.0f unprotect, "
                            "%.0f pages, %.2f%% of a core\n",
                            v[1][0], v[1][1], v[1][2], v[1][3], v[0][0], v[0][1], v[0][2], v[0][3]);
                std::fflush(stdout);
            }
        }
    }

    static void ProtectTraced(VAddr address, size_t size, Core::MemoryPermission perms) {
        // arg: first page (28 bits) | pages << 28 | write granted << 62
        BbTrace::Record(BbTrace::ProtectBegin,
                        ((address >> 12) & 0xfffffff) | ((((size + 4095) >> 12) & 0x3ffffffff) << 28) |
                            (u64(True(perms & Core::MemoryPermission::Write)) << 62) |
                            (t_protect_origin << 63));
        ProtectInner(address, size, perms);
        BbTrace::Record(BbTrace::ProtectEnd);
    }

    static void ProtectInner(VAddr address, size_t size, Core::MemoryPermission perms) {
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        // bbport: with read tracking (BB_READBACKS=2) a page both GPU- and CPU-modified (the
        // merge path) asks for write-only, which hosts cannot map: it stays fully accessible
        // (its reads go untracked, as without read tracking).
        if (perms == Core::MemoryPermission::Write) {
            perms = Core::MemoryPermission::ReadWrite;
        }
        impl.Protect(address, size, perms);
    }

};

static void ApplyRanges(std::vector<std::pair<VAddr, VAddr>>& ranges) {
    if (ranges.empty()) {
        return;
    }
    std::sort(ranges.begin(), ranges.end());
    VAddr begin = ranges[0].first, end = ranges[0].second;
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first <= end) {
            end = std::max(end, ranges[i].second);
            continue;
        }
        SignalImpl::ProtectTraced(begin, end - begin, Core::MemoryPermission::Read);
        begin = ranges[i].first;
        end = ranges[i].second;
    }
    SignalImpl::ProtectTraced(begin, end - begin, Core::MemoryPermission::Read);
}

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (std::getenv("BB_UFFD") && std::getenv("BB_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    FlushDeferredProtects();
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    FlushDeferredProtects();
    impl->OnUnmap(address, size);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    t_protect_origin = 1;
    impl->UpdatePageWatchers<track, false>(addr, size);
    t_protect_origin = 0;
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
