// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FPS counter's live VRR indicator (bbport_vrr.h).
#include "bbport_vrr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" int bbgpu_window_foreground(void);

namespace BbVrr {
namespace {

using Clock = std::chrono::steady_clock;

const bool enabled = [] {
    const char* env = std::getenv("BB_VRR");
    return !(env && env[0] == '0');
}();
const bool log_raw = [] {
    const char* env = std::getenv("BB_VRR_LOG");
    return env && env[0] == '1';
}();
const bool timing_forced = [] {
    const char* env = std::getenv("BB_VRR_TIMING");
    return env && env[0] == '1';
}();

std::atomic<void*> window{nullptr};
u32 vendor = 0; ///< the Vulkan device's PCI vendor (0x10de NVIDIA, 0x1002 AMD)

/// Raw state of a source: -2 not certain (the indicator hides), -1 not known, 0 off, 1 on.
struct Raw {
    int state = -1;
    std::string why;
};

void Log(const char* fmt, auto... args) {
    std::printf(fmt, args...);
    std::fflush(stdout);
}

// ---- The window's monitor: Windows' adapter, target and source (QueryDisplayConfig) -----------
#ifdef _WIN32
struct Monitor {
    HMONITOR handle = nullptr;
    LUID adapter{};
    u32 target = 0;      ///< the adapter-relative target id
    u32 source = 0;      ///< Windows' source id (the n-1 of \\.\DISPLAYn is not used: names move)
    double refresh = 0;  ///< Hz
    std::string name;    ///< the monitor's own name (EDID), for the log
    bool valid = false;
};

Monitor FindMonitor(HWND hwnd) {
    Monitor m;
    m.handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!m.handle || !GetMonitorInfoW(m.handle, &info)) {
        return m;
    }
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return m;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return m;
    }
    for (u32 i = 0; i < path_count; ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) {
            continue;
        }
        m.adapter = path.targetInfo.adapterId;
        m.target = path.targetInfo.id;
        m.source = path.sourceInfo.id;
        const auto& rate = path.targetInfo.refreshRate;
        m.refresh = rate.Denominator ? double(rate.Numerator) / rate.Denominator : 0.0;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, target.monitorFriendlyDeviceName, -1, name, sizeof(name) - 1,
                                nullptr, nullptr);
            m.name = name;
        }
        if (m.name.empty()) {
            m.name = "display";
        }
        m.valid = true;
        break;
    }
    return m;
}

// ---- NVIDIA: NVAPI (nvapi64.dll of the driver; the entry points by their published ids) -----------
namespace Nv {

constexpr u32 Ver(u32 size, u32 version) {
    return size | (version << 16);
}

struct DisplayIds { // NV_GPU_DISPLAYIDS (version 3)
    u32 version, connector_type, display_id, flags;
};
struct DisplayIdInfo { // NV_DISPLAY_ID_INFO_DATA_V1
    u32 version;
    LUID adapter;
    u32 target;
    u32 reserved[4];
};
struct VrrInfo { // NV_GET_VRR_INFO_V1
    u32 version;
    u32 bits; ///< 0 bIsVRREnabled (active now), 1 possible, 2 requested, 3 indicator, 4 display in VRR mode
    u32 reserved[4];
};
struct PathTarget { // NV_DISPLAYCONFIG_PATH_TARGET_INFO_V2
    u32 display_id;
    void* details;
    u32 target_id;
};
struct SourceMode { // NV_DISPLAYCONFIG_SOURCE_MODE_INFO_V1
    u32 width, height, depth, format;
    s32 x, y;
    u32 spanning, bits;
};
struct PathInfo { // NV_DISPLAYCONFIG_PATH_INFO_V2
    u32 version;
    u32 source_id;
    u32 target_count;
    PathTarget* targets;
    SourceMode* source_mode;
    u32 bits;
    void* os_adapter;
};
static_assert(sizeof(DisplayIds) == 16 && sizeof(DisplayIdInfo) == 32 && sizeof(VrrInfo) == 24);
static_assert(sizeof(PathTarget) == 24 && sizeof(PathInfo) == 48);

using QueryInterface = void*(__cdecl*)(u32);
using Initialize = int(__cdecl*)();
using EnumPhysicalGpus = int(__cdecl*)(void** gpus, u32* count);
using GetConnectedDisplayIds = int(__cdecl*)(void* gpu, DisplayIds* ids, u32* count, u32 flags);
using GetDisplayIdInfo = int(__cdecl*)(u32 display_id, DisplayIdInfo* info);
using GetDisplayConfig = int(__cdecl*)(u32* count, PathInfo* paths);
using GetVrrInfo = int(__cdecl*)(u32 display_id, VrrInfo* info);

bool loaded = false;
EnumPhysicalGpus enum_gpus{};
GetConnectedDisplayIds connected_ids{};
GetDisplayIdInfo id_info{};
GetDisplayConfig display_config{};
GetVrrInfo vrr_info{};

bool Load() {
    static const bool ok = [] {
        HMODULE dll = LoadLibraryW(L"nvapi64.dll");
        if (!dll) {
            return false;
        }
        const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(dll, "nvapi_QueryInterface"));
        if (!query) {
            return false;
        }
        const auto init = reinterpret_cast<Initialize>(query(0x0150e828));
        enum_gpus = reinterpret_cast<EnumPhysicalGpus>(query(0xe5ac921f));
        connected_ids = reinterpret_cast<GetConnectedDisplayIds>(query(0x0078dba2));
        id_info = reinterpret_cast<GetDisplayIdInfo>(query(0xbae8aa5e));
        display_config = reinterpret_cast<GetDisplayConfig>(query(0x11abccf8));
        vrr_info = reinterpret_cast<GetVrrInfo>(query(0xdf8fda57));
        if (!init || !enum_gpus || !connected_ids || !id_info || !vrr_info) {
            Log("VRR: NVAPI without the functions needed (driver too old?)\n");
            return false;
        }
        const int status = init();
        if (status != 0) {
            Log("VRR: NVAPI did not start (%d)\n", status);
            return false;
        }
        return true;
    }();
    loaded = ok;
    return ok;
}

