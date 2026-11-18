// SPDX-License-Identifier: GPL-2.0-or-later
// bbport even presents: the GPU holds a paced frame's present work until its target.
//
// A flip is on time when its present was queued before its last GPU work and that work runs on an
// idle GPU (2026-11-18, PresentMon: a present call made on the CPU at the target after the frame was
// long done flipped 0.2-3 ms late at random; the old CPU gate, which held the GPU thread until the
// present work was in, gave even flips but held the guest's next frame with it: 115 fps at a 144
// cap). Here the presenter queues the frame's blit and present at once, behind a one-invocation
// dispatch that keeps the queue until the device clock reaches the target (VK_KHR_shader_clock's
// device clock, mapped from the CPU's clock with VK_KHR/EXT_calibrated_timestamps), and the next
// frame's GPU work waits behind that blit (an execution barrier at the end of its command buffer).
// The guest records and queues its next frame meanwhile; the GPU is idle from the frame's end to
// its target, and the blit runs then.
// The dispatch always ends by itself (a limit from its start): there is no wait that only the host
// can release, so a process that is closed or killed during a hold ends as usual (a relay design
// that waited for the host on the GPU left an unkillable process, 11-18).
#pragma once

#include <array>
#include <functional>
#include <mutex>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

class Instance;

/// The release queue's submissions and presents (presenter thread; a lock all the same).
inline std::mutex g_release_queue_mutex;

class PresentRelease {
public:
    explicit PresentRelease(const Instance& instance);
    ~PresentRelease();
    PresentRelease(const PresentRelease&) = delete;
    PresentRelease& operator=(const PresentRelease&) = delete;

    /// Made, and the device clock is known against the CPU's (the first holds wait for that).
    bool Ready() const {
        return made;
    }

    /// Even presents are on and this device has what the holds need (a present-capable compute
    /// queue, the device clock, calibrated timestamps). BB_EVEN_RELEASE=0: off (A/B only).
    static bool Wanted(const Instance& instance);

    /// At the end of a held frame's present command buffer on the graphics queue: notes when its
    /// blit is done there (the frame's ready time; the release queue's own start would follow the
    /// last hold, its holds run in order). Returns the slot for Submit.
    s32 RecordReady(vk::CommandBuffer cmdbuf);
    /// On the release queue: waits for `blit_done` (that blit), notes the time, holds until
    /// `until_ns` (steady clock) but `limit_ns` at most, notes the time again (its release) and
    /// signals `released`, which the present waits for. `flip_arg`: the game flip it belongs to
    /// (its times go to Poll; -1: none). False: not submitted (nothing signals `released`).
    bool Submit(s32 slot, vk::Semaphore blit_done, vk::Semaphore released, s64 flip_arg, s64 until_ns,
                s64 limit_ns);
    /// The holds that have run: `done(flip_arg, ready_ns, release_ns)` for each, in order.
    void Poll(const std::function<void(s64, s64, s64)>& done);

private:
    struct Slot {
        bool pending = false;
        s64 flip_arg = -1;
        s64 until_ns = 0;
    };
    static constexpr u32 Slots = 16;

    void Calibrate();
    s64 TicksToNs(u64 ticks) const;
    u64 NsToTicks(s64 ns) const;

    const Instance& instance;
    bool made = false;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueQueryPool pool;
    vk::UniqueCommandPool command_pool; ///< the release queue's family
    std::array<vk::UniqueCommandBuffer, Slots> cmdbufs;
    std::array<vk::UniqueFence, Slots> fences;
    vk::Buffer buffer{};
    VmaAllocation allocation{};
    u64* stamps = nullptr; ///< mapped: each slot's shader clock at its start
    u64 address = 0;
    std::array<Slot, Slots> slots{};
    u32 next = 0;   ///< the slot RecordHold uses next
    u32 oldest = 0; ///< the oldest pending slot
    double tick_ns = 1.0;
    // The timestamps against the steady clock (calibrated timestamps, refreshed every frame).
    u64 cal_ticks = 0;
    s64 cal_ns = 0;
    s64 steady_minus_qpc = 0; ///< steady clock - QueryPerformanceCounter in ns (both read together)
    // The shader's device clock against the timestamps: clock = a * ticks + b (pairs from every
    // hold: the timestamp just before it, the clock it started at).
    bool fitted = false;
    double fit_a = 1.0, fit_b = 0.0;
    u64 first_ticks = 0, first_clock = 0;
    u32 pairs = 0;
};

} // namespace Vulkan
