// bbport diagnosis: causal profiling. A switch adds a busy wait of N ns to one kind of work; how much the frame rate
// drops shows how much making that work faster would gain (BB_SLOW_ACOMPUTE_NS: stage A, each async compute
// dispatch; BB_SLOW_AGFX_NS: stage A, each graphics draw; BB_SLOW_B_NS: stage B, each packet; BB_SLOW_REC_NS:
// the Vulkan recorder, each recorded command).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>

namespace BbCausal {

/// TSC ticks for the switch's ns (0 when unset).
inline std::uint64_t Ticks(const char* name) {
    const char* env = std::getenv(name);
    const long long ns = env ? std::atoll(env) : 0;
    if (ns <= 0) {
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) {
    }
    const std::uint64_t ticks = (__rdtsc() - c0) * static_cast<std::uint64_t>(ns) / 20'000'000ull;
    std::printf("Causal: %s=%lld ns (%llu TSC ticks)\n", name, ns, static_cast<unsigned long long>(ticks));
    return ticks;
}

/// Items that reached each site (0 compute dispatch, 1 graphics draw, 2 stage B packet, 3 recorded command), with or
/// without a delay;
/// printed (and reset) with BB_FRAME_STATS.
/// perf11: one cache line per site. The four counters shared one line, and BB_FRAME_STATS is on in the launcher's
/// runs: stage A, stage B and the Vulkan recorder (~3M commands/s in U3's crowd) took that line from each other with
/// every draw, packet and command.
struct alignas(64) Hits : std::atomic<std::uint64_t> {};
inline Hits g_hits[4];

/// Only with BB_FRAME_STATS (review 2026-11-01): otherwise three busy threads did a locked add on one shared cache
/// line for every draw, packet and recorded command, for counts nobody printed.
inline void Count(int site) {
    static const bool on = [] {
        const char* env = std::getenv("BB_FRAME_STATS");
        return env && env[0] == '1';
    }();
    if (!on) {
        return;
    }
    g_hits[site].fetch_add(1, std::memory_order_relaxed);
}

/// Each switch calibrated once (the first use), shared by every site that uses it.
inline std::uint64_t SlowACompute() {
    static const std::uint64_t ticks = Ticks("BB_SLOW_ACOMPUTE_NS");
    return ticks;
}
inline std::uint64_t SlowAGfx() {
    static const std::uint64_t ticks = Ticks("BB_SLOW_AGFX_NS");
    return ticks;
}
inline std::uint64_t SlowB() {
    static const std::uint64_t ticks = Ticks("BB_SLOW_B_NS");
    return ticks;
}
inline std::uint64_t SlowRec() {
    static const std::uint64_t ticks = Ticks("BB_SLOW_REC_NS");
    return ticks;
}

inline void Spin(std::uint64_t ticks) {
    if (ticks == 0) {
        return;
    }
    const std::uint64_t end = __rdtsc() + ticks;
    while (__rdtsc() < end) {
        _mm_pause();
    }
}

} // namespace BbCausal