/// NVAPI's display id of the monitor (by adapter and target), 0: none, or more than one (a lookup
/// failed or the match is not unique).
u32 DisplayIdOf(const Monitor& m) {
    u32 found = 0, matches = 0;
    void* gpus[64]{};
    u32 gpu_count = 0;
    if (enum_gpus(gpus, &gpu_count) != 0) {
        return 0;
    }
    for (u32 g = 0; g < gpu_count && g < 64; ++g) {
        u32 count = 0;
        if (connected_ids(gpus[g], nullptr, &count, 0) != 0 || count == 0) {
            continue;
        }
        std::vector<DisplayIds> ids(count);
        for (DisplayIds& id : ids) {
            id.version = Ver(sizeof(DisplayIds), 3);
        }
        if (connected_ids(gpus[g], ids.data(), &count, 0) != 0) {
            continue;
        }
        for (u32 i = 0; i < count && i < ids.size(); ++i) {
            DisplayIdInfo info{};
            info.version = Ver(sizeof(info), 1);
            if (id_info(ids[i].display_id, &info) == 0 && info.target == m.target &&
                info.adapter.LowPart == m.adapter.LowPart && info.adapter.HighPart == m.adapter.HighPart) {
                found = ids[i].display_id;
                ++matches;
            }
        }
    }
    return matches == 1 ? found : 0;
}

/// NVAPI's own source number of each display id (NvAPI_DISP_GetDisplayConfig).
std::vector<std::pair<u32, u32>> SourcesOfDisplays() {
    std::vector<std::pair<u32, u32>> out; // (display id, NVAPI source)
    if (!display_config) {
        return out;
    }
    u32 count = 0;
    if (display_config(&count, nullptr) != 0 || count == 0 || count > 16) {
        return out;
    }
    std::vector<PathInfo> paths(count);
    for (PathInfo& p : paths) {
        p = {};
        p.version = Ver(sizeof(PathInfo), 2);
    }
    if (display_config(&count, paths.data()) != 0) {
        return out;
    }
    std::vector<std::vector<PathTarget>> targets(count);
    std::vector<SourceMode> source_modes(count);
    for (u32 i = 0; i < count; ++i) {
        targets[i].resize(std::max(paths[i].target_count, 1u));
        paths[i].targets = targets[i].data();
        paths[i].source_mode = &source_modes[i];
    }
    if (display_config(&count, paths.data()) != 0) {
        return out;
    }
    for (u32 i = 0; i < count; ++i) {
        for (u32 t = 0; t < paths[i].target_count && t < targets[i].size(); ++t) {
            out.emplace_back(targets[i][t].display_id, paths[i].source_id);
        }
    }
    return out;
}

} // namespace Nv

