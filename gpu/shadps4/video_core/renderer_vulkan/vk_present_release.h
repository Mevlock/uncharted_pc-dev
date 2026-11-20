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
//
// Host-timed holds (BB_EVEN_RELEASE=3, 2026-11-19): the same place in the game's queue, but the GPU
// waits idle instead of spinning (the spin kept the GPU busy: 57% GPU usage against 34% at a 60
// cap on an RTX 5080, under 60 FPS on an RTX 3060 laptop). The present work is queued at once in
// two submissions: the first only takes the frame's ready time; the second (the blit) waits for a
// timeline semaphore that a thread of ours signals at the target (vkSignalSemaphore after a
// high-resolution waitable timer and a short spin), then the present follows it as before. That is
// again a GPU wait that only the host releases: every way the process ends by itself (the menu's
// Quit, the window's close, a fault, a restart) first releases them all (ReleaseAllForExit), and
// releases are never later than the hold's limit. A process killed from outside (Task Manager,
// TerminateProcess) inside a hold can still be left as an unkillable zombie, as 11-18.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

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

    /// How paced frames are held (BB_EVEN_RELEASE; default 4): 1 on the GPU in the game's own queue
    /// (RecordHold; the flips land within ~0.2 ms), 2 on the release queue (a compute queue beside
    /// it; allows slack, but its presents flip ~1 ms loose on NVIDIA, 11-18), 3 in the game's own
    /// queue as 1 but host-timed (the GPU waits idle for a semaphore our thread signals at the
    /// target: RecordHostReady, QueueRelease, RecordHostRelease), 4 in the game's own queue as 1 but
    /// submitted only W before the target (the short spin: ShortSpin, SubmitWait), 0 on the
    /// presenter thread (the CPU hold, as before). Even presents off: 0.
    static int Mode();
    /// The device has what this mode's holds need (1/2: the device clock and calibrated
    /// timestamps; 3: calibrated timestamps).
    static bool Supported(const Instance& instance);
    /// Mode 3 and made: holds are host-timed.
    bool HostTimed() const {
        return host_timed && made;
    }
    /// Mode 4 and made: the short-spin hybrid. The present work is recorded as for mode 1 (the
    /// spin to the target, the blit), but submitted only at the target less W (SubmitWait, on the
    /// presenter thread: a high-resolution timer, then a short CPU spin), so the GPU spins for
    /// about W instead of the whole wait. W follows the GPU's start after our submission.
    bool ShortSpin() const {
        return short_spin && made;
    }
    /// Mode 4, right before the present submission of `slot` (RecordHold), held to `until_ns`:
    /// waits until until_ns - W (100 ms at most), then `frame_done` says whether the frame's own
    /// GPU work was done by then (the submission's start is then ours, a sample for W).
    void SubmitWait(u32 slot, s64 until_ns, const std::function<bool()>& frame_done);
    /// Mode 2 and the device has a present-capable compute queue.
    static bool Wanted(const Instance& instance);

    /// Mode 1, at the start of a paced frame's present command buffer on the game's queue: waits
    /// for all earlier work there, notes the time (the frame's ready time), holds until `until_ns`
    /// (steady clock) but `limit_ns` at most, notes the time again (its release). Later work waits
    /// for the hold too (RecordFence after the blit): the blit runs on an idle GPU at the target.
    /// Returns the slot (Slots: none, BB_HOLD_PART=skip).
    u32 RecordHold(vk::CommandBuffer cmdbuf, s64 flip_arg, s64 until_ns, s64 limit_ns);
    /// Instead of RecordHold for a paced frame whose target has passed when its present work is
    /// recorded (a hold would end at once): only its ready time, no barrier, dispatch or fence, so
    /// a frame that is not early costs nothing.
    void RecordLate(vk::CommandBuffer cmdbuf, s64 flip_arg);
    /// At the end of a RecordHold command buffer: all later work on the queue waits for it.
    static void RecordFence(vk::CommandBuffer cmdbuf);

    /// Mode 3, a paced frame's present work whose target is still ahead, in two submissions on the
    /// game's queue. RecordHostReady: into the first one (which waits for the frame): everything
    /// before it done, then its ready time; returns the slot. QueueRelease: the value of
    /// HostTimeline() that the second submission (the blit) must wait for (0: none, the holds are
    /// released for the exit: submit it without that wait); our thread signals it at `until_ns`
    /// (steady clock) but `limit_ns` from now at most. RecordHostRelease: first in the second
    /// submission's command buffer (its release time, once the wait is over); RecordFence after
    /// the blit as for RecordHold.
    u32 RecordHostReady(vk::CommandBuffer cmdbuf, s64 flip_arg, s64 until_ns);
    u64 QueueRelease(u32 slot, s64 until_ns, s64 limit_ns);
    void RecordHostRelease(vk::CommandBuffer cmdbuf, u32 slot);
    vk::Semaphore HostTimeline() const {
        return host.timeline;
    }
    /// Every hold a host thread must release is released now and none is made after it (the
    /// process ends: TerminateProcess would leave it a zombie behind a GPU wait no one signals).
    /// Any thread, also a fault handler; returns within ~100 ms. Nothing to do without mode 3.
    static void ReleaseAllForExit();

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
    /// The present submission that carries the slots recorded since the last call (RecordHold,
    /// RecordLate, RecordReady + Submit) signals `tick` on `timeline`: Poll reads a slot's queries
    /// only once that is reached (they are reset in that command buffer; before it runs they still
    /// hold the slot's run 16 holds earlier).
    void Submitted(vk::Semaphore timeline, u64 tick);
    /// The holds that have run: `done(flip_arg, ready_ns, release_ns)` for each, in order.
    void Poll(const std::function<void(s64, s64, s64)>& done);

    /// BB_HOLD_PROBE=<csv> (diagnosis): the real time each held present's submission ended, taken
    /// on the CPU (a thread polls `timeline` for `tick`; QueryPerformanceCounter), so without the
    /// device clock or the calibration; joinable to PresentMon by its QPC times.
    static bool Probing();
    /// BB_HOLD_PART=<a,b,...> (diagnosis, the hold's cost by part): skip (no hold commands at all),
    /// nofence (no execution barrier after the blit), nohost (no host-domain barrier after the
    /// dispatch), cal1s (calibrated timestamps once a second, not every frame).
    static bool Part(const char* name);
    void ProbeNote(vk::Semaphore timeline, u64 tick, s64 flip_arg, s64 until_ns);

