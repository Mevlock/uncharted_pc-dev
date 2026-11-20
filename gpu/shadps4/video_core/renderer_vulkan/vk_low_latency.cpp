// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: low latency mode (vk_low_latency.h).

#include "video_core/renderer_vulkan/vk_low_latency.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/emulator_settings.h"
#include "core/libraries/videoout/driver.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan::LowLatency {
namespace {

enum class Kind { Off, Reflex, AntiLag };

Kind kind = Kind::Off;
bool sleep_on = false; ///< false: markers only (BB_LOW_LATENCY=m)
bool boost = false;
bool present_id = false;

VkPhysicalDevicePresentIdFeaturesKHR present_id_features{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
VkPhysicalDeviceAntiLagFeaturesAMD anti_lag_features{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
const VkSwapchainLatencyCreateInfoNV swapchain_latency{
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV, .latencyModeEnable = VK_TRUE};

VkDevice device = VK_NULL_HANDLE;
PFN_vkSetLatencySleepModeNV set_sleep_mode{};
PFN_vkLatencySleepNV latency_sleep{};
PFN_vkSetLatencyMarkerNV set_marker{};
PFN_vkGetLatencyTimingsNV get_timings{};
PFN_vkAntiLagUpdateAMD anti_lag_update{};
VkSemaphore sleep_semaphore = VK_NULL_HANDLE;
u64 sleep_value = 0; ///< swapchain_mutex

std::mutex swapchain_mutex;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;

/// The runtime found no frame start of the game (SetFrameSource): off from then on.
std::atomic<bool> no_source{false};

/// The last id of each stage (id = the game's frame number + 1). Each only goes up, and a stage
/// takes an id only once the stage before it did: a frame is submitted after it started and
/// presented after it was submitted.
std::atomic<u64> started{0};
std::atomic<u64> submitted{0};
std::atomic<u64> presented{0};

/// BB_LOW_LATENCY: 0 off, 1 on, 2 on with boost, m markers only.
char Setting() {
    const char* env = std::getenv("BB_LOW_LATENCY");
    return env && env[0] ? env[0] : '0';
}

bool On() {
    return kind != Kind::Off && !no_source.load(std::memory_order_relaxed);
}

/// BB_REFLEX_LOG=1: per stage, about once a second, the ids seen (as game frame numbers), gaps
/// (ids skipped), repeats (an id not above the last) and refused ids (not through the stage
/// before), with the first few of those, and how far each stage runs behind the one before.
const bool log_on = [] {
    const char* env = std::getenv("BB_REFLEX_LOG");
    return env && env[0] == '1';
}();

struct StageLog {
    const char* name;
    std::atomic<u64> count{0}, first{0}, last{0}, gaps{0}, repeats{0}, refused{0};
    std::atomic<s64> lead_min{INT64_MAX}, lead_max{INT64_MIN};
    static constexpr u32 Samples = 6;
    std::atomic<u32> sampled{0};
    std::atomic<u64> sample_from[Samples]{}, sample_to[Samples]{};
    std::atomic<char> sample_kind[Samples]{};

    void Sample(char what, u64 from, u64 to) {
        const u32 i = sampled.fetch_add(1, std::memory_order_relaxed);
        if (i < Samples) {
            sample_kind[i].store(what, std::memory_order_relaxed);
            sample_from[i].store(from, std::memory_order_relaxed);
            sample_to[i].store(to, std::memory_order_relaxed);
        }
    }
    /// `id` taken after `prev` (0: none yet); `lead`: how far the stage before is ahead.
    void Taken(u64 prev, u64 id, s64 lead) {
        if (count.fetch_add(1, std::memory_order_relaxed) == 0) {
            first.store(id, std::memory_order_relaxed);
        }
        last.store(id, std::memory_order_relaxed);
        if (prev && id != prev + 1) {
            gaps.fetch_add(id - prev - 1, std::memory_order_relaxed);
            Sample('g', prev, id);
        }
        s64 v = lead_min.load(std::memory_order_relaxed);
        while (lead < v && !lead_min.compare_exchange_weak(v, lead, std::memory_order_relaxed)) {
        }
        v = lead_max.load(std::memory_order_relaxed);
        while (lead > v && !lead_max.compare_exchange_weak(v, lead, std::memory_order_relaxed)) {
        }
    }
    void Repeat(u64 last_id, u64 id) {
        repeats.fetch_add(1, std::memory_order_relaxed);
        Sample('r', last_id, id);
    }
    void Refused(u64 before, u64 id) {
        refused.fetch_add(1, std::memory_order_relaxed);
        Sample('x', before, id);
    }
    /// Appends this window's line part and starts the next window.
    void Print(char* out, size_t size) {
        const u64 n = count.exchange(0, std::memory_order_relaxed);
        const s64 lo = lead_min.exchange(INT64_MAX, std::memory_order_relaxed);
        const s64 hi = lead_max.exchange(INT64_MIN, std::memory_order_relaxed);
        size_t at = std::strlen(out);
        at += std::snprintf(out + at, size - at, " | %s ", name);
        if (n) {
            at += std::snprintf(out + at, size - at, "%llu-%llu (%llu)",
                                static_cast<unsigned long long>(first.load() - 1),
                                static_cast<unsigned long long>(last.load() - 1),
                                static_cast<unsigned long long>(n));
        } else {
            at += std::snprintf(out + at, size - at, "none");
        }
        at += std::snprintf(out + at, size - at, " gaps %llu repeats %llu refused %llu",
                            static_cast<unsigned long long>(gaps.exchange(0)),
                            static_cast<unsigned long long>(repeats.exchange(0)),
                            static_cast<unsigned long long>(refused.exchange(0)));
        if (n && lo <= hi) {
            at += std::snprintf(out + at, size - at, " behind %lld-%lld", static_cast<long long>(lo),
                                static_cast<long long>(hi));
        }
        const u32 k = std::min(sampled.exchange(0, std::memory_order_relaxed), Samples);
        for (u32 i = 0; i < k && at < size; ++i) {
            // g: gap prev -> id; r: repeat (last, id); x: refused (the stage before at, id)
            at += std::snprintf(out + at, size - at, "%s%c %lld>%lld", i ? ", " : " [",
                                sample_kind[i].load(), static_cast<long long>(sample_from[i].load()) - 1,
                                static_cast<long long>(sample_to[i].load()) - 1);
        }
        if (k && at < size) {
            std::snprintf(out + at, size - at, "]");
        }
    }
};
StageLog start_log{"start"}, submit_log{"submit"}, present_log{"present"};
std::atomic<u64> pad_reads{0};
std::atomic<s64> kick_lag_min{INT64_MAX}, kick_lag_max{INT64_MIN};

/// From the game's frame start (it runs every frame, also in loads where nothing is presented).
void PrintLog() {
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(1)) {
        return;
    }
    window = now;
    char line[1024] = "Reflex ids (game frame numbers):";
    start_log.Print(line, sizeof(line));
    const s64 lo = kick_lag_min.exchange(INT64_MAX), hi = kick_lag_max.exchange(INT64_MIN);
    size_t at = std::strlen(line);
    if (lo <= hi) {
        at += std::snprintf(line + at, sizeof(line) - at, " kick behind %lld-%lld",
                            static_cast<long long>(lo), static_cast<long long>(hi));
    }
    submit_log.Print(line, sizeof(line));
    present_log.Print(line, sizeof(line));
    at = std::strlen(line);
    std::snprintf(line + at, sizeof(line) - at, " | pad reads %llu",
                  static_cast<unsigned long long>(pad_reads.exchange(0)));
    std::printf("%s\n", line);
    std::fflush(stdout);
}

/// Latency pacing (with the sleep on; BB_LATENCY_PACING=0: off). A frame rate cap used to be kept
/// by holding finished frames on the present thread until a vblank tick (Frame rate) or slot
/// (Frame rate limit): under a cap the game ran ahead, its frames queued there after their input
/// was read, and the game's own wait for them stretched its frames (U3 at 144: frame start to
/// present 30 ms, PC latency 34-43 ms; at 60: 76 / 85-89 ms). Here the cap is kept at the frame's
/// start instead, before its input: a frame starts no earlier than one cap period after the last
/// frame started, and the present thread shows each game frame as soon as it is done (a front
/// edge limiter, as RTSS's front edge sync or the driver limiters of Reflex / Anti-Lag). Nothing
/// is held for a tick, so a frame cannot miss one, and there is nothing to predict or tune. The
/// game still gets its vblanks at the Frame rate.
namespace Pace {
const bool enabled = [] {
    const char* env = std::getenv("BB_LATENCY_PACING");
    return !(env && env[0] == '0');
}();
/// On once the frame start is hooked and the sleep is on (SetFrameSource).
std::atomic<bool> active{false};
/// The last game frame whose flip reached the present thread (GPU thread).
std::atomic<u64> queued_last{0};
s64 last_wake = 0; ///< frame thread: when the last paced frame was let start
s64 last_target = 0; ///< frame thread: the last frame's wake time (BB_LATENCY_TRACE)
std::atomic<u64> paced{0}, unpaced{0}; ///< counts for BB_LATENCY_TRACE
std::atomic<s64> slept_ns{0};

/// Even presents (BB_EVEN_PRESENT=0: off). On by default in this build (0.3.6.63-test-pacing1) with
/// the short-spin holds (BB_EVEN_RELEASE=4, vk_present_release.h): the present work is submitted just
/// before its target, so the GPU spins for well under a millisecond. 0.3.6.60.1 had turned them off:
/// the GPU hold of 0.3.6.59 (BB_EVEN_RELEASE=1) spun a shader on the GPU until each target, which
/// kept GPU usage near 100% even at a 60 FPS cap and cost FPS on GPU-limited cards (RTX 3060 laptop,
/// 1440p DLSS Quality: under 60, sometimes under 50). Shown as soon as it is done, a frame's present interval
/// was the period plus the change in its render time (the frame start's slot plus its own time):
/// under G-SYNC each refresh lasts that interval (QD-OLED: brightness flicker, U3 at 120 on a
/// 165 Hz panel), and the paced frame clock stepped the world by the start intervals while the
/// frames were shown by their end times (Nate on the horse stepped against the display). Each
/// paced frame now gets a present target = its slot + an offset L, and its world step is the
/// interval between successive targets (the frame clock reads the target's TSC, probe.c): slots
/// are one period apart, so targets are too, presents are held to them, and world steps equal
/// present intervals. The flip itself is released at the target by the GPU (Swapchain::Present:
/// the present is queued at once on a queue of its own, behind a semaphore relay that the
/// presenter lets go at the target): a present call made late on the CPU was flipped 0.2-3 ms
/// after it at random, a flip that follows a GPU signal is on time. L = about the 99.6th
/// percentile of (GPU done - slot) over the last ~4 s plus 0.5 ms: smoothness before latency.
/// L changes are world steps too, so it moves by at most 0.1 ms per frame up and 10 us per frame
/// down (only when 1 ms too high); it is learnt again whenever pacing stops (loads, the boot). A
/// frame later than its target is shown when it is done.
const bool even = [] {
    const char* env = std::getenv("BB_EVEN_PRESENT");
    return !(env && env[0] == '0');
}();
constexpr u32 Ring = 256;
struct FrameSlot {
    std::atomic<u64> id{0};
    std::atomic<s64> slot{0}, target{0};
};
std::array<FrameSlot, Ring> frames{};
/// presenter (swap thread) -> frame thread: GPU done - slot of the last frames
constexpr u32 Samples = 512;
std::array<std::atomic<s64>, Samples> lags{};
std::atomic<u64> lag_count{0};
u64 lag_base = 0; ///< frame thread: the samples before it are from before the last Stop
s64 offset = 0;      ///< frame thread: L
s64 offset_goal = 0; ///< frame thread: L from the samples
std::atomic<s64> current_target{0}; ///< frame thread -> probe.c: this frame's present target (0: none)
std::atomic<s64> offset_ns{0};      ///< for the log
std::atomic<u64> held{0}, late{0};
std::atomic<s64> held_ns{0}, late_ns{0};

/// GPU holds on the release queue (vk_present_release.h; set by the presenter when it has them):
/// the present work is queued at once, held to its target beside the game's GPU work, and the
/// game's GPU queue never waits for a hold.
std::atomic<bool> gpu_release{false};
/// The holds are on the release queue (else on the game's queue: they delay its next frame's GPU
/// work, so the gate keeps that work behind the present work, and no slack is possible there).
std::atomic<bool> release_queue{false};
/// On the game's queue: an extra margin over the frames' own time (BB_EVEN_MARGIN_MS, default 0;
/// the game needs its GPU frame three back done when it starts a frame).
const s64 margin_ns = [] {
    const char* env = std::getenv("BB_EVEN_MARGIN_MS");
    return env && env[0] ? s64(std::atof(env) * 1e6) : s64{0};
}();
/// With them, the offset carries a fixed slack over the frames' usual time (the 95th percentile of
/// GPU done - slot): a frame up to that much later than usual is still shown on its target, so the
/// game's own spikes (an area change on the U3 ride at 120: the game's submit 14.6 ms after its
/// frame start instead of 5.5, the frame done 12.8 ms after its target with 2.5 ms of slack) do
/// not show. Latency instead: about the slack more. BB_EVEN_SLACK_MS (default 16).
const s64 slack_ns = [] {
    const char* env = std::getenv("BB_EVEN_SLACK_MS");
    return env && env[0] ? s64(std::atof(env) * 1e6) : s64{16'000'000};
}();

/// GPU-bound (BB_EVEN_GPU_BOUND=0: off): when the GPU's own time per frame does not fit in a period,
/// no frame can be shown on its target and every hold only delays the next frame's GPU work (AMD,
/// U3 ride at 120: 8100 of ~9200 frames late, the ~1000 held ones cost 3.5% fps against rc059).
/// Then paced frames are not held and not gated (shown when done, as without even presents) until
/// the GPU fits again. The GPU's own time of frame N: its ready time minus the later of the last
/// frame's release and the moment its own last work was queued (its flip): GPU-bound, the work is
/// all queued before the GPU is free, so this is its GPU time; CPU-bound, only the tail after the
/// last submission. On at a median over ~4 s of 0.9 x the period, off again under 0.8 x.
/// That missed a GPU that only just keeps up (2026-11-19, a player's RTX 3060 laptop at 60: under
/// 60 FPS with the holds, fine without): when the game queues its frame's work over most of its
/// CPU frame and the GPU follows it, the last work is queued after the last frame's release and
/// the "own time" is only the short tail after it, however busy the GPU is; and a frame that idled
/// to its target leaves its spare time unused, so a slow frame after a fast one misses its target.
/// So it is also on when the holds themselves make frames late: over ~4 s, 5% or more of the
/// frames (BB_EVEN_GPU_BOUND_LATE, percent) late although they would have been on time without the
/// delay the last frame's hold put on their GPU work (ready - delay <= target), and only frames
/// whose last work was queued before that hold ended: their GPU time then all came after the hold,
/// so the GPU, not the CPU, made them miss. A frame whose work came later (the CPU short of the cap:
/// U2 at 144 on a 9800X3D, 2026-11-20, 26 of 512 such frames turned the holds off at a GPU own time
/// of 2.6 of 6.9 ms) never counts. GPU-bound that way, the targets lie more than a period plus the
/// CPU's frame after the slot (the offset learns the GPU's lag), so the next frame's work is all
/// queued while the hold runs: the laptop case is seen. Off again only after a stay of 10 s,
/// doubled (to 40 s at most) whenever it comes back within 60 s of going off: a check that cannot
/// see the cost while nothing is held must not hold and drop by turns.
const bool gpu_bound_check = [] {
    const char* env = std::getenv("BB_EVEN_GPU_BOUND");
    return !(env && env[0] == '0');
}();
const u32 gpu_bound_late_pct = [] {
    const char* env = std::getenv("BB_EVEN_GPU_BOUND_LATE");
    return env && env[0] ? u32(std::clamp(std::atoi(env), 1, 100)) : 5u;
}();
std::atomic<bool> gpu_bound{false};
std::array<s64, 512> gpu_own{};
std::array<u8, 512> hold_cost{}; ///< 1: late only because of the last frame's hold (presenter)
u64 gpu_own_count = 0; ///< presenter thread
s64 bound_since = 0, bound_left = 0; ///< presenter: when it went on / off last
s64 bound_stay = 10'000'000'000;     ///< presenter: the stay before it may go off
bool bound_local = false;            ///< presenter: on by its own check (Stop clears gpu_bound)
std::atomic<u32> hold_cost_permille{0}; ///< for the log
/// Stop (pacing ends: loads, menus) starts the GPU-bound check afresh: frame thread -> presenter.
std::atomic<u64> gpu_own_epoch{0};
u64 gpu_own_epoch_seen = 0; ///< presenter thread
std::atomic<u64> gpu_bound_changes{0};
std::atomic<s64> gpu_own_median{0}; ///< for the log

void UpdateOffset(s64 period, u64 id) {
    const bool slack = release_queue.load(std::memory_order_relaxed);
    const s64 extra = slack ? slack_ns : gpu_release.load(std::memory_order_relaxed) ? margin_ns : 0;
    if ((id & 15) == 0) {
        const u64 count = lag_count.load(std::memory_order_acquire);
        // the last 4 s at most (512 samples were 8.5 s at a 60 cap: the menu's frames kept the
        // offset ~7 ms high for ~10 s into gameplay, t60g)
        const u64 window = std::max<s64>(64, 4'000'000'000 / std::max<s64>(period, 1'000'000));
        const u32 n = static_cast<u32>(std::min<u64>({count - lag_base, Samples - 32, window}));
        if (n >= 32) {
            std::array<s64, Samples> copy;
            for (u32 i = 0; i < n; ++i) {
                copy[i] = lags[(count - 1 - i) % Samples].load(std::memory_order_relaxed);
            }
            // the third largest (~p99.6); with the slack the 95th percentile: the spikes are what
            // the slack is for, the offset must not chase them
            const u32 k = slack ? n * 95 / 100 : n - 1 - std::min<u32>(n / 200, 2);
            std::nth_element(copy.begin(), copy.begin() + k, copy.begin() + n);
            constexpr s64 margin = 500'000;
            offset_goal = std::clamp<s64>(copy[k] + margin + extra, 0, 2 * period + extra);
        }
    }
    if (offset == 0) {
        // the first: nothing shown at a target yet. With the slack, from the frames' own time,
        // the slack added at the rate below (a jump to the full depth at once left U3's pipeline
        // deep: its submit waits for its GPU frame two back, s3)
        offset = extra ? std::max<s64>(offset_goal - extra, 1) : offset_goal;
    } else if (offset_goal > offset) {
        offset += std::min<s64>(offset_goal - offset, 100'000);
    } else if (offset_goal + 1'000'000 < offset) {
        // down by 1/32 of the excess a frame, 10-100 us (the world steps change by that: under
        // 1% at 60, unseen; 10 us a frame took ~12 s to come down 7 ms at 60)
        offset -= std::clamp<s64>((offset - offset_goal) / 32, 10'000, 100'000);
    }
    offset_ns.store(offset, std::memory_order_relaxed);
}

s64 Now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

void Queued(u64 id) {
    queued_last.store(id, std::memory_order_relaxed);
}

void Stop() {
    last_wake = 0;
    gpu_bound.store(false, std::memory_order_relaxed);
    gpu_own_epoch.fetch_add(1, std::memory_order_release);
    current_target.store(0, std::memory_order_relaxed);
    offset = offset_goal = 0;
    lag_base = lag_count.load(std::memory_order_acquire);
    Libraries::VideoOut::SetStartPaced(false);
}

/// Sleeps to within 1 ms (Windows' sleep granularity), then yields to the time.
void SleepUntil(s64 wake) {
    const auto at = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(wake));
    const auto coarse = at - std::chrono::milliseconds(1);
    if (std::chrono::steady_clock::now() < coarse) {
        std::this_thread::sleep_until(coarse);
    }
    while (std::chrono::steady_clock::now() < at) {
        std::this_thread::yield();
    }
}

/// The frame `id` starts (frame thread, before the driver's sleep and the frame's input).
/// The game frames' world steps (FrameStepNs): each frame's start, as the game's frame clock sees
/// it (paced: its target, the world steps being the intervals between the targets; else when it
/// started). Frame thread -> GPU thread.
std::array<std::atomic<u64>, Ring> step_id{};
std::array<std::atomic<s64>, Ring> step_at{};
void NoteStep(u64 id, s64 at) {
    step_id[id % Ring].store(0, std::memory_order_relaxed);
    step_at[id % Ring].store(at, std::memory_order_relaxed);
    step_id[id % Ring].store(id, std::memory_order_release);
}
s64 StepAt(u64 id) {
    return step_id[id % Ring].load(std::memory_order_acquire) == id
               ? step_at[id % Ring].load(std::memory_order_relaxed)
               : 0;
}
/// The GPU thread's last game flip (its flip argument; -1: none yet).
std::atomic<s64> gpu_flip_arg{-1};

void BeforeFrame(u64 id) {
    if (!active.load(std::memory_order_relaxed)) {
        NoteStep(id, Now());
        return;
    }
    const s64 period = Libraries::VideoOut::FrameCapPeriodNs();
    const s64 now = Now();
    // No cap now (a movie, the boot's fast vblank, the game frozen), or frames that do not reach
    // the present thread (a load: the cap would only slow it): not paced, and the present
    // thread's slot hold keeps the cap meanwhile.
    last_target = 0;
    if (period <= 0 || queued_last.load(std::memory_order_relaxed) + 8 < id) {
        Stop();
        unpaced.fetch_add(1, std::memory_order_relaxed);
        NoteStep(id, now);
        return;
    }
    // The frame's slot: one period after the last one's, at most one period in the past (as the
    // old slot limiter on the present thread). A frame that comes late starts at once, and the
    // next one may then start sooner than a period after it: the cap holds the average rate. With
    // each frame held a full period after the last one's start instead, a game whose frames
    // straddle the period (a slow one, a fast one) lost the fast one's spare time: slow CPU at
    // 144, -4.5% fps against the old hold (fe2, low latency off).
    const s64 slot = last_wake ? std::max(last_wake + period, now - period) : now;
    const s64 wake = std::max(now, slot);
    last_wake = slot;
    last_target = wake;
    if (even) {
        UpdateOffset(period, id);
        FrameSlot& entry = frames[id % Ring];
        entry.id.store(0, std::memory_order_relaxed);
        entry.slot.store(slot, std::memory_order_relaxed);
        entry.target.store(slot + offset, std::memory_order_relaxed);
        entry.id.store(id, std::memory_order_release);
        current_target.store(slot + offset, std::memory_order_relaxed);
        last_target = slot + offset; // the trace's target: the present target
    }
    NoteStep(id, even ? slot + offset : wake);
    Libraries::VideoOut::SetStartPaced(true);
    paced.fetch_add(1, std::memory_order_relaxed);
    if (wake > now) {
        slept_ns.fetch_add(wake - now, std::memory_order_relaxed);
        SleepUntil(wake);
    }
}
/// Game frame `id`'s present target (0: none).
s64 Target(u64 id) {
    const FrameSlot& entry = frames[id % Ring];
    return entry.id.load(std::memory_order_acquire) == id
               ? entry.target.load(std::memory_order_relaxed)
               : 0;
}

std::atomic<bool> timed{false}; ///< the presents carry their target (VK_EXT_present_timing)

/// BB_EVEN_HOLD_CAP=1 (experiment, OFF by default): GPU holds on the game's queue (mode 1) end at
/// the target, but never after the next frame's slot. Aim: a later end blocks the next frame's GPU
/// work and every GPU wait the game makes inside that frame (AMD 7800 XT, U3 ride at 60: frames
/// 17-20 ms from slot to GPU done, offset 21-33 ms, 1-3 game frames of 25-37 ms a run). Off by
/// default because a frame latency above one period is normal elsewhere: NVIDIA, U3 ride at 120,
/// offset 12.6 ms against 8.33, the cap ended ~93% of the holds before their frames were done, they
/// went unheld and the display spread rose 0.24-0.30 -> 0.79-0.85 ms (11-18, g120_F3a/b vs c0a).
const bool hold_cap = [] {
    const char* env = std::getenv("BB_EVEN_HOLD_CAP");
    return env && env[0] == '1';
}();
/// The hold's end for game frame `id` (0: none).
s64 HoldEnd(u64 id) {
    const FrameSlot& entry = frames[id % Ring];
    if (entry.id.load(std::memory_order_acquire) != id) {
        return 0;
    }
    const s64 target = entry.target.load(std::memory_order_relaxed);
    if (!hold_cap || release_queue.load(std::memory_order_relaxed)) {
        return target;
    }
    const s64 period = Libraries::VideoOut::FrameCapPeriodNs();
    return period > 0 ? std::min(target, entry.slot.load(std::memory_order_relaxed) + period) : target;
}


/// The GPU gate (BB_EVEN_GATE=0: off): a paced frame's present work (upscaling / post-processing,
/// the hold, the blit) must be queued before the next frame's GPU work, so that it runs on an idle
/// GPU at its target. The GPU thread only notes each flipped frame that can still be early (its
/// target not passed: a later one has no hold, so there is nothing to keep in order); the draw
/// scheduler's first submission after it then waits, on the thread that submits it, until the
/// presenter has submitted that frame's present work. The GPU thread itself goes on recording:
/// waiting there for the presenter (~0.15 ms a frame) cost 2-4% fps wherever the GPU thread sets
/// the frame rate (U3 market uncapped, 11-18: 165.4 with that wait, 168.1 without it). A CPU wait
/// with a deadline: never a GPU wait that only the host releases.
const bool gate = [] {
    const char* env = std::getenv("BB_EVEN_GATE");
    return !(env && env[0] == '0');
}();
std::mutex gate_mutex;
std::condition_variable gate_cv;
s64 gate_posted = -1; ///< the last game flip whose post work was submitted
std::atomic<s64> gate_posted_a{-1}, gate_noted_a{-1}; ///< lock-free copies: posted, last noted flip
/// The noted frames (gate_mutex): flip, the draw scheduler value of its last work, its deadline.
struct GateEntry {
    u64 id = 0;
    s64 flip = -1;
    u64 tick = 0;
    s64 deadline = 0;
};
std::array<GateEntry, 16> gate_ring{};
u32 gate_next = 0;
std::atomic<u64> gate_waits{0}, gate_timeouts{0}, gate_yields{0};
std::atomic<s64> gate_wait_ns{0};
std::atomic<u32> gate_yield{0}; ///< threads holding the submit lock that wait for the recorder
/// The GPU idle time the gate put before each game frame's GPU work (by frame id): from the last
/// frame's GPU done (or the gate's start, if later) to its post work's submit, i.e. its hold. The
/// presenter takes it off that frame's ready time, so the offset learns the GPU's own time for the
/// frame: with the gate's delay in, each frame's lag carried the last frame's target and the offset
/// fed itself up to its clamp (16.7 ms at 120, m1_gate).
std::array<std::atomic<u64>, Ring> ready_id{}, gate_before_id{};
std::array<std::atomic<s64>, Ring> ready_at{}, gate_before_ns{};

/// GPU holds (vk_present_release.h; set by the presenter when it has them): the presenter queues
/// the present work at once and the GPU holds it to the target; the gate only keeps the next
/// frame's GPU work behind it (the presenter's submit follows the flip at once).
/// The GPU thread's flip of each game frame (by frame id): its last GPU work is queued then.
std::array<std::atomic<u64>, Ring> flip_id{};
std::array<std::atomic<s64>, Ring> flip_at{};
std::atomic<u64> release_n{0}, release_late{0};
std::atomic<s64> release_err_ns{0}, release_err_max{0}, release_hold_ns{0};
/// A held frame's hold also delays the next frame's GPU work (it waits behind the held blit, on the
/// game's queue). The offset's samples must not carry that delay: with it in, frame N's sample
/// (ready - slot) was at least the offset minus a period plus its GPU time, so the offset fed
/// itself and stayed deep (AMD, U3 ride at 60: 21-33 ms for the whole ride against ~10 ms of the
/// frames' own time, after one late burst at the ride start; the game's own GPU waits inside a
/// frame then waited for the last frame's hold: 25-28 ms game frames that rc059 never had; GPU-bound
/// at 144 it ran to its clamp). So each frame's first draw submission is noted (the time its GPU
/// work could start without the hold), with the last frame's ready and release times.
std::atomic<u64> first_pending_id{0}, first_pending_tick{0};
std::array<std::atomic<u64>, Ring> first_submit_id{};
std::array<std::atomic<s64>, Ring> first_submit_at{};
std::array<u64, Ring> times_id{};      ///< presenter: GpuTimes' frames
std::array<s64, Ring> times_ready{}, times_release{};
std::atomic<s64> own_delay_ns{0};      ///< for the log: the delay taken off the samples

void GateNote(u64 id, s64 period, u64 draw_tick) {
    const s64 now = Now();
    // the next frame's first draw submission (the first one signalling more than this frame's work)
    first_pending_tick.store(draw_tick, std::memory_order_relaxed);
    first_pending_id.store(id + 1, std::memory_order_release);
    flip_at[id % Ring].store(now, std::memory_order_relaxed);
    flip_id[id % Ring].store(id, std::memory_order_release);
    const s64 target = Target(id);
    if (!gate || !target || timed.load(std::memory_order_relaxed) ||
        release_queue.load(std::memory_order_relaxed)) {
        return; // with the release queue nothing on the GPU queue waits for a hold
    }
    if (now >= HoldEnd(id) || gpu_bound.load(std::memory_order_relaxed)) {
        return; // done after its hold's end (its GPU work is still being queued), or not held
    }
    const s64 flip = s64(id) - 1;
    std::scoped_lock lk{gate_mutex};
    // deadline: one period after the target (a frame that never comes to the presenter)
    gate_ring[gate_next] = {.id = id,
                            .flip = flip,
                            .tick = draw_tick,
                            .deadline = target + std::max<s64>(period, 1'000'000)};
    gate_next = (gate_next + 1) % gate_ring.size();
    gate_noted_a.store(flip, std::memory_order_release);
}

/// The newest noted frame a submission signalling `tick` must follow whose post work is not
/// submitted yet (gate_mutex held); null: none.
const GateEntry* GateFor(u64 tick) {
    const GateEntry* found = nullptr;
    for (const GateEntry& e : gate_ring) {
        if (e.flip > gate_posted && e.tick < tick && (!found || e.flip > found->flip)) {
            found = &e;
        }
    }
    return found;
}

bool GateNeeded(u64 tick) {
    if (gate_noted_a.load(std::memory_order_acquire) <= gate_posted_a.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lk{gate_mutex};
    return GateFor(tick) != nullptr;
}

/// Short-spin holds (mode 4): the present work is submitted only just before its target, so the
/// gate holds the next frame's first draw submission much longer than with mode 1; its "could
/// start" time (own delay, GpuTimes) is then when it came to the gate, not when the gate let it go
/// (else the gate's wait stayed in the offset's samples and fed the offset, as m1_gate).
std::atomic<bool> short_spin{false};

void NoteFirstSubmit(u64 tick, s64 at = 0) {
    u64 id = first_pending_id.load(std::memory_order_acquire);
    if (id != 0 && tick > first_pending_tick.load(std::memory_order_relaxed) &&
        first_pending_id.compare_exchange_strong(id, 0, std::memory_order_acq_rel)) {
        first_submit_at[id % Ring].store(at ? at : Now(), std::memory_order_relaxed);
        first_submit_id[id % Ring].store(id, std::memory_order_release);
    }
}

void GateBeforeSubmit(u64 tick, bool recorder) {
    if (gate_noted_a.load(std::memory_order_acquire) <= gate_posted_a.load(std::memory_order_acquire)) {
        NoteFirstSubmit(tick);
        return;
    }
    std::unique_lock lk{gate_mutex};
    const GateEntry* entry = GateFor(tick);
    if (!entry) {
        lk.unlock();
        NoteFirstSubmit(tick);
        return;
    }
    const GateEntry e = *entry;
    const s64 start = Now();
    if (recorder) {
        g_async_gated_tick.store(tick, std::memory_order_release); // WaitAsyncSubmits passes it
    }
    const auto deadline =
        std::chrono::steady_clock::time_point(std::chrono::nanoseconds(std::max(e.deadline, start)));
    const auto open = [&] {
        return gate_posted >= e.flip || (recorder && gate_yield.load(std::memory_order_acquire) > 0);
    };
    if (!gate_cv.wait_until(lk, deadline, open)) {
        gate_timeouts.fetch_add(1, std::memory_order_relaxed);
        gate_posted = std::max(gate_posted, e.flip); // given up: the later submissions go on too
        gate_posted_a.store(gate_posted, std::memory_order_release);
    } else if (gate_posted < e.flip) {
        gate_yields.fetch_add(1, std::memory_order_relaxed); // gave way (GateYieldBegin)
    }
    if (recorder) {
        g_async_gated_tick.store(0, std::memory_order_release);
    }
    lk.unlock();
    // submitted now: after the wait (short spin: when it came, see short_spin)
    NoteFirstSubmit(tick, short_spin.load(std::memory_order_relaxed) ? start : 0);
    const s64 end = Now();
    gate_waits.fetch_add(1, std::memory_order_relaxed);
    gate_wait_ns.fetch_add(end - start, std::memory_order_relaxed);
    const u64 id = e.id;
    const s64 idle_from = ready_id[id % Ring].load(std::memory_order_acquire) == id
                              ? std::max(start, ready_at[id % Ring].load(std::memory_order_relaxed))
                              : start;
    gate_before_ns[(id + 1) % Ring].store(std::max<s64>(0, end - idle_from),
                                          std::memory_order_relaxed);
    gate_before_id[(id + 1) % Ring].store(id + 1, std::memory_order_release); // the next frame's work follows
}

void GateYieldBegin() {
    gate_yield.fetch_add(1, std::memory_order_acq_rel);
    { std::scoped_lock lk{gate_mutex}; } // a gate wait checks the count under this lock
    gate_cv.notify_all();
}

void GateYieldEnd() {
    gate_yield.fetch_sub(1, std::memory_order_acq_rel);
}

void GatePost(s64 flip) {
    {
        std::scoped_lock lk{gate_mutex};
        gate_posted = std::max(gate_posted, flip);
        gate_posted_a.store(gate_posted, std::memory_order_release);
    }
    gate_cv.notify_all();
}

void Sample(u64 id, s64 ready) {
    const FrameSlot& entry = frames[id % Ring];
    if (entry.id.load(std::memory_order_acquire) != id) {
        return;
    }
    const u64 n = lag_count.load(std::memory_order_relaxed);
    lags[n % Samples].store(ready - entry.slot.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    lag_count.store(n + 1, std::memory_order_release);
}

/// A GPU hold has run (presenter, a frame or two later): game frame `id`'s blit was done on the
/// GPU at `ready`, its present released at `release`. The offset's sample is ready - slot less the
/// delay the last frame's hold put on this frame's GPU work (on the game's queue the next frame's
/// work waits behind a hold; on the release queue nothing does).
void GpuTimes(u64 id, s64 ready, s64 release) {
    const FrameSlot& entry = frames[id % Ring];
    if (entry.id.load(std::memory_order_acquire) != id) {
        return;
    }
    const s64 slot = entry.slot.load(std::memory_order_relaxed);
    const s64 target = HoldEnd(id); // where its hold ends (the target, or the next slot)
    const s64 hold = std::max<s64>(0, release - ready);
    // The delay the last frame's hold put on this frame's GPU work (it waited behind that hold on
    // the game's queue, from when its first work was submitted, or the last frame was done, to the
    // hold's release; never more than the hold): off the sample, so the offset learns the frames'
    // own time. An estimate that is too large only lowers the offset (frames then come late and are
    // not held, so the next samples are exact): it cannot feed itself.
    s64 own_delay = 0;
    if (!release_queue.load(std::memory_order_relaxed) && times_id[(id - 1) % Ring] == id - 1 &&
        first_submit_id[id % Ring].load(std::memory_order_acquire) == id) {
        const s64 last_ready = times_ready[(id - 1) % Ring];
        const s64 last_release = times_release[(id - 1) % Ring];
        const s64 could_start =
            std::max(first_submit_at[id % Ring].load(std::memory_order_relaxed), last_ready);
        own_delay = std::clamp<s64>(last_release - could_start, 0, std::max<s64>(0, last_release - last_ready));
    }
    if (const u64 epoch = gpu_own_epoch.load(std::memory_order_acquire); epoch != gpu_own_epoch_seen) {
        gpu_own_epoch_seen = epoch; // a new paced stretch: its own history
        gpu_own_count = 0;
        // Stop turned it off; within its stay it stays on (a load does not make the GPU faster)
        if (bound_local && Now() - bound_since < bound_stay) {
            gpu_bound.store(true, std::memory_order_relaxed);
        } else if (bound_local) {
            bound_local = false;
            bound_left = Now();
        }
    }
    if (gpu_bound_check && !release_queue.load(std::memory_order_relaxed) &&
        times_id[(id - 1) % Ring] == id - 1 && flip_id[id % Ring].load(std::memory_order_acquire) == id) {
        const s64 from = std::max(times_release[(id - 1) % Ring], flip_at[id % Ring].load(std::memory_order_relaxed));
        // The hold's cost to this frame: its delay, but no more than the GPU's time after the later
        // of the hold's end and the frame's last submission (a frame whose work came in late, a CPU
        // spike, was not made late by the hold; a GPU that worked through a backlog was)
        const s64 cost = std::min(own_delay, ready - from);
        // all its work queued before the last hold ended (else the CPU made it late, not the hold)
        const bool queued_in_time = flip_at[id % Ring].load(std::memory_order_relaxed) < times_release[(id - 1) % Ring];
        hold_cost[gpu_own_count % hold_cost.size()] =
            queued_in_time && cost > 0 && ready > target && ready - cost <= target;
        gpu_own[gpu_own_count++ % gpu_own.size()] = ready - from;
        const s64 period = Libraries::VideoOut::FrameCapPeriodNs();
        if (period > 0 && (gpu_own_count & 15) == 0) {
            const u64 window = std::clamp<s64>(4'000'000'000 / period, 32, s64(gpu_own.size()));
            const u32 n = u32(std::min<u64>(gpu_own_count, window));
            std::array<s64, 512> copy;
            u32 costly = 0;
            for (u32 i = 0; i < n; ++i) {
                copy[i] = gpu_own[(gpu_own_count - 1 - i) % gpu_own.size()];
                costly += hold_cost[(gpu_own_count - 1 - i) % hold_cost.size()];
            }
            std::nth_element(copy.begin(), copy.begin() + n / 2, copy.begin() + n);
            const s64 median = copy[n / 2];
            gpu_own_median.store(median, std::memory_order_relaxed);
            hold_cost_permille.store(costly * 1000 / n, std::memory_order_relaxed);
            const bool was = gpu_bound.load(std::memory_order_relaxed);
            const s64 at = Now();
            // on: the GPU's own time near the period, or the holds making frames late (64 frames
            // at least, so a burst at the stretch's start does not decide it)
            const bool costs = n >= 64 && costly * 100 >= n * gpu_bound_late_pct;
            const bool now_bound = was ? median * 10 >= period * 8 || costs || at - bound_since < bound_stay
                                       : median * 10 >= period * 9 || costs;
            if (now_bound != was) {
                if (now_bound) {
                    // back within 60 s of going off: it costs again, so a longer stay
                    bound_stay = bound_left && at - bound_left < 60'000'000'000
                                     ? std::min<s64>(bound_stay * 2, 40'000'000'000)
                                     : 10'000'000'000;
                    bound_since = at;
                } else {
                    bound_left = at;
                }
                gpu_bound.store(now_bound, std::memory_order_relaxed);
                gpu_bound_changes.fetch_add(1, std::memory_order_relaxed);
                bound_local = now_bound;
                std::printf("Even presents: %s (GPU own time median %.2f ms of %.2f, %u of %u frames late by the "
                            "holds)\n",
                            now_bound ? "GPU-bound: paced frames not held" : "the GPU fits again: paced frames held",
                            double(median) / 1e6, double(period) / 1e6, costly, n);
                if (now_bound) {
                    std::printf("Even presents: GPU-bound for %.0f s at least\n", double(bound_stay) / 1e9);
                }
                std::fflush(stdout);
            }
        }
    }
    times_id[id % Ring] = id;
    times_ready[id % Ring] = ready;
    times_release[id % Ring] = release;
    own_delay_ns.fetch_add(own_delay, std::memory_order_relaxed);
    const u64 n = lag_count.load(std::memory_order_relaxed);
    lags[n % Samples].store(ready - slot - own_delay, std::memory_order_relaxed);
    lag_count.store(n + 1, std::memory_order_release);
    release_n.fetch_add(1, std::memory_order_relaxed);
    release_hold_ns.fetch_add(hold, std::memory_order_relaxed);
    if (ready > target) {
        late.fetch_add(1, std::memory_order_relaxed);
        late_ns.fetch_add(ready - target, std::memory_order_relaxed);
    } else {
        held.fetch_add(1, std::memory_order_relaxed);
        held_ns.fetch_add(hold, std::memory_order_relaxed);
        const s64 err = release - target; // the hold's own precision
        release_err_ns.fetch_add(std::abs(err), std::memory_order_relaxed);
        if (std::abs(err) > release_err_max.load(std::memory_order_relaxed)) {
            release_err_max.store(std::abs(err), std::memory_order_relaxed);
        }
        if (err > 200'000) {
            release_late.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

/// Without timed presents (build-1 path): the presenter, before game frame `id`'s present: notes
/// its ready time and holds it until its target.
void ReadyAndWait(u64 id) {
    FrameSlot& entry = frames[id % Ring];
    if (entry.id.load(std::memory_order_acquire) != id) {
        return;
    }
    const s64 slot = entry.slot.load(std::memory_order_relaxed);
    const s64 target = entry.target.load(std::memory_order_relaxed);
    const s64 now = Now();
    ready_at[id % Ring].store(now, std::memory_order_relaxed);
    ready_id[id % Ring].store(id, std::memory_order_release);
    const s64 gated = gate_before_id[id % Ring].load(std::memory_order_acquire) == id
                          ? gate_before_ns[id % Ring].load(std::memory_order_relaxed)
                          : 0;
    const u64 n = lag_count.load(std::memory_order_relaxed);
    lags[n % Samples].store(now - slot - gated, std::memory_order_relaxed);
    lag_count.store(n + 1, std::memory_order_release);
    if (target > now) {
        if (target - now < 100'000'000) {
            held.fetch_add(1, std::memory_order_relaxed);
            held_ns.fetch_add(target - now, std::memory_order_relaxed);
            SleepUntil(target);
        }
    } else {
        late.fetch_add(1, std::memory_order_relaxed);
        late_ns.fetch_add(now - target, std::memory_order_relaxed);
    }
}
} // namespace Pace

u32 MinimumIntervalUs() {
    if (Pace::enabled) {
        return 0; // the frame start paces (one pacer)
    }
    const u32 limit = EmulatorSettings.GetFrameLimit();
    return limit ? 1'000'000 / limit : 0;
}

void Marker(VkLatencyMarkerNV marker, u64 id) {
    if (!id) {
        return;
    }
    std::scoped_lock lk{swapchain_mutex};
    if (!swapchain) {
        return;
    }
    const VkSetLatencyMarkerInfoNV info{
        .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = marker};
    set_marker(device, swapchain, &info);
}

void AntiLag(VkAntiLagStageAMD stage, u64 id) {
    const VkAntiLagPresentationInfoAMD presentation{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .stage = stage,
        .frameIndex = id,
    };
    const VkAntiLagDataAMD data{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .mode = sleep_on ? VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD,
        .maxFPS = sleep_on && !Pace::enabled ? EmulatorSettings.GetFrameLimit() : 0, // one pacer
        .pPresentationInfo = &presentation,
    };
    anti_lag_update(device, &data);
}

/// Every 5 s: the driver's timings of the last frames, game input to the frame done.
void ReportTimings() {
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    VkLatencyTimingsFrameReportNV reports[64];
    for (auto& report : reports) {
        report = {.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV};
    }
    VkGetLatencyMarkerInfoNV info{.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,
                                  .timingCount = 64,
                                  .pTimings = reports};
    {
        std::scoped_lock lk{swapchain_mutex};
        if (!swapchain) {
            return;
        }
        get_timings(device, swapchain, &info);
    }
    double total = 0.0, sim_total = 0.0;
    u32 count = 0;
    for (u32 i = 0; i < std::min<u32>(info.timingCount, 64); ++i) {
        const auto& r = reports[i];
        if (!r.simStartTimeUs || r.presentEndTimeUs < r.simStartTimeUs) {
            continue;
        }
        total += double(r.presentEndTimeUs - r.simStartTimeUs);
        sim_total += double(r.simEndTimeUs > r.simStartTimeUs ? r.simEndTimeUs - r.simStartTimeUs
                                                               : 0);
        ++count;
    }
    if (!count) {
        return;
    }
    // Each frame from its own start (the game's frame start, before its input is read) to its own
    // present (the same game frame number on every marker).
    std::printf("Low latency: Reflex %s, %u frames: frame start to present %.1f ms, frame start "
                "to submit %.1f ms\n",
                sleep_on ? (boost ? "on + boost" : "on") : "off (markers only)", count,
                total / count / 1000.0, sim_total / count / 1000.0);
    std::fflush(stdout);
}

void SetSleepMode(bool on) {
    const VkLatencySleepModeInfoNV mode{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .lowLatencyMode = on ? VK_TRUE : VK_FALSE,
        .lowLatencyBoost = on && boost ? VK_TRUE : VK_FALSE,
        .minimumIntervalUs = on ? MinimumIntervalUs() : 0,
    };
    if (set_sleep_mode(device, swapchain, &mode) != VK_SUCCESS) {
        std::printf("Low latency: the driver refused the sleep mode\n");
    }
}

/// Moves `last` up to `id` when `id` is above it; false (and `last` = the value seen) if not.
bool Advance(std::atomic<u64>& last_id, u64 id, u64& last) {
    last = last_id.load(std::memory_order_relaxed);
    while (id > last) {
        if (last_id.compare_exchange_weak(last, id, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}


/// BB_LATENCY_TRACE=1: per game frame, when it entered its frame start, woke from the sleep, was
/// submitted, reached by the GPU, taken by the present thread and presented; about once a second
/// the mean and p99 of each step over the frames presented since, and the frame thread's waits per
/// frame by kind (runtime.c). Diagnosis only (a clock read per stage).
namespace Trace {
const bool on = [] {
    const char* env = std::getenv("BB_LATENCY_TRACE");
    return env && env[0] == '1';
}();
enum Stage : u32 {
    Enter, Awake, Submit, Gpu, Take, PresentStart, PresentEnd, Target, Paced,
    Ready,   ///< GPU holds: the frame's work done on the GPU
    Release, ///< GPU holds: its present work let go
    Stages
};
constexpr u64 Ring = 256;
struct Slot {
    std::atomic<u64> id{0};
    std::atomic<s64> t[Stages]{};
};
Slot ring[Ring];
u64 printed = 0; ///< the last id summed (frame thread)
/// Present to present of the game's frames over the whole run (frame thread): the pacing summary.
std::vector<float> intervals;
s64 last_present = 0;
u32 alternations = 0; ///< a long interval right after a short one or the other way (judder)

s64 Now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
void Stamp(u64 id, Stage stage) {
    if (!id) {
        return;
    }
    Slot& slot = ring[id % Ring];
    if (stage == Enter) {
        for (auto& t : slot.t) {
            t.store(0, std::memory_order_relaxed);
        }
        slot.id.store(id, std::memory_order_release);
    } else if (slot.id.load(std::memory_order_acquire) != id) {
        return;
    }
    slot.t[stage].store(Now(), std::memory_order_relaxed);
}
/// A stage's time taken elsewhere (the GPU's, for GPU holds).
void StampAt(u64 id, Stage stage, s64 t) {
    Slot& slot = ring[id % Ring];
    if (slot.id.load(std::memory_order_acquire) == id) {
        slot.t[stage].store(t, std::memory_order_relaxed);
    }
}
/// The frame's intended wake (the pacer's slot) and the time the pacer let it go.
void StampPace(u64 id) {
    Slot& slot = ring[id % Ring];
    if (slot.id.load(std::memory_order_acquire) != id) {
        return;
    }
    slot.t[Target].store(Pace::last_target, std::memory_order_relaxed);
    slot.t[Paced].store(Now(), std::memory_order_relaxed);
}
/// BB_LATENCY_FRAMES=<file> (with the trace): one CSV line per game frame, each stage in us
/// after the frame's start (-1: none), for offline pacing analysis.
FILE* frames_csv = [] {
    const char* path = std::getenv("BB_LATENCY_FRAMES");
    FILE* f = path && path[0] ? std::fopen(path, "w") : nullptr;
    if (f) {
        std::fprintf(f, "id,enter_us,target,paced,awake,submit,gpu,take,pstart,pend,period_us,ready,"
                        "release\n");
    }
    return f;
}();

extern "C" int runtime_frame_waits_take(const char** what, double* seconds, int max);
struct WaitSum {
    const char* what;
    double sum;
};
WaitSum waits[16];
u32 wait_kinds = 0;
u32 wait_frames = 0;

/// The frame thread at its frame start (before the sleep): its waits since the last frame start.
void TakeWaits() {
    const char* what[12];
    double sec[12];
    const int n = runtime_frame_waits_take(what, sec, 12);
    for (int i = 0; i < n; ++i) {
        u32 k = 0;
        while (k < wait_kinds && std::strcmp(waits[k].what, what[i])) {
            ++k;
        }
        if (k == wait_kinds) {
            if (k == 16) {
                continue;
            }
            waits[k] = {what[i], 0.0};
            ++wait_kinds;
        }
        waits[k].sum += sec[i];
    }
    ++wait_frames;
}

/// From the frame start (frame thread): about once a second, the frames since the last summary
/// that are presented by now.
void Print(u64 current) {
    static s64 window = Now();
    const s64 now = Now();
    if (now - window < 1'000'000'000) {
        return;
    }
    window = now;
    constexpr u32 Steps = 7;
    static const char* names[Steps] = {"sleep", "start->submit", "submit->gpu", "gpu->taken",
                                       "taken->presented", "start->presented", "period"};
    std::vector<double> v[Steps];
    const u64 from = std::max<u64>(printed + 1, current > Ring - 8 ? current - (Ring - 8) : 1);
    for (u64 id = from; id + 6 <= current; ++id) {
        const Slot& a = ring[id % Ring];
        if (a.id.load(std::memory_order_acquire) != id) {
            continue;
        }
        s64 t[Stages];
        for (u32 i = 0; i < Stages; ++i) {
            t[i] = a.t[i].load(std::memory_order_relaxed);
        }
        printed = id;
        if (frames_csv && t[Enter]) {
            static const s64 base = t[Enter];
            const auto rel = [&](u32 i) { return t[i] ? (t[i] - t[Enter]) / 1000 : s64{-1}; };
            std::fprintf(frames_csv, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                         static_cast<unsigned long long>(id),
                         static_cast<long long>((t[Enter] - base) / 1000),
                         static_cast<long long>(rel(Target)), static_cast<long long>(rel(Paced)),
                         static_cast<long long>(rel(Awake)), static_cast<long long>(rel(Submit)),
                         static_cast<long long>(rel(Gpu)), static_cast<long long>(rel(Take)),
                         static_cast<long long>(rel(PresentStart)),
                         static_cast<long long>(rel(PresentEnd)),
                         static_cast<long long>(Libraries::VideoOut::FrameCapPeriodNs() / 1000),
                         static_cast<long long>(rel(Ready)), static_cast<long long>(rel(Release)));
        }
        // presented: the present marker, else (the low latency mode off) the present thread taking it
        const s64 end = t[PresentEnd] ? t[PresentEnd] : t[Take];
        if (!t[Enter] || !t[Awake] || !end) {
            continue; // not presented (a load step, a dropped frame)
        }
        const auto ms = [](s64 x, s64 y) { return double(y - x) / 1e6; };
        v[0].push_back(ms(t[Enter], t[Awake]));
        if (t[Submit]) v[1].push_back(ms(t[Awake], t[Submit]));
        if (t[Submit] && t[Gpu]) v[2].push_back(ms(t[Submit], t[Gpu]));
        if (t[Gpu] && t[Take]) v[3].push_back(ms(t[Gpu], t[Take]));
        if (t[Take] && t[PresentEnd]) v[4].push_back(ms(t[Take], t[PresentEnd]));
        v[5].push_back(ms(t[Awake], end));
        if (last_present && end > last_present && intervals.size() < 4'000'000) {
            const double d = ms(last_present, end);
            if (d < 250.0) { // not across a load or a pause
                const double period =
                    double(Libraries::VideoOut::FrameCapPeriodNs()) / 1e6;
                if (!intervals.empty() && period > 0) {
                    const double prev = intervals.back();
                    alternations += (prev > 1.25 * period && d < 0.75 * period) ||
                                    (prev < 0.75 * period && d > 1.25 * period);
                }
                intervals.push_back(float(d));
            }
        }
        last_present = end;
        const Slot& b = ring[(id + 1) % Ring];
        const s64 next =
            b.id.load(std::memory_order_acquire) == id + 1 ? b.t[Enter].load(std::memory_order_relaxed) : 0;
        if (next) v[6].push_back(ms(t[Enter], next));
    }
    char line[1400];
    size_t at = std::snprintf(line, sizeof(line), "Latency trace: %zu frames", v[5].size());
    for (u32 i = 0; i < Steps && at < sizeof(line); ++i) {
        if (v[i].empty()) {
            continue;
        }
        double sum = 0;
        for (const double x : v[i]) sum += x;
        std::sort(v[i].begin(), v[i].end());
        const double p99 = v[i][std::min<size_t>(v[i].size() - 1, v[i].size() * 99 / 100)];
        at += std::snprintf(line + at, sizeof(line) - at, " | %s %.2f (p99 %.2f)", names[i],
                            sum / v[i].size(), p99);
    }
    if (wait_frames && at < sizeof(line)) {
        at += std::snprintf(line + at, sizeof(line) - at, " | frame thread waits per frame:");
        for (u32 k = 0; k < wait_kinds && at < sizeof(line); ++k) {
            if (waits[k].sum > 0) {
                at += std::snprintf(line + at, sizeof(line) - at, " %s %.2f", waits[k].what,
                                    waits[k].sum * 1000.0 / wait_frames);
            }
            waits[k].sum = 0;
        }
        wait_frames = 0;
    }
    if (at < sizeof(line)) {
        const u64 n = Pace::paced.exchange(0);
        at += std::snprintf(line + at, sizeof(line) - at,
                      " | paced: %llu frames, slept %.2f per frame, %llu not paced",
                      static_cast<unsigned long long>(n),
                      double(Pace::slept_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, n)),
                      static_cast<unsigned long long>(Pace::unpaced.exchange(0)));
    }
    if (Pace::even && at < sizeof(line)) {
        const u64 h = Pace::held.exchange(0), l = Pace::late.exchange(0);
        at += std::snprintf(line + at, sizeof(line) - at,
                            " | even: offset %.2f, held %llu by %.2f, late %llu by %.2f, gate %llu waits %.2f, %llu timeouts, %llu yields",
                            double(Pace::offset_ns.load()) / 1e6, static_cast<unsigned long long>(h),
                            double(Pace::held_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, h)),
                            static_cast<unsigned long long>(l),
                            double(Pace::late_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, l)),
                            static_cast<unsigned long long>(Pace::gate_waits.load()),
                            double(Pace::gate_wait_ns.exchange(0)) / 1e6 /
                                double(std::max<u64>(1, Pace::gate_waits.exchange(0))),
                            static_cast<unsigned long long>(Pace::gate_timeouts.exchange(0)),
                            static_cast<unsigned long long>(Pace::gate_yields.exchange(0)));
        if (Pace::gpu_release.load(std::memory_order_relaxed) && at < sizeof(line)) {
            const u64 r = Pace::release_n.exchange(0);
            at += std::snprintf(line + at, sizeof(line) - at,
                                " | GPU hold: %llu, hold %.2f, own delay %.2f, GPU own %.2f, late by holds %.1f%%%s (%llu changes), off target %.3f (max %.3f, %llu over 0.2)",
                                static_cast<unsigned long long>(r),
                                double(Pace::release_hold_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, r)),
                                double(Pace::own_delay_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, r)),
                                double(Pace::gpu_own_median.load()) / 1e6,
                                double(Pace::hold_cost_permille.load()) / 10.0,
                                Pace::gpu_bound.load() ? " GPU-BOUND: not held" : "",
                                static_cast<unsigned long long>(Pace::gpu_bound_changes.exchange(0)),
                                double(Pace::release_err_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, h)),
                                double(Pace::release_err_max.exchange(0)) / 1e6,
                                static_cast<unsigned long long>(Pace::release_late.exchange(0)));
        }
    }
    std::printf("%s (ms)\n", line);
    if (frames_csv) {
        std::fflush(frames_csv);
    }
    // every 10 s: the present intervals of the whole run so far
    static u32 seconds = 0;
    const double period = double(Libraries::VideoOut::FrameCapPeriodNs()) / 1e6;
    if (++seconds % 10 == 0 && intervals.size() > 100 && period > 0) {
        std::vector<float> sorted = intervals;
        std::sort(sorted.begin(), sorted.end());
        const auto pct = [&](double q) {
            return double(sorted[std::min<size_t>(sorted.size() - 1, size_t(double(sorted.size()) * q))]);
        };
        size_t on_slot = 0, short_ = 0, one_more = 0, more = 0;
        for (const float d : sorted) {
            if (d < 0.75 * period) ++short_;
            else if (d <= 1.25 * period) ++on_slot;
            else if (d <= 2.25 * period) ++one_more;
            else ++more;
        }
        std::printf("Present intervals (run so far, slot %.2f ms): %zu frames, p50 %.2f p99 %.2f "
                    "p99.9 %.2f max %.2f | short (<0.75 slot) %zu, on slot %zu, one slot more %zu, "
                    "longer %zu, alternating %u\n",
                    period, sorted.size(), pct(0.5), pct(0.99), pct(0.999), double(sorted.back()),
                    short_, on_slot, one_more, more, alternations);
    }
    std::fflush(stdout);
}
} // namespace Trace

} // namespace

bool StartPacingRequested() {
    return Pace::enabled;
}

s64 PresentTarget(s64 flip_arg) {
    return Pace::even && flip_arg >= 0 ? Pace::Target(static_cast<u64>(flip_arg) + 1) : 0;
}

bool EvenPresents() {
    return Pace::even && Pace::enabled;
}

s64 PresentHoldEnd(s64 flip_arg) {
    if (!Pace::even || flip_arg < 0 || Pace::gpu_bound.load(std::memory_order_relaxed)) {
        return 0;
    }
    return Pace::HoldEnd(static_cast<u64>(flip_arg) + 1);
}

void MarkOutOfBandPresentQueue(vk::Queue queue) {
    if (kind != Kind::Reflex || !device || !queue) {
        return;
    }
    const auto notify = reinterpret_cast<PFN_vkQueueNotifyOutOfBandNV>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkQueueNotifyOutOfBandNV"));
    if (notify) {
        const VkOutOfBandQueueTypeInfoNV info{.sType = VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV,
                                              .queueType = VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV};
        notify(static_cast<VkQueue>(queue), &info);
    }
}

void SetGpuHolds(int mode) {
    Pace::gpu_release.store(mode >= 1, std::memory_order_relaxed);
    Pace::release_queue.store(mode == 2, std::memory_order_relaxed);
    Pace::short_spin.store(mode == 4, std::memory_order_relaxed);
    std::printf("Even presents: paced frames held %s\n",
                mode == 4   ? "in the game's queue, submitted just before the target, the GPU spinning only the "
                              "last part (short spin)"
                : mode == 3 ? "in the game's queue, the GPU idle until our thread releases them (host-timed)"
                : mode == 2 ? "on the release queue"
                : mode == 1 ? "on the GPU in the game's queue"
                            : "on the presenter thread");
}

bool GpuRelease() {
    return Pace::gpu_release.load(std::memory_order_relaxed);
}

void PresentGpuTimes(s64 flip_arg, s64 ready_ns, s64 release_ns) {
    if (Pace::even && flip_arg >= 0) {
        Pace::GpuTimes(static_cast<u64>(flip_arg) + 1, ready_ns, release_ns);
        if (Trace::on) {
            Trace::StampAt(static_cast<u64>(flip_arg) + 1, Trace::Ready, ready_ns);
            Trace::StampAt(static_cast<u64>(flip_arg) + 1, Trace::Release, release_ns);
        }
    }
}

void PresentReadyAndWait(s64 flip_arg) {
    if (Pace::even && flip_arg >= 0 && !Pace::timed.load(std::memory_order_relaxed)) {
        Pace::ReadyAndWait(static_cast<u64>(flip_arg) + 1);
    }
}

void GateAfterFlip(s64 flip_arg, u64 draw_tick) {
    if (flip_arg >= 0) {
        Pace::gpu_flip_arg.store(flip_arg, std::memory_order_relaxed);
    }
    if (Pace::even && flip_arg >= 0) {
        Pace::GateNote(static_cast<u64>(flip_arg) + 1, Libraries::VideoOut::FrameCapPeriodNs(),
                       draw_tick);
    }
}

bool GateNeeded(u64 tick) {
    return Pace::even && Pace::GateNeeded(tick);
}

void GateBeforeSubmit(u64 tick, bool recorder) {
    if (Pace::even) {
        Pace::GateBeforeSubmit(tick, recorder);
    }
}

void GateYieldBegin() {
    Pace::GateYieldBegin();
}

void GateYieldEnd() {
    Pace::GateYieldEnd();
}

void GatePostSubmitted(s64 flip_arg) {
    if (Pace::even && flip_arg >= 0) {
        Pace::GatePost(flip_arg);
    }
}

void SetTimedPresents(bool on) {
    Pace::timed.store(on, std::memory_order_relaxed);
}

void PresentQueued(s64 flip_arg) {
    if (Pace::even && flip_arg >= 0 && Pace::timed.load(std::memory_order_relaxed)) {
        Pace::Sample(static_cast<u64>(flip_arg) + 1, Pace::Now());
    }
}

void PresentFlipped(s64 flip_arg, s64 flip_ns) {
    if (!Pace::even || flip_arg < 0) {
        return;
    }
    const u64 id = static_cast<u64>(flip_arg) + 1;
    const s64 target = Pace::Target(id);
    if (target && flip_ns > target + 200'000) {
        Pace::Sample(id, flip_ns); // late: the offset learns the real ready time
        Pace::late.fetch_add(1, std::memory_order_relaxed);
        Pace::late_ns.fetch_add(flip_ns - target, std::memory_order_relaxed);
    } else if (target) {
        Pace::held.fetch_add(1, std::memory_order_relaxed);
    }
}

s64 FrameStepNs() {
    // The frame being recorded is the one after the GPU thread's last game flip (ids: flip + 1).
    const s64 flip = Pace::gpu_flip_arg.load(std::memory_order_relaxed);
    if (flip < 0) {
        return 0;
    }
    const u64 id = static_cast<u64>(flip) + 2;
    const s64 now = Pace::StepAt(id), before = Pace::StepAt(id - 1);
    return now && before && now > before ? now - before : 0;
}

s64 PresentLeadNs() {
    const s64 target = Pace::current_target.load(std::memory_order_relaxed);
    return target ? target - Pace::Now() : 0;
}

void TraceFlip(FlipStage stage, s64 flip_arg) {
    if (flip_arg < 0) {
        return;
    }
    if (stage == FlipStage::GpuReached) {
        Pace::Queued(static_cast<u64>(flip_arg) + 1);
    }
    if (!Trace::on) {
        return;
    }
    Trace::Stamp(static_cast<u64>(flip_arg) + 1,
                 stage == FlipStage::GpuReached ? Trace::Gpu : Trace::Take);
}

void* PrepareDevice(vk::PhysicalDevice physical, const std::vector<std::string>& available,
                    std::vector<const char*>& extensions) {
    const char setting = Setting();
    if (setting == '0') {
        return nullptr;
    }
    const auto has = [&](const char* name) {
        return std::find(available.begin(), available.end(), name) != available.end();
    };
    if (has(VK_NV_LOW_LATENCY_2_EXTENSION_NAME)) {
        kind = Kind::Reflex;
        extensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
        void* next = nullptr;
        if (has(VK_KHR_PRESENT_ID_EXTENSION_NAME)) {
            VkPhysicalDevicePresentIdFeaturesKHR query{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                               .pNext = &query};
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical, &features);
            if (query.presentId) {
                present_id = true;
                extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                present_id_features.presentId = VK_TRUE;
                next = &present_id_features;
            }
        }
        sleep_on = setting != 'm';
        boost = setting == '2';
        return next;
    }
    if (has(VK_AMD_ANTI_LAG_EXTENSION_NAME)) {
        VkPhysicalDeviceAntiLagFeaturesAMD query{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &query};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical, &features);
        if (query.antiLag) {
            kind = Kind::AntiLag;
            sleep_on = setting != 'm';
            extensions.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
            anti_lag_features.antiLag = VK_TRUE;
            return &anti_lag_features;
        }
    }
    std::printf("Low latency: not supported by this GPU or driver (NVIDIA Reflex needs "
                "VK_NV_low_latency2, AMD Anti-Lag 2 VK_AMD_anti_lag)\n");
    return nullptr;
}

void OnDevice(vk::Device dev) {
    if (kind == Kind::Off) {
        return;
    }
    device = dev;
    const auto load = [&](const char* name) {
        return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
    };
    if (kind == Kind::Reflex) {
        set_sleep_mode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(load("vkSetLatencySleepModeNV"));
        latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(load("vkLatencySleepNV"));
        set_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(load("vkSetLatencyMarkerNV"));
        get_timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(load("vkGetLatencyTimingsNV"));
        VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                       .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        const VkSemaphoreCreateInfo create{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                           .pNext = &type};
        if (!set_sleep_mode || !latency_sleep || !set_marker || !get_timings ||
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(device, &create, nullptr,
                                                            &sleep_semaphore) != VK_SUCCESS) {
            std::printf("Low latency: Reflex functions missing, off\n");
            kind = Kind::Off;
            return;
        }
        std::printf("Low latency: NVIDIA Reflex %s%s\n",
                    sleep_on ? "on" : "off (markers only)", boost ? " + boost" : "");
    } else {
        anti_lag_update = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(load("vkAntiLagUpdateAMD"));
        if (!anti_lag_update) {
            std::printf("Low latency: Anti-Lag function missing, off\n");
            kind = Kind::Off;
            return;
        }
        std::printf("Low latency: AMD Anti-Lag 2 %s\n", sleep_on ? "on" : "off");
    }
    std::fflush(stdout);
}

const void* SwapchainNext() {
    return kind == Kind::Reflex ? &swapchain_latency : nullptr;
}

void OnSwapchain(vk::SwapchainKHR chain) {
    if (kind != Kind::Reflex) {
        return;
    }
    std::scoped_lock lk{swapchain_mutex};
    swapchain = chain;
    SetSleepMode(sleep_on && !no_source.load(std::memory_order_relaxed));
}

void OnSwapchainDestroy() {
    if (kind != Kind::Reflex) {
        return;
    }
    std::scoped_lock lk{swapchain_mutex};
    swapchain = VK_NULL_HANDLE;
}

void SetFrameSource(bool found) {
    // the frame rate cap at the frame start: with or without the low latency mode, on any GPU
    if (found && Pace::enabled) {
        Pace::active.store(true, std::memory_order_relaxed);
        std::printf("Frame pacing: the frame rate cap is kept at the game's frame start (before "
                    "its input; frames shown as soon as they are done; BB_LATENCY_PACING=0: off)\n");
        std::fflush(stdout);
    }
    if (kind == Kind::Off) {
        return;
    }
    if (found) {
        std::printf("Low latency: frame ids from the game's own frame number (start, submit, "
                    "present)%s\n",
                    log_on ? "; BB_REFLEX_LOG=1: the id sequence once a second" : "");
        std::fflush(stdout);
        return;
    }
    // No frame start: no correct place for the sleep and no id for the frame's start. Markers
    // with guessed ids would pace the driver against the wrong frames: off instead.
    no_source.store(true, std::memory_order_relaxed);
    if (kind == Kind::Reflex) {
        std::scoped_lock lk{swapchain_mutex};
        if (swapchain) {
            SetSleepMode(false);
        }
    }
    std::printf("Low latency: off (the game's frame start was not found in this program)\n");
    std::fflush(stdout);
}

void GameFrameStart(u64 frame, u64 kicked) {
    if (!On()) {
        // the frame rate cap alone (the low latency mode off or not supported here)
        const u64 id = frame + 1;
        static u64 last_alone = 0; // the frame thread's: a second entry in one frame is no start
        if (id > last_alone) {
            last_alone = id;
            if (Trace::on) {
                Trace::TakeWaits();
                Trace::Stamp(id, Trace::Enter);
                Trace::Print(id);
            }
            Pace::BeforeFrame(id);
            if (Trace::on) {
                Trace::StampPace(id);
                Trace::Stamp(id, Trace::Awake);
            }
        }
        return;
    }
    const u64 id = frame + 1;
    u64 last = 0;
    if (!Advance(started, id, last)) {
        // The frame function entered again in the same frame: not a new frame.
        if (log_on) {
            start_log.Repeat(last, id);
            PrintLog();
        }
        return;
    }
    if (Trace::on) {
        Trace::TakeWaits();
        Trace::Stamp(id, Trace::Enter);
        Trace::Print(id);
    }
    if (log_on) {
        start_log.Taken(last, id, 0);
        if (kicked != ~0ull) {
            const s64 lag = static_cast<s64>(frame - kicked);
            s64 v = kick_lag_min.load(std::memory_order_relaxed);
            while (lag < v && !kick_lag_min.compare_exchange_weak(v, lag)) {
            }
            v = kick_lag_max.load(std::memory_order_relaxed);
            while (lag > v && !kick_lag_max.compare_exchange_weak(v, lag)) {
            }
        }
        PrintLog();
    }
    Pace::BeforeFrame(id);
    if (Trace::on) {
        Trace::StampPace(id);
    }
    if (kind == Kind::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_INPUT_AMD, id); // the driver sleeps here when it is on
        if (Trace::on) {
            Trace::Stamp(id, Trace::Awake);
        }
        return;
    }
    if (sleep_on) {
        u64 value = 0;
        bool sleeping = false;
        {
            std::scoped_lock lk{swapchain_mutex};
            if (swapchain) {
                value = ++sleep_value;
                const VkLatencySleepInfoNV info{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
                                                .signalSemaphore = sleep_semaphore,
                                                .value = value};
                sleeping = latency_sleep(device, swapchain, &info) == VK_SUCCESS;
            }
        }
        if (sleeping) {
            // The driver signals when the game should start; never more than 50 ms (a swapchain
            // made again meanwhile, a driver that stops pacing).
            const VkSemaphoreWaitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                           .semaphoreCount = 1,
                                           .pSemaphores = &sleep_semaphore,
                                           .pValues = &value};
            VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(device, &wait, 50'000'000);
        }
    }
    if (Trace::on) {
        Trace::Stamp(id, Trace::Awake);
    }
    Marker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
    Marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
}

void InputSample() {
    if (log_on && On()) {
        pad_reads.fetch_add(1, std::memory_order_relaxed);
    }
}

void RenderSubmit(s64 flip_arg) {
    if (!On() || flip_arg < 0) {
        return;
    }
    const u64 id = static_cast<u64>(flip_arg) + 1;
    const u64 start = started.load(std::memory_order_acquire);
    if (id > start) {
        // Not a frame that started (a flip before the game's loop, another argument).
        if (log_on) {
            submit_log.Refused(start, id);
        }
        return;
    }
    u64 last = 0;
    if (!Advance(submitted, id, last)) {
        if (log_on) {
            submit_log.Repeat(last, id);
        }
        return;
    }
    if (log_on) {
        submit_log.Taken(last, id, static_cast<s64>(start - id));
    }
    if (Trace::on) {
        Trace::Stamp(id, Trace::Submit);
    }
    if (kind == Kind::Reflex) {
        Marker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
    }
}

u64 PresentId(s64 flip_arg) {
    if (!On() || flip_arg < 0) {
        return 0;
    }
    const u64 id = static_cast<u64>(flip_arg) + 1;
    const u64 submit = submitted.load(std::memory_order_acquire);
    if (id > submit) {
        if (log_on) {
            present_log.Refused(submit, id);
        }
        return 0;
    }
    u64 last = 0;
    if (!Advance(presented, id, last)) {
        if (log_on) {
            present_log.Repeat(last, id);
        }
        return 0; // present ids must go up (VK_KHR_present_id)
    }
    if (log_on) {
        present_log.Taken(last, id, static_cast<s64>(submit - id));
    }
    return id;
}

void PresentStart(u64 id) {
    if (!id || !On()) {
        return;
    }
    if (Trace::on) {
        Trace::Stamp(id, Trace::PresentStart);
    }
    if (kind == Kind::Reflex) {
        Marker(VK_LATENCY_MARKER_PRESENT_START_NV, id);
    } else if (kind == Kind::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_PRESENT_AMD, id);
    }
}

void PresentEnd(u64 id) {
    if (!id || kind != Kind::Reflex || !On()) {
        return;
    }
    Marker(VK_LATENCY_MARKER_PRESENT_END_NV, id);
    if (Trace::on) {
        Trace::Stamp(id, Trace::PresentEnd);
    }
    ReportTimings();
}

bool UsesPresentId() {
    return kind == Kind::Reflex && present_id;
}

const char* Name() {
    if (!sleep_on || !On()) {
        return nullptr;
    }
    return kind == Kind::Reflex ? "Reflex" : kind == Kind::AntiLag ? "Anti-Lag 2" : nullptr;
}

} // namespace Vulkan::LowLatency
