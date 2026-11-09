// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// bbport diagnosis (perf10 step 0, BB_GUEST_SYNC_STATS=1, off by default): the guest's own GPU
// sync points in the PM4 stream (partial flushes, cache flushes, ACQUIRE_MEM, RELEASE_MEM,
// EOP/EOS, WAIT_REG_MEM) against the barriers our tracker emits. Question: could barriers be
// placed only where the game syncs (guest-driven barriers) instead of tracking every buffer?
//
// Stage A counts the packets by kind and ring and hands each sync point to the recording thread
// in order (a task packet), so stage B knows how many guest sync points came before each of its
// barriers. A memory barrier of ours with no guest sync point since our previous barrier is a
// hazard the game did not sync for (or one the tracker over-reports): guest-driven barriers
// alone would miss it. One summary line per second.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "common/types.h"

namespace Vulkan::GuestSync {

inline bool On() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GUEST_SYNC_STATS");
        return env && env[0] == '1';
    }();
    return on;
}

/// perf10 step 4, MEASUREMENT ONLY, UNSAFE (BB_GUEST_BARRIERS=1): guest shader buffer bindings
/// skip the barrier tracker; a strong guest sync point, a switch between draws and dispatches,
/// or any host transfer emits one global barrier when a guest shader stored since the last one.
/// Gives the frame-rate ceiling of guest-driven barriers; storage written and read with no
/// strong guest sync between (merged async compute) is not ordered.
inline bool GuestBarriers() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GUEST_BARRIERS");
        return env && env[0] == '1';
    }();
    return on;
}

/// Packet kinds counted in stage A (per ring class: 0 graphics, 1 compute queues).
enum Kind : u32 {
    EwCsPartial,
    EwPsPartial,
    EwVsPartial,
    EwCacheFlush, ///< CACHE_FLUSH*, FLUSH_AND_INV_*, DB_CACHE_FLUSH_AND_INV
    EwOther,      ///< other EVENT_WRITEs (occlusion, streamout, ...): not a sync point
    Eop,
    Eos,
    AcquireMem,
    ReleaseMem,
    WaitRegMem,
    DmaData, ///< a transfer, not counted as a sync point
    KindCount
};

inline constexpr const char* KindNames[KindCount] = {"cs_pf", "ps_pf", "vs_pf", "cflush", "ew_oth",
                                                     "eop",   "eos",   "acq",   "rel",    "wrm",
                                                     "dma"};

inline bool IsSyncPoint(Kind kind) {
    return kind != EwOther && kind != DmaData;
}

/// Sync points that order later work in the queue (a wait or a cache action); EOP/EOS/RELEASE_MEM
/// only signal when earlier work is done.
inline bool IsStrong(Kind kind) {
    return kind == EwCsPartial || kind == EwPsPartial || kind == EwVsPartial ||
           kind == EwCacheFlush || kind == AcquireMem || kind == WaitRegMem;
}

/// Flush sites (stage B): what asked for our barrier.
enum Site : u32 { SiteOther, SiteDraw, SiteDispatch, SiteBatch, SiteCap, SiteSubmit, SiteCount };
inline constexpr const char* SiteNames[SiteCount] = {"host", "draw", "disp", "batch", "cap", "submit"};

struct Counters {
    // stage A
    std::array<std::array<std::atomic<u64>, KindCount>, 2> a_kinds{};
    // stage B (the recording thread; atomics because a task may run inline on stage A)
    std::atomic<u64> epoch{0};          ///< guest sync points seen in order
    std::atomic<u64> b_syncs{0};        ///< this window
    std::atomic<u64> b_syncs_work{0};   ///< ... with at least one draw/dispatch since the previous
    std::atomic<u64> draws{0}, dispatches{0};
    std::atomic<u64> work_since_sync{0};
    std::array<std::atomic<u64>, 6> hist{}; ///< work per interval: 0, 1, 2-4, 5-16, 17-64, 65+
    u64 epoch_at_flush = 0;
    // the same for strong sync points only
    std::atomic<u64> strong_epoch{0}, s_syncs{0}, s_syncs_work{0}, work_since_strong{0};
    std::array<std::atomic<u64>, 6> s_hist{};
    u64 strong_at_flush = 0;
    std::atomic<u64> s_mem_after{0}, s_mem_none{0};
    std::array<std::atomic<u64>, SiteCount> s_none_site{};
    // push descriptors (perf10 idea 3): pushes, ones equal to the last push on the same bind point
    // and layout, writes and descriptors per push
    std::atomic<u64> pushes{0}, same_pushes{0}, push_writes{0}, push_descs{0};
    // write-filtered guest barriers (Ryujinx/Xenia style): a guest sync point queues a request; the
    // next draw/dispatch emits one global barrier only if something stored (shader buffer/image
    // writes, our copies/clears) since the last one emitted. [0] all sync points, [1] strong only.
    bool wf_pending[2]{}, wf_wrote[2]{};
    std::atomic<u64> wf_emitted[2]{}, wf_dropped[2]{};
    std::array<std::atomic<u64>, SiteCount> mem_after_sync{}, mem_no_sync{};
    std::atomic<u64> image_only{0}, image_barriers{0}, mem_with_images{0};
    std::atomic<u64> dispatch_after_sync{0}; ///< dispatches that are the first work after a sync
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};

