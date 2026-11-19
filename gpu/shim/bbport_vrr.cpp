// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FPS counter's live VRR indicator (bbport_vrr.h).
#include "bbport_vrr.h"
#include "bbport_settings.h"

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
/// BB_VRR_INDICATOR=1: the indicator starts on whatever bbport.ini says (test runs); the in-game
/// menu can still turn it off (and on) live.
const bool indicator_forced = [] {
    const char* env = std::getenv("BB_VRR_INDICATOR");
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
    POINTL position{};   ///< the source's place on the desktop (physical pixels, any DPI awareness)
    u32 width = 0, height = 0; ///< the source's size (0: not known)
    RECT rect{};         ///< the monitor's rectangle (GetMonitorInfo, in the caller's DPI context)
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
        m.rect = info.rcMonitor;
        const u32 mode = path.sourceInfo.modeInfoIdx;
        if (mode != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && mode < mode_count &&
            modes[mode].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            m.position = modes[mode].sourceMode.position;
            m.width = modes[mode].sourceMode.width;
            m.height = modes[mode].sourceMode.height;
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

// ---- AMD: ADLX (amdadlx64.dll of Adrenalin) and ADL (atiadlxx.dll) ----------------------------
// ADLX has no live VRR state like NVAPI's: IADLXDisplayFreeSync only tells whether FreeSync is
// supported and switched on for a display (its change events say no more). So the state shown is
// derived: FreeSync on for the window's display, the window in front and covering that display
// (the port's fullscreen is borderless), and the frames on screen inside the display's FreeSync
// range (ADL2_Display_FreeSync_Cap; the current refresh rate caps it). ADL also tells whether the
// display mode in use and a borderless window can use FreeSync.
// The interfaces are called through their vtables, at the slots of the C declarations in the
// GPUOpen ADLX SDK headers (MIT; SDK/Include/ISystem.h, IDesktops.h, IDisplays.h,
// IDisplaySettings.h, ADLX.h): no SDK header or library is needed to build.
namespace Amd {

using Result = int; // ADLX_RESULT: 0 ADLX_OK

/// The method at `slot` of an ADLX interface (__stdcall, the ADLX_STD_CALL of the headers).
template <typename R, typename... A>
R Call(void* object, u32 slot, A... args) {
    using Method = R(__stdcall*)(void*, A...);
    const auto vtable = *static_cast<void* const* const*>(object);
    return reinterpret_cast<Method>(vtable[slot])(object, args...);
}

// Slots (C vtables of the ADLX SDK 2.0 headers; IADLXInterface: Acquire 0, Release 1, QueryInterface 2).
constexpr u32 Release = 1;
constexpr u32 SystemGetDisplaysServices = 3, SystemGetDesktopsServices = 4; // IADLXSystem (no IADLXInterface)
constexpr u32 DesktopServicesGetDesktops = 4;
constexpr u32 ListSize = 3, ListAtTyped = 11; // IADLXDesktopList::At_DesktopList, IADLXDisplayList::At_DisplayList
constexpr u32 DesktopSize = 4, DesktopTopLeft = 5, DesktopType = 6, DesktopGetDisplays = 8;
constexpr u32 DisplayName = 6;
constexpr u32 DisplayServicesGetFreeSync = 9;
constexpr u32 FreeSyncIsSupported = 3, FreeSyncIsEnabled = 4;
constexpr u32 MappingAdlIdsFromDisplay = 5, MappingAdlIdsFromDesktop = 7; // IADLMapping (no IADLXInterface)
constexpr int DesktopSingle = 0; // ADLX_DESKTOP_TYPE DESKTOP_SINGLE

struct Point { // ADLX_Point
    int x, y;
};

using QueryFullVersion = Result(__cdecl*)(u64* version);
using Initialize = Result(__cdecl*)(u64 version, void** system);
using Initialize2 = Result(__cdecl*)(u64 version, void** system, void** mapping);

void* adlx_system = nullptr; ///< IADLXSystem
void* mapping = nullptr; ///< IADLMapping (ADLXInitialize2; nullptr with older ADLX: no FreeSync range)

void ReleaseObject(void*& object) {
    if (object) {
        Call<long>(object, Release);
        object = nullptr;
    }
}

bool Load() {
    static const bool ok = [] {
        HMODULE dll = LoadLibraryW(L"amdadlx64.dll");
        if (!dll) {
            Log("VRR: ADLX not found (amdadlx64.dll)\n");
            return false;
        }
        const auto full_version =
            reinterpret_cast<QueryFullVersion>(GetProcAddress(dll, "ADLXQueryFullVersion"));
        const auto init2 = reinterpret_cast<Initialize2>(GetProcAddress(dll, "ADLXInitialize2"));
        const auto init = reinterpret_cast<Initialize>(GetProcAddress(dll, "ADLXInitialize"));
        u64 version = 0;
        if (!full_version || full_version(&version) != 0 || (!init2 && !init)) {
            Log("VRR: ADLX without the functions needed\n");
            return false;
        }
        // The interfaces used are in every ADLX release: the driver's own version is accepted.
        const Result result = init2 ? init2(version, &adlx_system, &mapping) : init(version, &adlx_system);
        if (result != 0 || !adlx_system) {
            Log("VRR: ADLX did not start (%d)\n", result);
            adlx_system = mapping = nullptr;
            return false;
        }
        Log("VRR: ADLX %u.%u.%u.%u%s\n", u32(version >> 48), u32(version >> 32) & 0xffff,
            u32(version >> 16) & 0xffff, u32(version) & 0xffff,
            mapping ? "" : " (no ADL mapping: the FreeSync range is not known)");
        return true;
    }();
    return ok;
}

// ADL: the display's FreeSync range and abilities.
namespace Adl {
struct FreeSyncCap { // ADLFreeSyncCap
    int caps;
    int min_uhz, max_uhz;
    unsigned char label;
    char reserved[3];
    int reserved2[4];
};
static_assert(sizeof(FreeSyncCap) == 32);
// ADL's ability flags are logged only: on an RX 7800 XT (Adrenalin 26.11.1) ADL never set "borderless
// window" (0x40) although FreeSync ran in the port's borderless fullscreen (PresentMon: display
// intervals spread continuously), so they decide nothing.
constexpr int CapSupported = 1 << 0;

using Malloc = void*(__stdcall*)(int size);
using Create = int(__cdecl*)(Malloc callback, int connected_only, void** context);
using GetCap = int(__cdecl*)(void* context, int adapter, int display, FreeSyncCap* cap);

void* context = nullptr;
GetCap get_cap{};

void* __stdcall Allocate(int size) {
    return std::malloc(size_t(size));
}

bool Load() {
    static const bool ok = [] {
        HMODULE dll = LoadLibraryW(L"atiadlxx.dll");
        if (!dll) {
            return false;
        }
        const auto create = reinterpret_cast<Create>(GetProcAddress(dll, "ADL2_Main_Control_Create"));
        get_cap = reinterpret_cast<GetCap>(GetProcAddress(dll, "ADL2_Display_FreeSync_Cap"));
        return create && get_cap && create(Allocate, 1, &context) == 0 && context;
    }();
    return ok;
}
} // namespace Adl

} // namespace Amd

/// The AMD source: the ADLX display of the window's monitor (the single-display desktop at the
/// monitor's place and size, and on its Windows source when ADLX maps it), found again when the
/// window moves to another monitor or the display layout changes.
struct AmdSource {
    HMONITOR monitor = nullptr;
    void* freesync = nullptr; ///< IADLXDisplayFreeSync of the display
    Monitor mon;
    Clock::time_point checked{};
    std::string hidden; ///< why the display is not certain (freesync nullptr)
    bool range_known = false;
    double range_min = 0, range_max = 0; ///< Hz (ADL)
    std::string logged;                  ///< the last display line logged
    Clock::time_point refreshed{};       ///< the current mode's refresh rate last read
    bool in_range = false;               ///< the frames inside the range (with the margins below)
    bool faster_logged = false;

    /// `fifo`: the swapchain presents with FIFO or FIFO Relaxed (never faster than the display).
    Raw Sample(HWND hwnd, bool front, double fps, bool fifo) {
        const auto now = Clock::now();
        const HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (current != monitor || !freesync || now - checked > std::chrono::seconds(10)) {
            Resolve(hwnd);
            checked = refreshed = now;
        } else if (now - refreshed >= std::chrono::seconds(2)) {
            // The current mode's refresh rate, read again (a mode change in play; the rate at the
            // start need not be the one the game runs at).
            refreshed = now;
            const Monitor again = FindMonitor(hwnd);
            if (!again.valid || again.source != mon.source || again.position.x != mon.position.x ||
                again.position.y != mon.position.y || again.width != mon.width || again.height != mon.height) {
                checked = {}; // the layout changed: the display is found again next time
            } else if (std::fabs(again.refresh - mon.refresh) > 0.5) {
                Log("VRR: ADLX %s: refresh rate %.2f -> %.2f Hz\n", mon.name.c_str(), mon.refresh, again.refresh);
                mon.refresh = again.refresh;
            }
        }
        if (!freesync) {
            return {-2, hidden};
        }
        bool supported = false, on = false;
        const Amd::Result a = Amd::Call<Amd::Result>(freesync, Amd::FreeSyncIsSupported, &supported);
        const Amd::Result b = Amd::Call<Amd::Result>(freesync, Amd::FreeSyncIsEnabled, &on);
        if (a != 0 || b != 0) {
            checked = {}; // the display may have changed: found again next time
            return {-2, "ADLX FreeSync state not read (" + std::to_string(a) + ", " + std::to_string(b) + ")"};
        }
        std::string why = "ADLX " + mon.name;
        if (!supported) {
            return {0, why + ": FreeSync not supported by the display"};
        }
        if (!on) {
            return {0, why + ": FreeSync off in AMD Software"};
        }
        // The window: in front, not minimised, covering the whole monitor (borderless fullscreen).
        RECT rect{};
        if (IsIconic(hwnd) || !GetWindowRect(hwnd, &rect)) {
            return {0, why + ": FreeSync on, the window is minimised"};
        }
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(current, &info)) {
            return {-2, "the window's monitor is not known"};
        }
        const RECT& m = info.rcMonitor;
        if (rect.left != m.left || rect.top != m.top || rect.right != m.right || rect.bottom != m.bottom) {
            return {0, why + ": FreeSync on, the game is in a window (not fullscreen)"};
        }
        if (!front) {
            return {0, why + ": FreeSync on, the game not in front"};
        }
        // The frames on screen inside the range (the current refresh rate caps it).
        if (fps <= 0.0) {
            return {-1, why + ": no frame rate yet"};
        }
        // The top: the current mode's refresh rate, capped by the FreeSync range's maximum. FIFO
        // never presents faster than the display refreshes, so FIFO frames above the mode's rate
        // show the display running faster: then the range's maximum is the top.
        double top = mon.refresh > 0 && (!range_known || mon.refresh < range_max) ? mon.refresh : range_max;
        if (fifo && range_known && top < range_max && fps > top * 1.015) {
            if (!faster_logged) {
                Log("VRR: ADLX %s: FIFO at %.1f FPS above the mode's %.2f Hz: the top is the range's %.0f Hz\n",
                    mon.name.c_str(), fps, mon.refresh, range_max);
                faster_logged = true;
            }
            top = range_max;
        }
        char text[160];
        if (range_known) {
            std::snprintf(text, sizeof(text), ": FreeSync on, %.0f FPS, range %.0f-%.0f Hz", fps, range_min, top);
        } else {
            std::snprintf(text, sizeof(text), ": FreeSync on, %.0f FPS, up to %.0f Hz (lower end not known)", fps,
                          top);
        }
        why += text;
        // Margins at the edges: in range from inside it (1 % tolerance for the measured rate), out
        // only beyond 3 %, so dips around an edge do not flip the indicator.
        const double out = in_range ? 0.03 : 0.01;
        const bool above = top > 0 && fps > top * (1.0 + out);
        const bool below = range_known && fps < range_min * (1.0 - out);
        in_range = !above && !below;
        if (above) {
            return {0, why + ", above the range"};
        }
        if (below) {
            return {0, why + ", below the range"};
        }
        return {1, why};
    }

    void Resolve(HWND hwnd) {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        Amd::ReleaseObject(freesync);
        range_known = false;
        mon = FindMonitor(hwnd);
        if (!mon.valid || mon.width == 0 || mon.height == 0) {
            hidden = "the window's monitor is not in Windows' display paths";
            return;
        }
        void* desktops_service = nullptr;
        void* desktops = nullptr;
        if (Amd::Call<Amd::Result>(Amd::adlx_system, Amd::SystemGetDesktopsServices, &desktops_service) != 0 ||
            !desktops_service ||
            Amd::Call<Amd::Result>(desktops_service, Amd::DesktopServicesGetDesktops, &desktops) != 0 || !desktops) {
            Amd::ReleaseObject(desktops_service);
            hidden = "ADLX desktops not read";
            return;
        }
        // The desktop at the monitor's place and size: one only, a single display, on the
        // monitor's Windows source when ADLX maps it.
        void* found = nullptr; // IADLXDesktop
        u32 matches = 0;
        bool single = false, source_ok = true;
        int mapped_source = -1;
        const u32 count = Amd::Call<u32>(desktops, Amd::ListSize);
        for (u32 i = 0; i < count && i < 64; ++i) {
            void* desktop = nullptr;
            if (Amd::Call<Amd::Result>(desktops, Amd::ListAtTyped, i, &desktop) != 0 || !desktop) {
                continue;
            }
            int w = 0, h = 0, type = -1;
            Amd::Point top_left{};
            if (Amd::Call<Amd::Result>(desktop, Amd::DesktopSize, &w, &h) == 0 &&
                Amd::Call<Amd::Result>(desktop, Amd::DesktopTopLeft, &top_left) == 0 &&
                top_left.x == mon.position.x && top_left.y == mon.position.y && u32(w) == mon.width &&
                u32(h) == mon.height) {
                ++matches;
                Amd::ReleaseObject(found);
                found = desktop;
                single = Amd::Call<Amd::Result>(desktop, Amd::DesktopType, &type) == 0 && type == Amd::DesktopSingle;
                if (Amd::mapping) {
                    int adapter = 0, source = 0, bus = 0, dev = 0, fn = 0;
                    if (Amd::Call<Amd::Result>(Amd::mapping, Amd::MappingAdlIdsFromDesktop, desktop, &adapter,
                                               &source, &bus, &dev, &fn) == 0) {
                        mapped_source = source;
                        source_ok = u32(source) == mon.source;
                    }
                }
                continue;
            }
            Amd::ReleaseObject(desktop);
        }
        Amd::ReleaseObject(desktops);
        Amd::ReleaseObject(desktops_service);
        char where[96];
        std::snprintf(where, sizeof(where), "%s (%ld,%ld %ux%u, Windows source %u)", mon.name.c_str(),
                      mon.position.x, mon.position.y, mon.width, mon.height, mon.source);
        if (matches != 1) {
            Amd::ReleaseObject(found);
            hidden = matches == 0 ? std::string("no AMD desktop matches ") + where
                                  : std::string("more than one AMD desktop matches ") + where;
            return;
        }
        if (!single) {
            Amd::ReleaseObject(found);
            hidden = std::string("the AMD desktop of ") + where + " is not a single display (duplicate or Eyefinity)";
            return;
        }
        if (!source_ok) {
            Amd::ReleaseObject(found);
            hidden = std::string("ADLX places the desktop of ") + where + " on source " +
                     std::to_string(mapped_source);
            return;
        }
        void* displays = nullptr;
        void* display = nullptr;
        const bool listed = Amd::Call<Amd::Result>(found, Amd::DesktopGetDisplays, &displays) == 0 && displays &&
                            Amd::Call<u32>(displays, Amd::ListSize) == 1 &&
                            Amd::Call<Amd::Result>(displays, Amd::ListAtTyped, 0u, &display) == 0 && display;
        Amd::ReleaseObject(displays);
        Amd::ReleaseObject(found);
        void* services = nullptr;
        if (!listed || Amd::Call<Amd::Result>(Amd::adlx_system, Amd::SystemGetDisplaysServices, &services) != 0 ||
            !services ||
            Amd::Call<Amd::Result>(services, Amd::DisplayServicesGetFreeSync, display, &freesync) != 0) {
            freesync = nullptr;
            Amd::ReleaseObject(services);
            Amd::ReleaseObject(display);
            hidden = std::string("ADLX FreeSync of ") + where + " not found";
            return;
        }
        Amd::ReleaseObject(services);
        const char* adlx_name = nullptr;
        Amd::Call<Amd::Result>(display, Amd::DisplayName, &adlx_name);
        std::string range = "range not known (no ADL)";
        int adapter = -1, index = -1, bus = 0, dev = 0, fn = 0;
        if (Amd::mapping &&
            Amd::Call<Amd::Result>(Amd::mapping, Amd::MappingAdlIdsFromDisplay, display, &adapter, &index, &bus,
                                   &dev, &fn) == 0 &&
            Amd::Adl::Load()) {
            Amd::Adl::FreeSyncCap cap{};
            const int status = Amd::Adl::get_cap(Amd::Adl::context, adapter, index, &cap);
            if (status == 0) {
                if ((cap.caps & Amd::Adl::CapSupported) && cap.min_uhz > 0 && cap.max_uhz > cap.min_uhz) {
                    range_known = true;
                    range_min = cap.min_uhz / 1e6;
                    range_max = cap.max_uhz / 1e6;
                }
                char text[128];
                std::snprintf(text, sizeof(text), "ADL range %.1f-%.1f Hz, abilities 0x%x", cap.min_uhz / 1e6,
                              cap.max_uhz / 1e6, cap.caps);
                range = text;
            } else {
                range = "ADL2_Display_FreeSync_Cap failed (" + std::to_string(status) + ")";
            }
        }
        Amd::ReleaseObject(display);
        char line[320];
        std::snprintf(line, sizeof(line), "VRR: ADLX %s: display \"%s\", ADL %d/%d, %.2f Hz, %s\n", where,
                      adlx_name ? adlx_name : "", adapter, index, mon.refresh, range.c_str());
        if (line != logged) {
            Log("%s", line);
            logged = line;
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
std::atomic<float> screen_fps{0.0f}; ///< the frames on screen per second (the FPS counter)
std::atomic<int> present_mode{-1};   ///< the swapchain's VkPresentModeKHR (-1: none yet)

const char* PresentModeName(int mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "Immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "Mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO Relaxed";
    default:
        return "other";
    }
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void PollThread() {
#ifdef _WIN32
    NvSource nv;
    const bool use_nv = vendor == 0x10de && Nv::Load();
    // AMD: ADLX where the live present timing is not offered (bbport_vrr.h).
    AmdSource amd;
    const bool use_amd = vendor == 0x1002 && !Timing::swapchain_on && Amd::Load();
    if (use_amd) {
        // Window and monitor rectangles in physical pixels, whatever the process' DPI awareness.
        using SetContext = void*(WINAPI*)(void*);
        if (const auto set = reinterpret_cast<SetContext>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))) {
            set(reinterpret_cast<void*>(intptr_t(-4))); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }
#else
    const bool use_nv = false;
    const bool use_amd = false;
#endif
    if (!use_nv && !use_amd && !Timing::swapchain_on && !Timing::device_on) {
        Log("VRR indicator hidden: no source on this GPU/driver (NVAPI, ADLX or VK_EXT_present_timing)\n");
        std::scoped_lock lk{shown_mutex};
        label = nullptr;
        return;
    }
    Log("VRR: indicator source %s%s\n",
        use_nv    ? "NVAPI"
        : use_amd ? "ADLX (FreeSync on for the display, fullscreen, in front, frames in its range)"
                  : "present timing",
        use_nv && Timing::device_on ? " (present timing logged beside it)" : "");
    int candidate = -2;
    Clock::time_point candidate_since{};
    int last_raw_state = -3;
    Clock::time_point last_raw_time{};
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
        } else if (use_amd && !Timing::swapchain_on) {
            // The swapchain's present mode as made (after any fallback of an unsupported mode).
            const int mode = present_mode.load(std::memory_order_relaxed);
            const bool fifo = mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            raw = hwnd ? amd.Sample(static_cast<HWND>(hwnd), front, screen_fps.load(std::memory_order_relaxed), fifo)
                       : Raw{-1, "no window yet"};
            if (raw.state >= 0) {
                raw.why += std::string(", ") + PresentModeName(mode);
            }
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
            // Each change of the raw state, and every 5 seconds.
            const auto at = Clock::now();
            if (raw.state != last_raw_state || at - last_raw_time >= std::chrono::seconds(5)) {
                Log("VRR raw: %s\n", line.c_str());
                last_raw_state = raw.state;
                last_raw_time = at;
            }
        }
        const auto now = Clock::now();
        if (raw.state != candidate) {
            candidate = raw.state;
            candidate_since = now;
        }
        // Half a second steady before the indicator follows (not known: keep what is shown); the
        // derived AMD state a full second (frame rate dips around a range edge, loading screens).
        const auto hold = std::chrono::milliseconds(use_amd && !Timing::swapchain_on ? 1000 : 500);
        if (candidate != shown && candidate != -1 && now - candidate_since >= hold) {
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
    if (indicator_forced) {
        BbSettings::Get().show_vrr = true; // the starting value only: the menu's checkbox follows it
    }
    vendor = physical.getProperties().vendorID;
    label = vendor == 0x10de ? "G-SYNC" : vendor == 0x1002 ? "FreeSync" : "VRR";
    // The indicator is an option (off by default): without it (and without BB_VRR_LOG=1) at start
    // the present timing extensions are not enabled. NVAPI and ADLX start only when shown.
    if (!BbSettings::Get().show_vrr && !log_raw) {
        return nullptr;
    }
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

void OnSwapchain(vk::SwapchainKHR chain, vk::PresentModeKHR mode) {
    if (present_mode.exchange(int(mode)) != int(mode)) {
        Log("VRR: swapchain present mode %s\n", PresentModeName(int(mode)));
    }
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

Shown Get(float fps) {
    // The option off: nothing started or queried (BB_VRR_LOG=1 runs the source for the log only).
    const bool option = BbSettings::Get().show_vrr.load(std::memory_order_relaxed); // live (the menu)
    if (!enabled || (!option && !log_raw)) {
        return {};
    }
    screen_fps.store(fps, std::memory_order_relaxed);
    last_wanted_ms.store(NowMs(), std::memory_order_relaxed);
    if (!poll_started.exchange(true)) {
        std::thread(PollThread).detach();
    }
    std::scoped_lock lk{shown_mutex};
    if (!option || !label || !shown_known) {
        return {};
    }
    Shown out{label, shown_active};
    if (vendor == 0x1002) {
        out.lit[0] = 0.95f, out.lit[1] = 0.25f, out.lit[2] = 0.22f; // AMD red, readable over the game
    }
    return out;
}

} // namespace BbVrr
