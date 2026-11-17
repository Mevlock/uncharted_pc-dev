// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// NVIDIA Reflex PC Latency Stats (PCLStats) for the port's low latency mode: the ETW provider of NVIDIA's pclstats.h
// (Reflex SDK, github.com/NVIDIA-RTX/REFLEX, Reflex_Stats/pclstats.h; fetched at build time, not part of this
// repository, under NVIDIA's SDK licence: NVIDIA-REFLEX-LICENSE.txt). PresentMon, FrameView and the NVIDIA overlay
// read PC latency from these events. This software contains source code provided by NVIDIA Corporation (the header's
// macros, compiled in here).
//
// The port passes its Reflex markers (VkLatencyMarkerNV numbers, the same as PCLStats') with the game frame ids; the
// pings of a latency tool go to this file's own message thread (PCLSTATS_SET_ID_THREAD) and are marked on the next
// frame start, where the game reads its input. Without a listening tool TraceLoggingWrite only tests a flag.

#include <atomic>
#include <cstdint>

#include "bbport_dlss_bridge.h"

#include <pclstats.h>

PCLSTATS_DEFINE()

namespace {

std::atomic<bool> ping_pending{false};
std::atomic<int> state{0}; // 0 not yet, 1 ready, -1 failed

DWORD WINAPI PingReceiver(LPVOID ready) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (PCLSTATS_IS_PING_MSG_ID(msg.message)) {
            ping_pending.store(true, std::memory_order_relaxed);
        }
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) int32_t BbPclStatsInit(void) {
    int expected = 0;
    if (!state.compare_exchange_strong(expected, -1)) {
        return expected == 1;
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) {
        return 0;
    }
    DWORD receiver = 0;
    HANDLE thread = CreateThread(nullptr, 0, PingReceiver, ready, 0, &receiver);
    if (!thread || WaitForSingleObject(ready, 1000) != WAIT_OBJECT_0) {
        CloseHandle(ready);
        return 0;
    }
    CloseHandle(ready);
    CloseHandle(thread);
    PCLSTATS_SET_ID_THREAD(receiver);
    PCLSTATS_INIT(0);
    state.store(1);
    return 1;
}

extern "C" __declspec(dllexport) void BbPclStatsMarker(uint32_t marker, uint64_t frame_id) {
    // Marker 6 (INPUT_SAMPLE) is deprecated in PCLStats: the frame start carries the input.
    if (state.load(std::memory_order_relaxed) != 1 || !g_PCLStatsEnable || marker == 6) {
        return;
    }
    if (marker == PCLSTATS_SIMULATION_START && ping_pending.exchange(false, std::memory_order_relaxed)) {
        PCLSTATS_MARKER(PCLSTATS_PC_LATENCY_PING, frame_id);
    }
    PCLSTATS_MARKER(marker, frame_id);
}