/// The NVIDIA source: the display id whose VRR state is read, found again when the window moves to
/// another monitor or the display layout changes.
struct NvSource {
    HMONITOR monitor = nullptr;
    u32 query_id = 0;
    Clock::time_point checked{};
    std::string name;
    std::string hidden; ///< why the display is not certain (query_id 0)

    Raw Sample(HWND hwnd) {
        const auto now = Clock::now();
        const HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (current != monitor || query_id == 0 || now - checked > std::chrono::seconds(10)) {
            Resolve(hwnd);
            checked = now;
        }
        if (query_id == 0) {
            return {-2, hidden};
        }
        Nv::VrrInfo info{};
        info.version = Nv::Ver(sizeof(info), 1);
        const int status = Nv::vrr_info(query_id, &info);
        if (status != 0) {
            return {-2, "NvAPI_Disp_GetVRRInfo failed (" + std::to_string(status) + ")"};
        }
        char why[96];
        std::snprintf(why, sizeof(why), "NVAPI %s: active %u possible %u requested %u", name.c_str(),
                      info.bits & 1, (info.bits >> 1) & 1, (info.bits >> 2) & 1);
        return {int(info.bits & 1), why};
    }

    void Resolve(HWND hwnd) {
        const Monitor m = FindMonitor(hwnd);
        monitor = m.handle;
        const u32 old = query_id;
        query_id = 0;
        if (!m.valid) {
            hidden = "the window's monitor is not in Windows' display paths";
            return;
        }
        const u32 id = Nv::DisplayIdOf(m);
        if (!id) {
            hidden = "no single NVIDIA display matches " + m.name;
            return;
        }
        // The driver keeps the VRR state per Windows source, and NvAPI_Disp_GetVRRInfo finds the
        // source from the display id by NVAPI's own source number, which need not be Windows' (seen
        // on driver 617.14 with two monitors: NVAPI numbered the primary 0, Windows 1, and the
        // game's VRR state came back for the other display id, following the game's focus there).
        // So the state of this monitor's Windows source comes from the display id NVAPI numbers
        // with that source; with both numberings the same that is the monitor's own id.
        // Only when this is certain: the monitor's id once in NVAPI's layout, one display on the
        // monitor's Windows source number, and with the numberings different at most two displays
        // (where the crossing was seen); else the indicator hides.
        u32 query = 0, nv_source = ~0u, own = 0, on_source = 0;
        const auto sources = Nv::SourcesOfDisplays();
        for (const auto& [display, source] : sources) {
            if (display == id) {
                nv_source = source;
                ++own;
            }
            if (source == m.source) {
                query = display;
                ++on_source;
            }
        }
        if (own != 1 || on_source != 1) {
            hidden = "NVAPI's display layout does not place " + m.name + " on one source";
            return;
        }
        if (nv_source == m.source) {
            query = id;
        } else if (sources.size() > 2) {
            hidden = "Windows and NVAPI number the sources differently, with more than two displays";
            return;
        }
        query_id = query;
        name = m.name;
        if (query != old) {
            if (query == id) {
                Log("VRR: NVAPI %s, display 0x%x (Windows source %u)\n", m.name.c_str(), id, m.source);
            } else {
                Log("VRR: NVAPI %s, display 0x%x: Windows source %u is NVAPI's source %u, so its VRR "
                    "state is read through display 0x%x (NVAPI's source %u)\n",
                    m.name.c_str(), id, m.source, nv_source, query, m.source);
            }
        }
    }
};
#endif // _WIN32