inline Counters& Get() {
    static Counters c;
    return c;
}

inline void NoteStageA(u32 ring, Kind kind) {
    Get().a_kinds[ring & 1][kind].fetch_add(1, std::memory_order_relaxed);
}

inline void Report();

/// Stage B: a store is recorded (shader buffer/image write, copy, clear).
inline void NoteWrite() {
    auto& c = Get();
    c.wf_wrote[0] = c.wf_wrote[1] = true;
}

/// Stage B, in order: a guest sync point.
inline u32 Bucket(u64 w) {
    return w == 0 ? 0 : w == 1 ? 1 : w <= 4 ? 2 : w <= 16 ? 3 : w <= 64 ? 4 : 5;
}

inline void NoteSyncB(Kind kind) {
    auto& c = Get();
    c.wf_pending[0] = true;
    if (IsStrong(kind)) {
        c.wf_pending[1] = true;
    }
    if (IsStrong(kind)) {
        c.strong_epoch.fetch_add(1, std::memory_order_relaxed);
        c.s_syncs.fetch_add(1, std::memory_order_relaxed);
        const u64 w = c.work_since_strong.exchange(0, std::memory_order_relaxed);
        c.s_syncs_work.fetch_add(w ? 1 : 0, std::memory_order_relaxed);
        c.s_hist[Bucket(w)].fetch_add(1, std::memory_order_relaxed);
    }
    c.epoch.fetch_add(1, std::memory_order_relaxed);
    c.b_syncs.fetch_add(1, std::memory_order_relaxed);
    const u64 w = c.work_since_sync.exchange(0, std::memory_order_relaxed);
    c.b_syncs_work.fetch_add(w ? 1 : 0, std::memory_order_relaxed);
    c.hist[Bucket(w)].fetch_add(1, std::memory_order_relaxed);
    Report();
}

