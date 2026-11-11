// bbport diagnosis: BB_FRAME_TRACE=<seconds>: after that many seconds of play, records a
// timestamped event per guest GPU wait, command processor submission and drain, draw recording
// packet and Vulkan recording chunk for 30 guest flips, then writes them to BB_FRAME_TRACE_FILE
// (default frame_trace.csv): microseconds since the first event, event, thread, argument.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <x86intrin.h>

namespace BbTrace {

enum Code : std::uint32_t {
    GuestIdleWaitBegin = 1, ///< arg: site (0 doorbell, 1 submit, 2 submit and flip, 3 submit done)
    GuestIdleWaitEnd = 2,
    GuestSubmitDone = 3,
    IdleReset = 4,          ///< the guest's GPU idle (submission lock released)
    DrainBegin = 6,         ///< arg: reason
    DrainEnd = 7,
    PacketBegin = 8,        ///< stage B; arg: kind
    PacketEnd = 9,
    ChunkBegin = 10,        ///< Vulkan recording thread
    ChunkEnd = 11,
    Flip = 12,
    SubmissionBegin = 13,   ///< command processor; arg: queue
    SubmissionEnd = 14,
    DryBegin = 15,          ///< command processor ran dry: submission end work
    DryEnd = 16,
    VoWaitBegin = 17,       ///< the graphics queue waits for a video out label (flip done)
    VoWaitEnd = 18,
};

struct Event {
    std::uint64_t tsc;
    std::uint32_t code;
    std::uint32_t thread;
    std::uint64_t arg;
};

inline constexpr std::size_t Capacity = std::size_t{1} << 21;
inline Event* g_events = nullptr;
inline std::atomic<std::size_t> g_count{0};
inline std::atomic<int> g_state{0}; ///< 0 waiting, 1 recording, 2 done
inline std::atomic<std::uint32_t> g_next_thread{0};

inline std::uint32_t ThreadIndex() {
    static thread_local const std::uint32_t index = g_next_thread.fetch_add(1);
    return index;
}

inline void Record(std::uint32_t code, std::uint64_t arg = 0) {
    if (g_state.load(std::memory_order_relaxed) != 1) {
        return;
    }
    const std::size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i < Capacity) {
        g_events[i] = {__rdtsc(), code, ThreadIndex(), arg};
    }
}

/// Called once per guest flip (stage B): starts, counts and ends the recording.
inline void OnFlip() {
    static const double start_seconds = [] {
        const char* env = std::getenv("BB_FRAME_TRACE");
        return env ? std::atof(env) : -1.0;
    }();
    if (start_seconds < 0) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static const auto launch = Clock::now();
    static Clock::time_point clock_start;
    static std::uint64_t tsc_start = 0;
    static int flips = 0;
    const int state = g_state.load(std::memory_order_relaxed);
    if (state == 0) {
        if (Clock::now() - launch < std::chrono::duration<double>(start_seconds)) {
            return;
        }
        g_events = new Event[Capacity];
        clock_start = Clock::now();
        tsc_start = __rdtsc();
        g_state.store(1, std::memory_order_release);
        Record(Flip, 0);
        return;
    }
    if (state != 1) {
        return;
    }
    Record(Flip, ++flips);
    if (flips < 30) {
        return;
    }
    g_state.store(2, std::memory_order_release);
    const double us_per_tick =
        std::chrono::duration<double, std::micro>(Clock::now() - clock_start).count() /
        double(__rdtsc() - tsc_start);
    // Writers that loaded state 1 just before may still be storing: give them a moment.
    std::thread([us_per_tick] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const char* env = std::getenv("BB_FRAME_TRACE_FILE");
        FILE* f = std::fopen(env && env[0] ? env : "frame_trace.csv", "w");
        if (!f) {
            return;
        }
        const std::size_t n = std::min(g_count.load(), Capacity);
        const std::uint64_t base = n ? g_events[0].tsc : 0;
        std::fprintf(f, "us,code,thread,arg\n");
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = g_events[i];
            std::fprintf(f, "%.2f,%u,%u,%llu\n", double(std::int64_t(e.tsc - base)) * us_per_tick,
                         e.code, e.thread, static_cast<unsigned long long>(e.arg));
        }
        std::fclose(f);
        std::printf("Frame trace: %zu events written\n", n);
    }).detach();
}

} // namespace BbTrace
