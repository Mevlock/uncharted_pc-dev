// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage B stamps each packet it records with
// the submission tick (Stamp), stage A retires stamps whose tick the GPU has passed.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class ConstantRing {
public:
    static constexpr u64 Capacity = 32ull << 20;

    ConstantRing(const Instance& instance, Scheduler& scheduler_)
        : scheduler{scheduler_},
          buffer{instance, 0, Capacity, VideoCore::MemoryType::Stream, "bbport constant ring"} {
        ASSERT(!buffer.mapped_data.empty());
    }

    [[nodiscard]] vk::Buffer Handle() const noexcept {
        return buffer.Handle();
    }

    /// Stage A: `size` bytes at `alignment`; returns the ring offset, or nullopt when the ring is
    /// still in use by the GPU (the caller leaves the binding to stage B).
    std::optional<u64> Allocate(u64 size, u64 alignment) {
        u64 at = (position + alignment - 1) & ~(alignment - 1);
        if (at % Capacity + size > Capacity) {
            at += Capacity - at % Capacity; // wrap to the ring start
        }
        const auto in_use = [&] { return at + size > Capacity && at + size - Capacity > retired; };
        if (in_use()) {
            Retire();
            if (in_use()) {
                ++failed_allocations;
                return std::nullopt;
            }
        }
        position = at + size;
        return at % Capacity;
    }

    [[nodiscard]] u8* Data(u64 offset) noexcept {
        return buffer.mapped_data.data() + offset;
    }

    /// Stage A, after writing: makes the bytes visible to the GPU.
    void Flush(u64 offset, u64 size) {
        buffer.Flush(offset, size);
    }

    /// Stage A: the position the next packet's allocations end at.
    [[nodiscard]] u64 Position() const noexcept {
        return position;
    }

    /// Stage B, after recording a packet whose allocations end at `end`. Stamps of one
    /// submission tick are merged: only the last end per tick is published.
    void Stamp(u64 end) {
        const u64 tick = scheduler.CurrentTick();
        if (end == pending_end) {
            // bbport: a full ring stops moving (its allocations fail), so the same end comes
            // again and again: it still has to be published once its tick is submitted, or the
            // ring never retires and stays full (every allocation failed after the first lap,
            // ~2 million a second in U3's square). BB_RING_STALL_FIX=0: as before.
            static const bool fix = [] {
                const char* env = std::getenv("BB_RING_STALL_FIX");
                return !(env && env[0] == '0');
            }();
            if (fix && tick != pending_tick && pending_end != last_published) {
                Publish(pending_end, pending_tick);
                pending_tick = tick;
            }
            return;
        }
        if (tick != pending_tick && pending_end != last_published) {
            Publish(pending_end, pending_tick);
        }
        pending_end = end;
        pending_tick = tick;
    }

private:
    void Publish(u64 end, u64 tick) {
        const u64 at = stamps_head.load(std::memory_order_relaxed);
        if (at - stamps_tail.load(std::memory_order_acquire) >= stamps.size()) {
            return; // full: this region retires with a later stamp
        }
        stamps[at % stamps.size()] = {end, tick};
        stamps_head.store(at + 1, std::memory_order_release);
        last_published = end;
    }

    void Retire() {
        auto* semaphore = scheduler.GetWorkSemaphore();
        // bbport: querying the GPU's progress is a driver call (a system call); with the ring
        // waiting on the GPU every allocation came here (~7% of stage A in U3's square). The
        // known tick is tried first; a query at most every 20 us (BB_RING_RETIRE_THROTTLE=0:
        // every time, as before).
        static const bool throttle = [] {
            const char* env = std::getenv("BB_RING_RETIRE_THROTTLE");
            return !(env && env[0] == '0');
        }();
        const u64 tail = stamps_tail.load(std::memory_order_relaxed);
        const bool known_free = tail < stamps_head.load(std::memory_order_acquire) &&
                                semaphore->IsFree(stamps[tail % stamps.size()].tick);
        const u64 now = __rdtsc();
        if (!throttle || (!known_free && now - last_refresh >= 20 * TicksPerUs())) {
            semaphore->Refresh();
            last_refresh = now;
            ++refreshes;
        }
        ++retires;
        PrintStats(now);
        u64 at = stamps_tail.load(std::memory_order_relaxed);
        const u64 head = stamps_head.load(std::memory_order_acquire);
        while (at < head && semaphore->IsFree(stamps[at % stamps.size()].tick)) {
            retired = stamps[at % stamps.size()].end;
            ++at;
        }
        stamps_tail.store(at, std::memory_order_release);
    }

    static u64 TicksPerUs() {
        static const u64 ticks = [] {
            const auto t0 = std::chrono::steady_clock::now();
            const u64 c0 = __rdtsc();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2)) {
            }
            return std::max<u64>(1, (__rdtsc() - c0) / 2000);
        }();
        return ticks;
    }

    /// BB_FRAME_STATS: retire calls, driver queries and failed allocations per second.
    void PrintStats(u64 now) {
        static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
        if (!stats) {
            return;
        }
        if (!stats_since) {
            stats_since = now;
        }
        const u64 window = 5'000'000 * TicksPerUs();
        if (now - stats_since >= window) {
            std::printf("Constant ring (5 s): %.0f retires/s, %.0f GPU queries/s, %.0f failed "
                        "allocations/s\n",
                        retires / 5.0, refreshes / 5.0, failed_allocations / 5.0);
            retires = refreshes = failed_allocations = 0;
            stats_since = now;
        }
    }

    u64 last_refresh = 0;     ///< stage A
    u64 retires = 0, refreshes = 0, failed_allocations = 0, stats_since = 0;

    struct StampEntry {
        u64 end;
        u64 tick;
    };
    Scheduler& scheduler;
    VideoCore::Buffer buffer;
    u64 position = 0;     ///< stage A
    u64 retired = 0;      ///< stage A: the GPU is done with everything before this
    u64 pending_end = 0, pending_tick = 0, last_published = 0; ///< stage B
    std::array<StampEntry, 1024> stamps{};
    alignas(64) std::atomic<u64> stamps_head{0}; ///< stage B writes
    alignas(64) std::atomic<u64> stamps_tail{0}; ///< stage A reads
};

} // namespace Vulkan