// ---- Vendor-neutral: VK_EXT_present_timing -----------------------------------------------------
namespace Timing {

bool device_on = false;   ///< extensions and features enabled
bool swapchain_on = false; ///< this swapchain measures (present thread / creation)
VkDevice device = VK_NULL_HANDLE;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
PFN_vkSetSwapchainPresentTimingQueueSizeEXT set_queue_size{};
PFN_vkGetSwapchainTimingPropertiesEXT get_properties{};
PFN_vkGetPastPresentationTimingEXT get_past{};

VkPhysicalDevicePresentId2FeaturesKHR id2_features{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
VkPhysicalDevicePresentTimingFeaturesEXT timing_features{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};

constexpr u32 QueueSize = 64;

// Present thread only.
u64 present_id = 0;
u64 last_id = 0, last_time = 0;
u64 properties_counter = ~0ull;
u64 refresh_ns = 0, interval_ns = 0;
std::deque<std::pair<Clock::time_point, double>> deltas; ///< display intervals (ns), last second
Clock::time_point last_sample{};
uint64_t ids[1];
VkPresentTimingInfoEXT timing_info{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
VkPresentTimingsInfoEXT timings_info{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
VkPresentId2KHR id2_info{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};

/// The latest verdict (written by the present thread, read by the poll thread).
std::mutex verdict_mutex;
Raw verdict;
Clock::time_point verdict_time{};
std::atomic<bool> wanted{false}; ///< the indicator is shown: measure

void Drain() {
    VkPastPresentationTimingEXT timings[QueueSize];
    VkPresentStageTimeEXT stages[QueueSize];
    for (u32 i = 0; i < QueueSize; ++i) {
        timings[i] = {.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,
                      .presentStageCount = 1,
                      .pPresentStages = &stages[i]};
    }
    VkPastPresentationTimingInfoEXT info{.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
                                         .swapchain = swapchain};
    VkPastPresentationTimingPropertiesEXT props{
        .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
        .presentationTimingCount = QueueSize,
        .pPresentationTimings = timings};
    const VkResult result = get_past(device, &info, &props);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return;
    }
    if (props.timingPropertiesCounter != properties_counter) {
        VkSwapchainTimingPropertiesEXT timing{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
        u64 counter = 0;
        if (get_properties(device, swapchain, &timing, &counter) == VK_SUCCESS) {
            properties_counter = counter;
            if (timing.refreshDuration != refresh_ns || timing.refreshInterval != interval_ns) {
                refresh_ns = timing.refreshDuration;
                interval_ns = timing.refreshInterval;
                Log("VRR: present timing: refresh %.3f ms, interval %s\n", refresh_ns / 1e6,
                    interval_ns == UINT64_MAX ? "variable (VRR)"
                    : interval_ns == 0        ? "not known"
                                              : (interval_ns == refresh_ns ? "fixed" : "a multiple"));
            }
        }
    }
    const auto now = Clock::now();
    for (u32 i = 0; i < props.presentationTimingCount; ++i) {
        const VkPastPresentationTimingEXT& t = timings[i];
        if (!t.reportComplete || t.presentStageCount == 0 ||
            !(t.pPresentStages[0].stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) ||
            t.pPresentStages[0].time == 0) {
            continue;
        }
        const u64 time = t.pPresentStages[0].time;
        if (t.presentId == last_id + 1 && time > last_time) {
            deltas.emplace_back(now, double(time - last_time));
        }
        last_id = t.presentId;
        last_time = time;
    }
    while (!deltas.empty() && now - deltas.front().first > std::chrono::seconds(1)) {
        deltas.pop_front();
    }
}

/// Whether the frames reach the screen on the refresh grid: composed by the desktop or a fixed
/// refresh (on), or when they are ready (VRR, or a tear-through flip).
Raw Judge() {
    if (properties_counter == ~0ull) {
        return {-1, "present timing: no report yet"};
    }
    if (refresh_ns == 0 || (interval_ns != refresh_ns && interval_ns != UINT64_MAX)) {
        return {-2, "the presentation engine does not report VRR or fixed refresh (present timing)"};
    }
    if (interval_ns == refresh_ns) {
        return {0, "present timing: fixed refresh"};
    }
    if (deltas.size() < 6) {
        return {-1, "present timing: too few frames"};
    }
    const double period = double(refresh_ns);
    u32 on_grid = 0, at_limit = 0;
    for (const auto& [when, d] : deltas) {
        const double k = std::round(d / period);
        if (k >= 1.0 && std::fabs(d - k * period) < 300'000.0) {
            ++on_grid;
            at_limit += k == 1.0;
        }
    }
    const double n = double(deltas.size());
    char why[96];
    std::snprintf(why, sizeof(why), "present timing: %s, %.0f%% on the refresh grid (%zu frames)",
                  "VRR mode", 100.0 * on_grid / n, deltas.size());
    if (at_limit >= 0.8 * n) {
        return {-1, std::string(why) + ", at the refresh limit"}; // VRR and fixed look the same
    }
    if (on_grid >= 0.85 * n) {
        return {0, why}; // composed or a fixed refresh
    }
    if (on_grid <= 0.5 * n) {
        return {1, why}; // off the grid in VRR mode: flipped when ready
    }
    return {-1, why};
}

} // namespace Timing

// ---- The state shown, with hysteresis ------------------------------------------------------------
std::mutex shown_mutex;
bool shown_known = false;
bool shown_active = false;
const char* label = nullptr;
std::atomic<bool> poll_started{false};
std::atomic<int64_t> last_wanted_ms{0};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void PollThread() {
#ifdef _WIN32
    NvSource nv;
    const bool use_nv = vendor == 0x10de && Nv::Load();
#else
    const bool use_nv = false;
#endif
    if (!use_nv && !Timing::swapchain_on && !Timing::device_on) {
        Log("VRR indicator hidden: no source on this GPU/driver (NVAPI or VK_EXT_present_timing)\n");
        std::scoped_lock lk{shown_mutex};
        label = nullptr;
        return;
    }
    Log("VRR: indicator source %s%s\n", use_nv ? "NVAPI" : "present timing",
        use_nv && Timing::device_on ? " (present timing logged beside it)" : "");
    int candidate = -2;
    Clock::time_point candidate_since{};
    std::string last_raw;
    std::string hidden_logged;
    int shown = -2;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (NowMs() - last_wanted_ms.load(std::memory_order_relaxed) > 2000) {
            Timing::wanted = false;
            continue; // the counter is hidden: nothing queried
        }
        Timing::wanted = true;
        Raw raw;
        const bool front = bbgpu_window_foreground() != 0;
        Raw timing;
        {
            std::scoped_lock lk{Timing::verdict_mutex};
            timing = Timing::verdict;
            if (Clock::now() - Timing::verdict_time > std::chrono::seconds(1)) {
                timing = {-1, "present timing: no presents"};
            }
        }
#ifdef _WIN32
        void* hwnd = window.load();
        if (use_nv) {
            raw = hwnd ? nv.Sample(static_cast<HWND>(hwnd)) : Raw{-1, "no window yet"};
        } else
#endif
        {
            raw = Timing::swapchain_on ? timing
                                       : Raw{-2, "present timing is not offered for the game's window"};
        }
        // Not certain: no indicator at all (a diagnostic that may be wrong is worse than none).
        if (raw.state == -2) {
            if (raw.why != hidden_logged) {
                Log("VRR indicator hidden: %s\n", raw.why.c_str());
                hidden_logged = raw.why;
            }
            candidate = shown = -2;
            std::scoped_lock lk{shown_mutex};
            shown_known = false;
            continue;
        }
        hidden_logged.clear();
        // The driver's or the presentation engine's own answer, also with the game behind other
        // windows (NVIDIA's indicator goes to "Normal" then, and so does NVAPI's state).
        if (!front) {
            raw.why += ", game not in front";
        }
        if (log_raw) {
            std::string line = raw.why;
            if (use_nv && Timing::device_on) {
                line += " | " + timing.why;
            }
            if (line != last_raw) {
                Log("VRR raw: %s\n", line.c_str());
                last_raw = line;
            }
        }
        const auto now = Clock::now();
        if (raw.state != candidate) {
            candidate = raw.state;
            candidate_since = now;
        }
        // Half a second steady before the indicator follows (not known: keep what is shown).
        if (candidate != shown && candidate != -1 && now - candidate_since >= std::chrono::milliseconds(500)) {
            Log("VRR: %s -> %s (%s)\n", shown == 1 ? "active" : shown == 0 ? "off" : "hidden",
                candidate == 1 ? "active" : "off", raw.why.c_str());
            shown = candidate;
            std::scoped_lock lk{shown_mutex};
            shown_known = true;
            shown_active = shown == 1;
        }
    }
}

} // namespace

void SetWindow(void* hwnd) {
    window = hwnd;
}

void* PrepareDevice(vk::PhysicalDevice physical, const std::vector<std::string>& available,
                    std::vector<const char*>& extensions, bool reflex_present_id) {
    if (!enabled) {
        return nullptr;
    }
    vendor = physical.getProperties().vendorID;
    label = vendor == 0x10de ? "G-SYNC" : vendor == 0x1002 ? "FreeSync" : "VRR";
    // NVIDIA reads NVAPI; the timing source there only on request (BB_VRR_TIMING=1), and never
    // beside low latency mode's present ids.
    if ((vendor == 0x10de && !timing_forced) || reflex_present_id) {
        return nullptr;
    }
    const auto has = [&](const char* name) {
        return std::find(available.begin(), available.end(), name) != available.end();
    };
    if (!has(VK_EXT_PRESENT_TIMING_EXTENSION_NAME) || !has(VK_KHR_PRESENT_ID_2_EXTENSION_NAME)) {
        return nullptr;
    }
    const char* calibrated = has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                                 ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                             : has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                                 ? VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                                 : nullptr;
    if (!calibrated) {
        return nullptr;
    }
    VkPhysicalDevicePresentTimingFeaturesEXT timing_query{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR id2_query{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .pNext = &timing_query};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &id2_query};
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!id2_query.presentId2 || !timing_query.presentTiming) {
        return nullptr;
    }
    extensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    extensions.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    extensions.push_back(calibrated);
    Timing::id2_features.presentId2 = VK_TRUE;
    Timing::timing_features.presentTiming = VK_TRUE;
    Timing::id2_features.pNext = &Timing::timing_features;
    Timing::timing_features.pNext = nullptr;
    Timing::device_on = true;
    return &Timing::id2_features;
}

