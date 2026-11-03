// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport (perf6 phase 0, BB_FENCE_STATS=1): how long the guest waits for its compute fences, split by
// pipeline link. Each compute queue RELEASE_MEM is timed at four points:
//   submit  the guest submitted the command buffer that holds it (SubmitAsc)
//   decode  stage A reached the packet (ProcessCompute)
//   run     stage B (the recording thread) ran it (RunReleaseMem)
//   signal  the label was written and the interrupt raised (SignalReleaseMem; after the GPU work
//           and the copies before it landed)
// Every 5 s: "Fence stats: N/s; submit->decode, decode->run, run->signal, total: mean/p95 us".
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace AmdGpu::FenceStats {

inline bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_FENCE_STATS");
        return env && env[0] == '1';
    }();
    return on;
}

inline u64 Now() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

/// Submit times per compute queue, in submit order (the queue runs its submits in that order).
struct SubmitTimes {
    std::mutex lock;
    std::array<std::deque<u64>, 64> queues;
};
inline SubmitTimes& Submits() {
    static SubmitTimes times;
    return times;
}

/// Queue depth seen by each new submit, and stage A's busy time per compute submit (resume to done,
/// summed over its resumes), for the report.
struct Depths {
    std::mutex lock;
    u64 depth_sum = 0, submits = 0, busy_ns = 0, done = 0;
};
inline Depths& DepthLog() {
    static Depths d;
    return d;
}
inline void NoteSubmit(u32 vqid) {
    if (!Enabled() || vqid >= 64) return;
    auto& s = Submits();
    size_t depth;
    {
        std::scoped_lock lk{s.lock};
        depth = s.queues[vqid].size();
        s.queues[vqid].push_back(Now());
    }
    auto& d = DepthLog();
    std::scoped_lock lk{d.lock};
    d.depth_sum += depth;
    ++d.submits;
}
inline void NoteBusy(u64 ns, bool done) {
    auto& d = DepthLog();
    std::scoped_lock lk{d.lock};
    d.busy_ns += ns;
    d.done += done ? 1 : 0;
}

/// The submit time of the command buffer the queue starts processing now (0: unknown).
inline u64 TakeSubmit(u32 vqid) {
    if (!Enabled() || vqid >= 64) return 0;
    auto& s = Submits();
    std::scoped_lock lk{s.lock};
    auto& q = s.queues[vqid];
    if (q.empty()) return 0;
    const u64 t = q.front();
    q.pop_front();
    return t;
}

/// Compute queue WAIT_REG_MEMs that were not met at once: per address, how long, and which
/// compute pipe's RELEASE_MEM wrote the address last (-1: none seen, so a graphics EOP or the CPU).
struct Waits {
    std::mutex lock;
    struct Entry {
        u64 ns = 0;
        u32 count = 0;
    };
    std::unordered_map<u64, Entry> by_address;
    std::unordered_map<u64, int> fence_writer; // address -> pipe id of the last RELEASE_MEM
    u64 total_ns = 0;
    u32 total = 0;
};
inline Waits& WaitLog() {
    static Waits waits;
    return waits;
}
inline void NoteFenceWrite(u64 address, u32 pipe_id) {
    if (!Enabled()) return;
    auto& w = WaitLog();
    std::scoped_lock lk{w.lock};
    w.fence_writer[address] = int(pipe_id);
}
inline void NoteWait(u64 address, u64 ns) {
    auto& w = WaitLog();
    std::scoped_lock lk{w.lock};
    auto& e = w.by_address[address];
    e.ns += ns;
    ++e.count;
    w.total_ns += ns;
    ++w.total;
}
inline void ReportWaits(double secs) {
    auto& w = WaitLog();
    std::scoped_lock lk{w.lock};
    std::vector<std::pair<u64, Waits::Entry>> top(w.by_address.begin(), w.by_address.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::printf("Fence stats: compute queue waits not met at once: %.0f/s, %.0f us/s waiting, mean %.0f us",
                double(w.total) / secs, double(w.total_ns) / 1000.0 / secs,
                w.total ? double(w.total_ns) / 1000.0 / w.total : 0.0);
    for (size_t i = 0; i < std::min<size_t>(3, top.size()); ++i) {
        const auto writer = w.fence_writer.find(top[i].first);
        std::printf("; %#llx %u waits %.0f us (written by %s%d)", (unsigned long long)top[i].first,
                    top[i].second.count, double(top[i].second.ns) / 1000.0,
                    writer == w.fence_writer.end() ? "EOP/CPU " : "compute pipe ",
                    writer == w.fence_writer.end() ? -1 : writer->second);
    }
    std::printf("\n");
    w.by_address.clear();
    w.total_ns = 0;
    w.total = 0;
}

struct Samples {
    std::mutex lock;
    std::vector<u64> link[6]; // submit->decode, decode->run, run->signal, total, submit->start, start->decode (ns)
    u64 window_start = 0;
};
inline Samples& Collected() {
    static Samples samples;
    return samples;
}

inline void Report(Samples& s, u64 now) {
    const double secs = double(now - s.window_start) / 1e9;
    const size_t n = s.link[3].size();
    double mean[6]{}, p95[6]{};
    for (int i = 0; i < 6; ++i) {
        auto& v = s.link[i];
        if (v.empty()) continue;
        u64 sum = 0;
        for (const u64 x : v) sum += x;
        mean[i] = double(sum) / double(v.size()) / 1000.0;
        const size_t k = std::min(v.size() - 1, size_t(double(v.size()) * 0.95));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        p95[i] = double(v[k]) / 1000.0;
        v.clear();
    }
    std::printf("Fence stats: %.0f compute fences/s; submit->decode %.0f/%.0f us, decode->run %.0f/%.0f us, "
                "run->signal %.0f/%.0f us, total %.0f/%.0f us (mean/p95); submit->decode = queued "
                "%.0f/%.0f + in its submit %.0f/%.0f us\n",
                double(n) / secs, mean[0], p95[0], mean[1], p95[1], mean[2], p95[2], mean[3], p95[3],
                mean[4], p95[4], mean[5], p95[5]);
    ReportWaits(secs);
    {
        auto& d = DepthLog();
        std::scoped_lock lk{d.lock};
        std::printf("Fence stats: %.0f compute submits/s, %.1f already waiting in the queue on arrival; "
                    "stage A %.1f us per compute submit, %.0f%% of its time on compute submits\n",
                    double(d.submits) / secs, d.submits ? double(d.depth_sum) / d.submits : 0.0,
                    d.done ? double(d.busy_ns) / 1000.0 / d.done : 0.0,
                    double(d.busy_ns) / 1e7 / secs);
        d.depth_sum = d.submits = d.busy_ns = d.done = 0;
    }
    std::fflush(stdout);
    s.window_start = now;
}

inline void Record(u64 submit, u64 decode, u64 run, u64 start = 0) { // start: stage A began the submit
    if (!decode) return;
    const u64 signal = Now();
    auto& s = Collected();
    std::scoped_lock lk{s.lock};
    if (!s.window_start) s.window_start = signal;
    if (submit && submit <= decode) s.link[0].push_back(decode - submit);
    if (run >= decode) s.link[1].push_back(run - decode);
    if (signal >= run) s.link[2].push_back(signal - run);
    s.link[3].push_back(signal - (submit && submit <= decode ? submit : decode));
    if (submit && start >= submit && start <= decode) {
        s.link[4].push_back(start - submit);
        s.link[5].push_back(decode - start);
    }
    if (signal - s.window_start >= 5'000'000'000ull) Report(s, signal);
}

} // namespace AmdGpu::FenceStats
