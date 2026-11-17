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

#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "../../../dlss_bridge/bbport_dlss_bridge.h"

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

u32 MinimumIntervalUs() {
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
        .maxFPS = sleep_on ? EmulatorSettings.GetFrameLimit() : 0,
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

} // namespace

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
    if (kind == Kind::AntiLag) {
        AntiLag(VK_ANTI_LAG_STAGE_INPUT_AMD, id); // the driver sleeps here when it is on
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
