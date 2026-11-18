// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: low latency mode (vk_low_latency.h).

#include "bbport_trace.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"

#include <algorithm>
#include <array>
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
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "../../../dlss_bridge/bbport_dlss_bridge.h"

void BbHitchMark(char code); // gpu/shim/bbport_sampler.cpp (BB_HITCH_TIMELINE)

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

/// NVIDIA Reflex PC Latency Stats (bbport_dlss.dll's optional exports, its bbport_pclstats.cpp): the same markers
/// and ids as an ETW event, for PresentMon, FrameView and the NVIDIA overlay's PC latency. Set once in OnDevice
/// (Reflex only; BB_PCLSTATS=0: off); null: none.
BbPclStatsMarkerFn pcl_marker = nullptr;

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

/// Even presents (BB_EVEN_PRESENT=0: off). Shown as soon as it is done, a frame's present interval
/// was the period plus the change in its render time (the frame start's slot plus its own time):
/// under G-SYNC each refresh lasts that interval (QD-OLED: brightness flicker, U3 at 120 on a
/// 165 Hz panel), and the paced frame clock stepped the world by the start intervals while the
/// frames were shown by their end times (Nate on the horse stepped against the display). Each
/// paced frame now gets a present target = its slot + an offset L, and its world step is the
/// interval between successive targets (the frame clock reads the target's TSC, probe.c): slots
/// are one period apart, so targets are too, presents are held to them, and world steps equal
/// present intervals. L = about the 98th percentile of (arrival at the presenter - slot) over the
/// last ~2 s plus a margin: raised at once, lowered by 20 us a frame (no visible step). A frame
/// later than its target is shown when it comes (as before).
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
/// presenter (swap thread) -> frame thread: arrival - slot of the last frames
constexpr u32 Samples = 256;
std::array<std::atomic<s64>, Samples> lags{};
std::atomic<u64> lag_count{0};
s64 offset = 0;      ///< frame thread: L
s64 offset_goal = 0; ///< frame thread: L from the samples
std::atomic<s64> current_target{0}; ///< frame thread -> probe.c: this frame's present target (0: none)
std::atomic<s64> offset_ns{0};      ///< for the log
std::atomic<u64> held{0}, late{0};
std::atomic<s64> held_ns{0}, late_ns{0};

void UpdateOffset(s64 period, u64 id) {
    if ((id & 15) == 0) {
        const u64 count = lag_count.load(std::memory_order_acquire);
        const u32 n = static_cast<u32>(std::min<u64>(count, Samples - 16));
        if (n >= 32) {
            std::array<s64, Samples> copy;
            for (u32 i = 0; i < n; ++i) {
                copy[i] = lags[(count - 1 - i) % Samples].load(std::memory_order_relaxed);
            }
            const u32 k = n * 98 / 100;
            std::nth_element(copy.begin(), copy.begin() + k, copy.begin() + n);
            constexpr s64 margin = 400'000; // the present thread's take, the timer's spin
            offset_goal = std::clamp<s64>(copy[k] + margin, 0, 2 * period);
        }
    }
    if (offset_goal > offset) {
        offset = offset_goal;
    } else if (offset_goal + 250'000 < offset) {
        offset -= std::min<s64>(offset - offset_goal, 20'000);
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
    current_target.store(0, std::memory_order_relaxed);
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
    const s64 period = Libraries::VideoOut::FrameCapPeriodNs();
    const s64 now = Now();
    // No cap now (a movie, the boot's fast vblank, the game frozen), or frames that do not reach
    // the present thread (a load: the cap would only slow it): not paced, and the present
    // thread's slot hold keeps the cap meanwhile.
    last_target = 0;
    if (period <= 0 || queued_last.load(std::memory_order_relaxed) + 8 < id) {
        Stop();
        unpaced.fetch_add(1, std::memory_order_relaxed);
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
    }
    Libraries::VideoOut::SetStartPaced(true);
    paced.fetch_add(1, std::memory_order_relaxed);
    if (wake > now) {
        slept_ns.fetch_add(wake - now, std::memory_order_relaxed);
        SleepUntil(wake);
    }
}
/// The presenter, before the present of game frame `id`: holds it until its target.
void Hold(u64 id) {
    FrameSlot& entry = frames[id % Ring];
    if (entry.id.load(std::memory_order_acquire) != id) {
        return;
    }
    const s64 slot = entry.slot.load(std::memory_order_relaxed);
    const s64 target = entry.target.load(std::memory_order_relaxed);
    const s64 now = Now();
    const u64 n = lag_count.load(std::memory_order_relaxed);
    lags[n % Samples].store(now - slot, std::memory_order_relaxed);
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
    if (pcl_marker) {
        pcl_marker(static_cast<uint32_t>(marker), id); // an ETW flag test when no tool listens
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
enum Stage : u32 { Enter, Awake, Submit, Gpu, Take, PresentStart, PresentEnd, Target, Paced, Stages };
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
        std::fprintf(f, "id,enter_us,target,paced,awake,submit,gpu,take,pstart,pend,period_us\n");
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
            std::fprintf(frames_csv, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                         static_cast<unsigned long long>(id),
                         static_cast<long long>((t[Enter] - base) / 1000),
                         static_cast<long long>(rel(Target)), static_cast<long long>(rel(Paced)),
                         static_cast<long long>(rel(Awake)), static_cast<long long>(rel(Submit)),
                         static_cast<long long>(rel(Gpu)), static_cast<long long>(rel(Take)),
                         static_cast<long long>(rel(PresentStart)),
                         static_cast<long long>(rel(PresentEnd)),
                         static_cast<long long>(Libraries::VideoOut::FrameCapPeriodNs() / 1000));
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
                            " | even: offset %.2f, held %llu by %.2f, late %llu by %.2f",
                            double(Pace::offset_ns.load()) / 1e6, static_cast<unsigned long long>(h),
                            double(Pace::held_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, h)),
                            static_cast<unsigned long long>(l),
                            double(Pace::late_ns.exchange(0)) / 1e6 / double(std::max<u64>(1, l)));
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

void HoldPresent(s64 flip_arg) {
    if (Pace::even && flip_arg >= 0) {
        Pace::Hold(static_cast<u64>(flip_arg) + 1);
    }
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
        // PC Latency Stats: the same markers as ETW events (NVIDIA GPUs only, as the Reflex SDK's licence).
        const char* pcl = std::getenv("BB_PCLSTATS");
        if (pcl && pcl[0] == '0') {
            std::printf("Low latency: PCLStats off (BB_PCLSTATS=0)\n");
        } else {
            const auto init = reinterpret_cast<BbPclStatsInitFn>(DlssBridgeExport("BbPclStatsInit"));
            const auto marker =
                reinterpret_cast<BbPclStatsMarkerFn>(DlssBridgeExport("BbPclStatsMarker"));
            if (init && marker && init()) {
                pcl_marker = marker;
                std::printf("Low latency: PCLStats markers on (PC latency for PresentMon, FrameView "
                            "and the NVIDIA overlay)\n");
            } else {
                std::printf("Low latency: no PCLStats (%s)\n",
                            init && marker ? "the bridge could not start it"
                                           : "bbport_dlss.dll missing or without it");
            }
        }
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
    BbHitchMark('S');
    BbTrace::Record(BbTrace::GameFrameStart, frame);
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