/// Stage B: a draw or dispatch is recorded.
inline void NoteWork(bool dispatch) {
    auto& c = Get();
    (dispatch ? c.dispatches : c.draws).fetch_add(1, std::memory_order_relaxed);
    c.work_since_strong.fetch_add(1, std::memory_order_relaxed);
    for (u32 i = 0; i < 2; ++i) {
        if (c.wf_pending[i]) {
            c.wf_pending[i] = false;
            if (c.wf_wrote[i]) {
                c.wf_wrote[i] = false;
                c.wf_emitted[i].fetch_add(1, std::memory_order_relaxed);
            } else {
                c.wf_dropped[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (c.work_since_sync.fetch_add(1, std::memory_order_relaxed) == 0 && dispatch) {
        c.dispatch_after_sync.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Stage B: Runtime::FlushBarriers emits a barrier.
inline void NoteFlush(u32 site, bool memory, u32 images) {
    auto& c = Get();
    const u64 epoch = c.epoch.load(std::memory_order_relaxed);
    if (images) {
        c.image_barriers.fetch_add(images, std::memory_order_relaxed);
    }
    if (!memory) {
        c.image_only.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (images) {
        c.mem_with_images.fetch_add(1, std::memory_order_relaxed);
    }
    site = site < SiteCount ? site : SiteOther;
    (epoch != c.epoch_at_flush ? c.mem_after_sync : c.mem_no_sync)[site].fetch_add(
        1, std::memory_order_relaxed);
    c.epoch_at_flush = epoch;
    const u64 strong = c.strong_epoch.load(std::memory_order_relaxed);
    if (strong != c.strong_at_flush) {
        c.s_mem_after.fetch_add(1, std::memory_order_relaxed);
    } else {
        c.s_mem_none.fetch_add(1, std::memory_order_relaxed);
        c.s_none_site[site].fetch_add(1, std::memory_order_relaxed);
    }
    c.strong_at_flush = strong;
}

inline void Report() {
    auto& c = Get();
    const auto now = std::chrono::steady_clock::now();
    if (now - c.window < std::chrono::seconds(1)) {
        return;
    }
    const double s = std::chrono::duration<double>(now - c.window).count();
    c.window = now;
    const auto take = [s](std::atomic<u64>& v) { return double(v.exchange(0)) / s; };
    char buf[3072];
    int n = std::snprintf(buf, sizeof(buf), "Guest sync (/s):");
    for (u32 ring = 0; ring < 2; ++ring) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " %s", ring ? "| asc" : "gfx");
        for (u32 k = 0; k < KindCount; ++k) {
            n += std::snprintf(buf + n, sizeof(buf) - n, " %s %.0f", KindNames[k],
                               take(c.a_kinds[ring][k]));
        }
    }
    const double syncs = take(c.b_syncs), syncs_work = take(c.b_syncs_work);
    const double draws = take(c.draws), dispatches = take(c.dispatches);
    n += std::snprintf(buf + n, sizeof(buf) - n,
                       " || B: sync points %.0f (%.0f after work), draws %.0f, dispatches %.0f "
                       "(%.0f first after a sync), work per interval [0 1 2-4 5-16 17-64 65+]",
                       syncs, syncs_work, draws, dispatches, take(c.dispatch_after_sync));
    for (auto& h : c.hist) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " %.0f", take(h));
    }
    double after = 0, none = 0;
    char sites[512];
    int m = 0;
    sites[0] = 0;
    for (u32 i = 0; i < SiteCount; ++i) {
        const double a = take(c.mem_after_sync[i]), b = take(c.mem_no_sync[i]);
        after += a;
        none += b;
        m += std::snprintf(sites + m, sizeof(sites) - m, " %s %.0f/%.0f", SiteNames[i], a, b);
    }
    n += std::snprintf(buf + n, sizeof(buf) - n,
                       " || ours: memory barriers %.0f (guest synced since our last %.0f, not %.0f;"
                       " by site synced/not:%s), +images %.0f, image-only %.0f, image barriers %.0f\n",
                       after + none, after, none, sites, take(c.mem_with_images),
                       take(c.image_only), take(c.image_barriers));
    n -= 1; // drop the newline
    n += std::snprintf(buf + n, sizeof(buf) - n,
                       " || write-filtered: all %.0f emitted (%.0f dropped), strong %.0f (%.0f dropped)",
                       take(c.wf_emitted[0]), take(c.wf_dropped[0]), take(c.wf_emitted[1]),
                       take(c.wf_dropped[1]));
    const double pushes = take(c.pushes);
    n += std::snprintf(buf + n, sizeof(buf) - n,
                       " || pushes %.0f (same as last %.0f, %.1f writes, %.1f descriptors each)", pushes,
                       take(c.same_pushes), pushes ? take(c.push_writes) / pushes : 0.0,
                       pushes ? take(c.push_descs) / pushes : 0.0);
    n += std::snprintf(buf + n, sizeof(buf) - n,
                       " || strong: sync points %.0f (%.0f after work), per interval", take(c.s_syncs),
                       take(c.s_syncs_work));
    for (auto& h : c.s_hist) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " %.0f", take(h));
    }
    n += std::snprintf(buf + n, sizeof(buf) - n, ", ours after a strong one %.0f, not %.0f (by site",
                       take(c.s_mem_after), take(c.s_mem_none));
    for (u32 i = 0; i < SiteCount; ++i) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " %s %.0f", SiteNames[i], take(c.s_none_site[i]));
    }
    n += std::snprintf(buf + n, sizeof(buf) - n, ")\n");
    std::fputs(buf, stdout);
    std::fflush(stdout);
}

} // namespace Vulkan::GuestSync