void OnDevice(vk::Device dev) {
    if (!Timing::device_on) {
        return;
    }
    Timing::device = dev;
    const auto load = [&](const char* name) {
        return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(dev, name);
    };
    Timing::set_queue_size =
        reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(load("vkSetSwapchainPresentTimingQueueSizeEXT"));
    Timing::get_properties =
        reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(load("vkGetSwapchainTimingPropertiesEXT"));
    Timing::get_past = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(load("vkGetPastPresentationTimingEXT"));
    if (!Timing::set_queue_size || !Timing::get_properties || !Timing::get_past) {
        Timing::device_on = false;
    }
}

vk::SwapchainCreateFlagsKHR SwapchainFlags(vk::PhysicalDevice physical, vk::SurfaceKHR surface) {
    Timing::swapchain_on = false;
    if (!Timing::device_on || !VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceCapabilities2KHR) {
        return {};
    }
    VkPresentTimingSurfaceCapabilitiesEXT timing{
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
    VkSurfaceCapabilitiesPresentId2KHR id2{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
                                           .pNext = &timing};
    VkSurfaceCapabilities2KHR caps{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &id2};
    const VkPhysicalDeviceSurfaceInfo2KHR info{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                               .surface = surface};
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical, &info, &caps) !=
            VK_SUCCESS ||
        !id2.presentId2Supported || !timing.presentTimingSupported ||
        !(timing.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)) {
        return {};
    }
    Timing::swapchain_on = true;
    return vk::SwapchainCreateFlagBitsKHR::ePresentTimingEXT | vk::SwapchainCreateFlagBitsKHR::ePresentId2;
}

