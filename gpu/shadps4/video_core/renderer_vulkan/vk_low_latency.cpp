// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: low latency mode (vk_low_latency.h).

#include "video_core/renderer_vulkan/vk_low_latency.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/emulator_settings.h"
#include "core/libraries/videoout/driver.h"

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
/// by holding finished frames: the present thread shows a flip only on its vblank tick (Frame
/// rate) or slot (Frame rate limit), so under a cap the game ran ahead and its frames queued there,
/// after their input was read (U3 at 144: 5.8 ms waiting for the tick, start to present 28 ms).
/// Here the wait moves to the frame's start, before its input: the frame starts as late as lets it
/// reach the present thread just before the tick it is shown on (vblank ticks: phase-locked to the
/// present thread's clock, one frame per tick) or one limit period after the last one (a frame
/// limit; the limiter's hold then stands aside: one pacer). `lead`: how long frames take from
/// their start to the present thread's queue (95th percentile of the last 64 frames, measured,
/// adapting as the scene gets heavier or lighter), plus a margin.
namespace Pace {
const bool enabled = [] {
    const char* env = std::getenv("BB_LATENCY_PACING");
    return !(env && env[0] == '0');
}();
/// How early a frame should reach the present thread before its tick (the GPU's jitter).
const s64 margin_ns = [] {
    const char* env = std::getenv("BB_LATENCY_PACING_MARGIN_US");
    const long us = env && env[0] ? std::strtol(env, nullptr, 10) : 500;
    return s64(std::clamp<long>(us, 0, 5000)) * 1000;
}();
/// On once the frame start is hooked and the sleep is on (SetFrameSource).
std::atomic<bool> active{false};

s64 Now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

constexpr u64 Ring = 256;
/// Per frame (by id): when it really started (after the sleeps) and the tick it was paced for (0:
/// none). Written by the frame thread before the frame's flip can exist; read at its queueing.
std::atomic<u64> frame_id[Ring]{};
std::atomic<s64> frame_awake[Ring]{};
std::atomic<s64> frame_target[Ring]{};

/// Measured by the GPU thread when a frame's flip reaches the present thread's queue (mutex):
/// the frame's lead (start to queued), and whether it came after its tick (shown a tick later; a
/// count for the trace). The present thread notes the last frame it took.
std::mutex mutex;
constexpr u32 Samples = 128;
s64 leads[Samples]{};
u32 lead_count = 0, lead_next = 0;
/// How much earlier than its aimed time the tick that showed each recent frame came (0 if not):
/// the present thread's vblank timer wakes up to ~1.5 ms off its grid (it catches up on the next
/// tick), so a frame has to be there that much before the grid's time too.
s64 tick_early[Samples]{};
u32 tick_count = 0, tick_next = 0;
std::atomic<u64> late_frames{0}, skipped_ticks{0}, stops{0}, planned{0}; ///< counts for BB_LATENCY_TRACE
std::atomic<s64> planned_sleep_ns{0}, lead_ns{0}, late_by_max{0}, tick_err_min{INT64_MAX}, tick_err_max{INT64_MIN};
std::atomic<u64> shown_later{0};
u64 taken_id = 0;
s64 taken_ns = 0;
/// The longest lead of the paced frames (mutex), held for 10 s and then let down by 0.5 ms a
/// second: a frame that takes longer than the frames before it is late for its tick (the display
/// repeats one), and the slow frames of a scene come back every few seconds, so the frames start
/// as early as the slowest one of the last seconds needed. 0: not paced yet (the next lock starts
/// it from the measured frames).
s64 lead_peak = 0;
s64 lead_peak_at = 0;

/// Frame thread state.
s64 last_target = 0;   ///< the last frame's tick
s64 last_wake = 0;     ///< the last frame's start (a frame limit)
u64 skip_until = 0;    ///< no other tick skipped before this frame (the last skip takes effect)

void Queued(u64 id) {
    const u64 slot = id % Ring;
    if (frame_id[slot].load(std::memory_order_acquire) != id) {
        return;
    }
    const s64 now = Now();
    const s64 lead = now - frame_awake[slot].load(std::memory_order_relaxed);
    const s64 target = frame_target[slot].load(std::memory_order_relaxed);
    const s64 period = Libraries::VideoOut::CurrentFlipSchedule().period_ns;
    std::scoped_lock lk{mutex};
    if (lead > 0 && lead < 250'000'000) {
        leads[lead_next] = lead;
        lead_next = (lead_next + 1) % Samples;
        lead_count = std::min(lead_count + 1, Samples);
        if (target && lead_peak && lead <= lead_peak + lead_peak / 2 + 1'000'000) {
            // (a much slower frame is a hitch: late whatever the lead, not a reason to start
            // every frame earlier)
            if (lead >= lead_peak) {
                lead_peak = lead;
                lead_peak_at = now;
            } else if (now - lead_peak_at > 10'000'000'000) {
                lead_peak = std::max(lead_peak - std::max<s64>(period, 0) / 2000, lead);
            }
        }
    }
    if (target && target < now) {
        late_frames.fetch_add(1, std::memory_order_relaxed);
        s64 v = late_by_max.load(std::memory_order_relaxed);
        while (now - target > v && !late_by_max.compare_exchange_weak(v, now - target)) {
        }
    }
}

void Taken(u64 id) {
    const s64 now = Now();
    if (frame_id[id % Ring].load(std::memory_order_acquire) == id) {
        const s64 target = frame_target[id % Ring].load(std::memory_order_relaxed);
        if (target) {
            const s64 err = now - target; // the tick it was shown on against the one it aimed at
            s64 v = tick_err_min.load(std::memory_order_relaxed);
            while (err < v && !tick_err_min.compare_exchange_weak(v, err)) {
            }
            v = tick_err_max.load(std::memory_order_relaxed);
            while (err > v && !tick_err_max.compare_exchange_weak(v, err)) {
            }
            if (err > 3'000'000) {
                shown_later.fetch_add(1, std::memory_order_relaxed);
            }
            const s64 period = Libraries::VideoOut::CurrentFlipSchedule().period_ns;
            std::scoped_lock lk{mutex};
            // a frame shown a whole tick before the aimed one (it came far earlier than needed)
            // says nothing about the timer
            tick_early[tick_next] = err < 0 && err > -period / 2 ? -err : 0;
            tick_next = (tick_next + 1) % Samples;
            tick_count = std::min(tick_count + 1, Samples);
        }
    }
    std::scoped_lock lk{mutex};
    if (id > taken_id) {
        taken_id = id;
        taken_ns = now;
    }
}

/// The k-th smallest of the first n values.
s64 Kth(s64* v, u32 n, u32 k) {
    std::nth_element(v, v + k, v + n);
    return v[k];
}

void Stop() {
    {
        std::scoped_lock lk{mutex};
        lead_peak = 0;
    }
    last_target = 0;
    last_wake = 0;
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
void BeforeFrame(u64 id) {
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    const u64 slot = id % Ring;
    frame_target[slot].store(0, std::memory_order_relaxed);
    const auto schedule = Libraries::VideoOut::CurrentFlipSchedule();
    const s64 now = Now();
    const s64 period = schedule.period_ns;
    s64 lead_v[Samples];
    u32 n_lead;
    s64 timer_early = 0;
    u64 shown_id;
    s64 shown_ns;
    s64 peak;
    {
        std::scoped_lock lk{mutex};
        peak = lead_peak;
        n_lead = lead_count;
        std::copy(leads, leads + n_lead, lead_v);
        if (tick_count) {
            timer_early = *std::max_element(tick_early, tick_early + tick_count);
        }
        shown_id = taken_id;
        shown_ns = taken_ns;
    }
    // no schedule (movie, fast boot vblank, frozen, flip rate), the present thread not ticking
    // (a stale schedule), frames that do not reach it (a load: starts without flips), too few
    // measured frames yet
    if (period <= 0 || schedule.next_ns < now - 4 * period || !shown_id || shown_id + 8 < id ||
        n_lead < 16) {
        Stop();
        stops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s64 target;
    s64 wake;
    if (schedule.on_ticks) {
        // The lead: the held longest one of the paced frames (from the 90th percentile of the
        // frames measured so far when pacing starts), how early the timer's ticks came, and the
        // margin. A frame late for its tick is shown a tick later (the display repeats one) and a
        // tick has to be left out to get back, so frames start as early as the slowest recent one
        // needed against the earliest tick. At most two periods: a longer lead puts so many
        // frames in flight that the game itself waits for them (its own hold, as with the old
        // limiter), which the lead would then measure and keep.
        if (!peak) {
            // from the measured frames, at most one period: before pacing they include the wait
            // the old hold put into the game (it goes away once the frames are paced)
            peak = std::min(Kth(lead_v, n_lead, n_lead * 90 / 100), period);
            std::scoped_lock lk{mutex};
            lead_peak = peak;
            lead_peak_at = now;
        }
        const s64 lead_used = std::min(peak, 2 * period) + timer_early + margin_ns;
        // one frame per tick, after the last frame's, not before the frame can be there
        const auto tick_at = [&](s64 earliest) {
            const s64 k = earliest > schedule.next_ns
                              ? (earliest - schedule.next_ns + period - 1) / period
                              : 0;
            return schedule.next_ns + k * period;
        };
        target = tick_at(std::max(now + lead_used, last_target ? last_target + period / 2 : 0));
        // Ticks in a row found a frame still queued behind the one they showed (a frame came late,
        // or the queue the old hold built): at the cap every later frame would wait a tick for
        // ever. Leave one tick out (the display shows the last frame once more) and the queue is
        // empty again. (Below the cap a slow frame drains it by itself: not counted then.)
        if (schedule.backlogs >= 8) {
            if (id >= skip_until) {
                target += period;
                skipped_ticks.fetch_add(1, std::memory_order_relaxed);
                // once until the frames now in flight and this one are shown
                skip_until = id + (id > shown_id ? id - shown_id : 0) + 2;
            }
        }
        wake = target - lead_used;
        if (wake - now > 2 * period) {
            // the schedule ran ahead of the game (a frame that did not come for a while): from now
            target = tick_at(now + lead_used);
            wake = target - lead_used;
        }
        lead_ns.store(lead_used, std::memory_order_relaxed);
    } else {
        // a frame limit (shown as it comes, the limiter's hold aside): one period after the last
        // frame's start
        wake = std::max(now, last_wake ? last_wake + period : 0);
        target = 0;
        Libraries::VideoOut::SetStartPaced(true);
    }
    last_target = target;
    last_wake = wake;
    if (target) {
        frame_target[slot].store(target, std::memory_order_relaxed);
    }
    planned.fetch_add(1, std::memory_order_relaxed);
    planned_sleep_ns.fetch_add(wake - now, std::memory_order_relaxed);
    if (wake > now) {
        SleepUntil(wake);
    }
}

/// After the sleeps (the frame really starts).
void Awake(u64 id) {
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    const u64 slot = id % Ring;
    frame_awake[slot].store(Now(), std::memory_order_relaxed);
    frame_id[slot].store(id, std::memory_order_release);
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
enum Stage : u32 { Enter, Awake, Submit, Gpu, Take, PresentStart, PresentEnd, Stages };
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
        if (!t[Enter] || !t[Awake] || !t[PresentEnd]) {
            continue; // not presented (a load step, a dropped frame)
        }
        const auto ms = [](s64 x, s64 y) { return double(y - x) / 1e6; };
        v[0].push_back(ms(t[Enter], t[Awake]));
        if (t[Submit]) v[1].push_back(ms(t[Awake], t[Submit]));
        if (t[Submit] && t[Gpu]) v[2].push_back(ms(t[Submit], t[Gpu]));
        if (t[Gpu] && t[Take]) v[3].push_back(ms(t[Gpu], t[Take]));
        if (t[Take]) v[4].push_back(ms(t[Take], t[PresentEnd]));
        v[5].push_back(ms(t[Awake], t[PresentEnd]));
        if (last_present && t[PresentEnd] > last_present && intervals.size() < 4'000'000) {
            const double d = ms(last_present, t[PresentEnd]);
            if (d < 250.0) { // not across a load or a pause
                const double period =
                    double(Libraries::VideoOut::CurrentFlipSchedule().period_ns) / 1e6;
                if (!intervals.empty() && period > 0) {
                    const double prev = intervals.back();
                    alternations += (prev > 1.25 * period && d < 0.75 * period) ||
                                    (prev < 0.75 * period && d > 1.25 * period);
                }
                intervals.push_back(float(d));
            }
        }
        last_present = t[PresentEnd];
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
        std::snprintf(line + at, sizeof(line) - at, " | paced: %llu late (by up to %.2f), %llu shown later than aimed, shown-aimed %.2f..%.2f, %llu ticks skipped, %llu stops, planned sleep %.2f, lead %.2f",
                      static_cast<unsigned long long>(Pace::late_frames.exchange(0)),
                      double(Pace::late_by_max.exchange(0)) / 1e6,
                      static_cast<unsigned long long>(Pace::shown_later.exchange(0)),
                      double(Pace::tick_err_min.exchange(INT64_MAX)) / 1e6,
                      double(Pace::tick_err_max.exchange(INT64_MIN)) / 1e6,
                      static_cast<unsigned long long>(Pace::skipped_ticks.exchange(0)),
                      static_cast<unsigned long long>(Pace::stops.exchange(0)),
                      double(Pace::planned_sleep_ns.exchange(0)) / 1e6 /
                          double(std::max<u64>(1, Pace::planned.exchange(0))),
                      double(Pace::lead_ns.load()) / 1e6);
    }
    std::printf("%s (ms)\n", line);
    // every 10 s: the present intervals of the whole run so far
    static u32 seconds = 0;
    const double period = double(Libraries::VideoOut::CurrentFlipSchedule().period_ns) / 1e6;
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

bool PacedForLater(s64 flip_arg, s64 tick_ns) {
    if (flip_arg < 0 || !Pace::active.load(std::memory_order_relaxed)) {
        return false;
    }
    const u64 id = static_cast<u64>(flip_arg) + 1;
    const u64 slot = id % Pace::Ring;
    if (Pace::frame_id[slot].load(std::memory_order_acquire) != id) {
        return false;
    }
    const s64 target = Pace::frame_target[slot].load(std::memory_order_relaxed);
    return target && target > tick_ns;
}

void TraceFlip(FlipStage stage, s64 flip_arg) {
    if (flip_arg < 0) {
        return;
    }
    if (stage == FlipStage::GpuReached) {
        Pace::Queued(static_cast<u64>(flip_arg) + 1);
    } else if (Pace::active.load(std::memory_order_relaxed)) {
        Pace::Taken(static_cast<u64>(flip_arg) + 1);
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
    if (kind == Kind::Off) {
        return;
    }
    if (found) {
        std::printf("Low latency: frame ids from the game's own frame number (start, submit, "
                    "present)%s\n",
                    log_on ? "; BB_REFLEX_LOG=1: the id sequence once a second" : "");
        if (sleep_on && Pace::enabled) {
            Pace::active.store(true, std::memory_order_relaxed);
            std::printf("Low latency: frame pacing at the frame start (the frame rate cap waits "
                        "before the frame's input, not after it; margin %lld us; "
                        "BB_LATENCY_PACING=0: off)\n",
                        static_cast<long long>(Pace::margin_ns / 1000));
        }
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
    if (kind == Kind::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_INPUT_AMD, id); // the driver sleeps here when it is on
        Pace::Awake(id);
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
    Pace::Awake(id);
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
