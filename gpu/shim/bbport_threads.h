// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include <bit>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {
/// Set on the helper threads (draw preparation, preload): they are not the GPU thread.
inline thread_local bool background_thread = false;
inline bool IsBackground() {
    return background_thread;
}
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    background_thread = true;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// bbport: hybrid CPUs (Intel 12th gen and later, Core Ultra: performance and efficiency cores).
/// Windows may move a busy thread onto an efficiency core and leave it there; a player's frame
/// rate (i5-13600K) fell from 60 to 12 after an area change until the game was restarted. The
/// CPU sets of the highest efficiency class (the performance cores), empty where every core is of
/// one class. BB_PCORES=0: none; BB_PCORES=test: the lower half of the CPU sets stand in for
/// performance cores (tests on a CPU without efficiency cores).
inline const std::vector<ULONG>& PerformanceCpuSets() {
    static const std::vector<ULONG> ids = [] {
        std::vector<ULONG> result;
        const char* env = std::getenv("BB_PCORES");
        if (env && env[0] == '0') {
            std::printf("Performance cores: off (BB_PCORES=0)\n");
            return result;
        }
        const bool test = env && std::strcmp(env, "test") == 0;
        ULONG length = 0;
        GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
        std::vector<unsigned char> buffer(length);
        auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
        if (!length || !GetSystemCpuSetInformation(info, length, &length, GetCurrentProcess(), 0)) {
            return result;
        }
        std::vector<std::pair<ULONG, BYTE>> sets; // id, efficiency class
        for (ULONG at = 0; at < length;) {
            const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + at);
            if (entry->Type == CpuSetInformation) {
                sets.emplace_back(entry->CpuSet.Id, entry->CpuSet.EfficiencyClass);
            }
            at += entry->Size;
        }
        BYTE lowest = 0xff, highest = 0;
        for (const auto& [id, cls] : sets) {
            lowest = std::min(lowest, cls);
            highest = std::max(highest, cls);
        }
        for (size_t i = 0; i < sets.size(); ++i) {
            if (test ? i < sets.size() / 2 : (lowest != highest && sets[i].second == highest)) {
                result.push_back(sets[i].first);
            }
        }
        if (result.empty()) {
            std::printf("Performance cores: all %zu cores are of one class, threads left to "
                        "Windows\n", sets.size());
        } else {
            std::printf("Performance cores: %zu of %zu CPU sets%s; the GPU command, draw "
                        "recording, Vulkan recording and present threads stay on them\n",
                        result.size(), sets.size(), test ? " (BB_PCORES=test)" : "");
        }
        return result;
    }();
    return ids;
}

/// The calling thread prefers the performance cores of a hybrid CPU and is never
/// power-throttled (see PerformanceCpuSets). Nothing changes on other CPUs.
inline void PreferPerformanceCores() {
    const auto& ids = PerformanceCpuSets();
    if (ids.empty()) {
        return;
    }
    SetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), static_cast<ULONG>(ids.size()));
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
}

/// bbport (from the Bloodborne PC port, BB_GPU_THREAD_PRIORITY): the GPU command, draw
/// recording and Vulkan recording threads above the game's ~60 threads, so a busy CPU does not
/// preempt the thread that sets the frame rate. BB_GPU_THREAD_PRIORITY=0: normal priority.
/// On hybrid CPUs they also stay on the performance cores (PreferPerformanceCores).
inline void MakeRenderThread() {
    static const bool raise = [] {
        const char* env = std::getenv("BB_GPU_THREAD_PRIORITY");
        return !(env && env[0] == '0');
    }();
    if (raise) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
    PreferPerformanceCores();
}

/// The calling thread yields to the game's own threads but is not starved by them.
inline void MakeBelowNormal() {
    background_thread = true;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

/// Normal priority while in scope (a background helper that holds something the GPU thread may
/// wait for must not be starved by busy game threads).
struct ScopedNormalPriority {
    int previous = GetThreadPriority(GetCurrentThread());
    ScopedNormalPriority() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    }
    ~ScopedNormalPriority() {
        SetThreadPriority(GetCurrentThread(), previous);
    }
};
#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    background_thread = true;
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}

/// Windows only (see above).
inline void MakeRenderThread() {}
inline void PreferPerformanceCores() {}

/// The calling thread yields to the game's own threads but is not starved by them.
inline void MakeBelowNormal() {
    background_thread = true;
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 5);
}

/// Does nothing here: a thread that went to SCHED_IDLE or nice 19 cannot raise itself again
/// without privileges, so a helper holding a claim is only slowed while every core is busy.
/// (The empty constructor keeps -Wunused-variable quiet at the call sites.)
struct ScopedNormalPriority {
    ScopedNormalPriority() {}
    ~ScopedNormalPriority() {}
};
#endif

} // namespace BbThreads