void OnSwapchain(vk::SwapchainKHR chain) {
    if (!Timing::swapchain_on) {
        return;
    }
    Timing::swapchain = chain;
    if (Timing::set_queue_size(Timing::device, chain, Timing::QueueSize) != VK_SUCCESS) {
        Timing::swapchain_on = false;
        return;
    }
    // A new swapchain starts its own ids and reports.
    Timing::present_id = 0;
    Timing::last_id = 0;
    Timing::last_time = 0;
    Timing::properties_counter = ~0ull;
    Timing::deltas.clear();
}

void OnSwapchainDestroy() {
    Timing::swapchain = VK_NULL_HANDLE;
}

const void* PresentNext(const void* next) {
    if (!Timing::swapchain_on || !Timing::swapchain || !Timing::wanted.load(std::memory_order_relaxed)) {
        return next;
    }
    // Reports come back in order: collect them before each present so the queue never fills.
    Timing::Drain();
    const auto now = Clock::now();
    if (now - Timing::last_sample >= std::chrono::milliseconds(250)) {
        Timing::last_sample = now;
        Raw verdict = Timing::Judge();
        std::scoped_lock lk{Timing::verdict_mutex};
        Timing::verdict = std::move(verdict);
        Timing::verdict_time = now;
    }
    Timing::ids[0] = ++Timing::present_id;
    Timing::timing_info.presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    Timing::timings_info.swapchainCount = 1;
    Timing::timings_info.pTimingInfos = &Timing::timing_info;
    Timing::timings_info.pNext = next;
    Timing::id2_info.swapchainCount = 1;
    Timing::id2_info.pPresentIds = Timing::ids;
    Timing::id2_info.pNext = &Timing::timings_info;
    return &Timing::id2_info;
}

Shown Get() {
    if (!enabled) {
        return {};
    }
    last_wanted_ms.store(NowMs(), std::memory_order_relaxed);
    if (!poll_started.exchange(true)) {
        std::thread(PollThread).detach();
    }
    std::scoped_lock lk{shown_mutex};
    if (!label || !shown_known) {
        return {};
    }
    return {label, shown_active};
}

} // namespace BbVrr