private:
    struct Slot {
        bool pending = false;
        s64 flip_arg = -1;
        s64 until_ns = 0;
        bool submitted = false; ///< its command buffer is submitted, signalling `tick`
        u64 tick = 0;
        bool fenced = false; ///< also a release-queue submission (its fence)
    };
    static constexpr u32 Slots = 16;

    void Calibrate();
    u32 TakeSlot();
    void HostThread();
    void HostSignal(u64 value);
    void HostReleaseAll();
    void HostStats(u32 index, s64 ready_ns, s64 release_ns);
    void Spin(vk::CommandBuffer cmdbuf, u32 index, s64 until_ns, s64 limit_ns);
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
    vk::Semaphore submitted_timeline{};
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
    // BB_HOLD_PROBE: the calibration's spread per second (deviation, offset drift, pair offset).
    struct CalStats {
        s64 since = 0;
        u32 count = 0;
        u64 deviation_max = 0;
        double offset_min = 0, offset_max = 0;
        double pair_min = 0, pair_max = 0;
        u32 pair_count = 0;
        s64 call_ns_sum = 0, call_ns_max = 0;
    } cal_stats;
    void NoteCalibration(u64 deviation, s64 call_ns);

    // Mode 4: the short-spin hybrid (presenter thread).
    bool short_spin = false;
    void ShortSpinStats(u32 index, s64 ready_ns, s64 entry_ns, s64 exit_ns);
    struct ShortSpinState {
        void* timer = nullptr; ///< Windows: a high-resolution waitable timer
        bool high_resolution = false;
        s64 w_ns = 400'000;           ///< W: the submission this long before the target
        s64 spin_window_ns = 250'000; ///< the CPU spin after the timer
        std::array<s64, 64> wakes{};  ///< the timer's misses
        u32 wake_count = 0;
        std::array<s64, 64> starts{}; ///< GPU start - our intended submission (frames done by then)
        u32 start_count = 0;
        // per slot (SubmitWait -> Poll)
        std::array<s64, Slots> intended{}, submitted{}, cpu_spin{}, oversleep{};
        std::array<bool, Slots> frame_done{};
        std::array<s64, Slots> w_used{};
        // the 5 s stats (us)
        std::vector<float> gpu_err, gpu_spin, sleep_err, cpu, start_late, w;
        u64 late_frames = 0, late_releases = 0, late_starts = 0, unheld = 0;
        s64 window = 0;
    } ss;

    // Mode 3: the host-timed holds (vk_present_release.cpp HostThread).
    bool host_timed = false;
    struct HostJob {
        u64 value;     ///< HostTimeline() value to signal
        s64 at;        ///< when (steady clock): the target less the learnt lead, never after `deadline`
        s64 target;    ///< the hold's end (for the stats)
        s64 deadline;  ///< the hold's limit
        u32 slot;
    };
    struct Host {
        vk::Semaphore timeline{};
        std::thread thread;
        std::mutex mutex;           ///< jobs, wake
        std::condition_variable cv; ///< jobs added (non-Windows)
        std::deque<HostJob> jobs;
        void* wake_event = nullptr; ///< Windows: an auto-reset event (jobs added, exit)
        void* timer = nullptr;      ///< Windows: a high-resolution waitable timer
        bool high_resolution = false;
        std::atomic<u64> next_value{0};   ///< the last value handed out
        std::mutex signal_mutex;          ///< signalled, the signal calls
        u64 signalled = 0;
        std::atomic<bool> closed{false};  ///< released for the exit: no more holds
        std::atomic<bool> drained{false}; ///< the thread has signalled everything for the exit
        std::atomic<bool> quit{false};
        std::atomic<u32> thread_id{0};
        std::atomic<s64> lead_ns{0};      ///< signal this much before the target (GPU wake-up)
        std::atomic<s64> spin_window_ns{250'000};
        // per slot, from the thread (read by Poll once the slot's submission has run)
        std::array<std::atomic<s64>, Slots> signal_at{};
        std::array<std::atomic<s64>, Slots> spin_ns{};
        std::array<std::atomic<s64>, Slots> oversleep_ns{};
        // the thread's own timer misses (oversleep) of the last wakes, for the spin window
        std::array<s64, 64> wakes{};
        u32 wake_count = 0;
        // Poll (presenter): the GPU's wake-up after our signal, for the lead
        std::array<s64, 64> wakeups{};
        u32 wakeup_count = 0;
        // Poll: the 5 s stats (us)
        std::vector<float> host_err, gpu_err, wake_err, sleep_err, spin;
        u64 late_frames = 0, late_releases = 0, missed_limit = 0;
        s64 window = 0;
    } host;
};

} // namespace Vulkan
