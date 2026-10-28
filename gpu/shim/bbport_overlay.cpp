// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>

#include <SDL3/SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "bbport_settings.h"
#include <miniz.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f, last_present_ms = 0.0f;

// The game's own frames (present thread, like Render): their intervals, newest last, for the
// counter's game frame rate, 1% low, slowest frame and graph over the last few seconds.
constexpr int GameFrameHistory = 1024;
constexpr float GameStatsWindowMs = 5000.0f;
constexpr int GraphFrames = 240;
float game_frame_ms[GameFrameHistory]{};
int game_frame_next = 0, game_frame_count = 0;
std::chrono::steady_clock::time_point last_game_frame{}, game_stats_at{};
float game_fps = 0.0f, game_low_fps = 0.0f, game_worst_ms = 0.0f;

// Memory for the counter's third line, once a second: the game's RAM (its working set, what Task
// Manager shows) and the video memory the driver counts for it (VK_EXT_memory_budget heap
// usage of the device-local heaps; on a simulated small card it includes the ballast).
vk::PhysicalDevice memory_gpu{};
bool memory_budget = false;
std::chrono::steady_clock::time_point memory_at{};
double ram_gib = 0.0, ram_total_gib = 0.0, vram_gib = 0.0, vram_total_gib = 0.0;

void UpdateMemory() {
    const auto now = std::chrono::steady_clock::now();
    if (now - memory_at < std::chrono::seconds(1) && memory_at.time_since_epoch().count() != 0) {
        return;
    }
    memory_at = now;
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
#ifdef _WIN32
    struct Counters {
        DWORD cb, page_faults;
        SIZE_T peak_working_set, working_set, quota_peak_paged, quota_paged, quota_peak_nonpaged,
            quota_nonpaged, pagefile, peak_pagefile;
    };
    using GetInfo = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
    static const auto get_info = reinterpret_cast<GetInfo>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo")));
    Counters counters{};
    counters.cb = sizeof(counters);
    if (get_info && get_info(GetCurrentProcess(), &counters, sizeof(counters))) {
        ram_gib = double(counters.working_set) / GiB;
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        ram_total_gib = double(status.ullTotalPhys) / GiB;
    }
#endif
    if (memory_gpu) {
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        vk::PhysicalDeviceMemoryProperties2 props{.pNext = memory_budget ? &budget : nullptr};
        memory_gpu.getMemoryProperties2(&props);
        u64 used = 0, total = 0;
        for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
            if (props.memoryProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                total += props.memoryProperties.memoryHeaps[i].size;
                used += memory_budget ? budget.heapUsage[i] : 0;
            }
        }
        vram_gib = double(used) / GiB;
        vram_total_gib = double(total) / GiB;
    }
}

/// The game frame `back` frames ago (0 = newest).
float GameFrameMs(int back) {
    return game_frame_ms[(game_frame_next - 1 - back + GameFrameHistory) % GameFrameHistory];
}

/// Twice a second: the rate, the average of the slowest 1% and the slowest frame of the last
/// five seconds.
void UpdateGameStats() {
    const auto now = std::chrono::steady_clock::now();
    if (now - game_stats_at < std::chrono::milliseconds(500)) {
        return;
    }
    game_stats_at = now;
    static float sorted[GameFrameHistory];
    int n = 0;
    float total = 0.0f;
    while (n < game_frame_count && total < GameStatsWindowMs) {
        sorted[n] = GameFrameMs(n);
        total += sorted[n++];
    }
    if (n == 0 || total <= 0.0f) {
        game_fps = game_low_fps = game_worst_ms = 0.0f;
        return;
    }
    std::sort(sorted, sorted + n, std::greater<float>());
    const int slowest = std::max(1, n / 100);
    float slow_total = 0.0f;
    for (int i = 0; i < slowest; ++i) {
        slow_total += sorted[i];
    }
    game_fps = 1000.0f * float(n) / total;
    game_low_fps = 1000.0f * float(slowest) / slow_total;
    game_worst_ms = sorted[0];
}

/// What the game renders and shows, from the launcher's start settings: "2288x960 -> 3440x1440
/// (Quality)", "3440x1440 (native)" or the game's own 1920x1080.
const char* RenderSizeText() {
    static const std::string text = [] {
        const char* render = std::getenv("BB_RENDER_RES");
        const char* output = std::getenv("BB_OUTPUT_RES");
        if (!render || !render[0]) {
            return std::string("1920x1080 (the game's own)");
        }
        if (!output || !output[0] || std::strcmp(render, output) == 0) {
            return std::string(render) + " (native)";
        }
        std::string text = std::string(render) + " -> " + output;
        // The launcher's scales (RENDER_SCALES): output width over render width.
        const float render_w = float(std::atoi(render)), output_w = float(std::atoi(output));
        if (render_w > 0.0f) {
            const float scale = output_w / render_w;
            text += scale < 1.6f ? " (Quality)" : scale < 1.85f ? " (Balanced)" : " (Performance)";
        }
        return text;
    }();
    return text.c_str();
}

/// The counter's render size: "2288x960 Q" (Balanced B, Performance P), "3440x1440".
const char* ShortRenderText() {
    static const std::string text = [] {
        const char* render = std::getenv("BB_RENDER_RES");
        const char* output = std::getenv("BB_OUTPUT_RES");
        if (!render || !render[0]) {
            return std::string("1920x1080");
        }
        if (!output || !output[0] || std::strcmp(render, output) == 0) {
            return std::string(render);
        }
        const float render_w = float(std::atoi(render)), output_w = float(std::atoi(output));
        const float scale = render_w > 0.0f ? output_w / render_w : 1.0f;
        return std::string(render) + (scale < 1.6f ? " Q" : scale < 1.85f ? " B" : " P");
    }();
    return text.c_str();
}

const char* UpscalerShortName(int upscaler) {
    return upscaler == BbSettings::UpscalerFsr3     ? "FSR3"
           : upscaler == BbSettings::UpscalerFsr4   ? "FSR4"
           : upscaler == BbSettings::UpscalerFsr411 ? "FSR4.1"
           : upscaler == BbSettings::UpscalerTaa    ? "TAA"
           : upscaler == BbSettings::UpscalerDlss   ? "DLSS"
           : upscaler == BbSettings::UpscalerXess   ? "XeSS"
                                                    : "NoAA";
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The widget runs before Store: as one call's arguments, `v` could be copied (clang: is)
// before the widget changes it, and a click stored the old value.
void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin("Uncharted — PERF BUILD — settings  (Insert / L3+R3)###Uncharted — settings  (Insert / L3+R3)", &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    UpdateGameStats();
    ImGui::Text("%.0f FPS  (%.1f ms)   1%% low %.0f", game_fps,
                game_fps > 0.0f ? 1000.0f / game_fps : 0.0f, game_low_fps);
    Hint("Changes here apply at once, for this session. The launcher's settings apply each time "
         "the game starts: set the same there to keep a change.");

    ImGui::SeparatorText("Picture");
    ImGui::Text("Rendering %s", RenderSizeText());
    Hint("Set in the launcher (Display: Render resolution); it applies when the game starts. "
         "Quality, Balanced and Performance render smaller and let the anti-aliasing (DLSS or "
         "FSR) rebuild the full size: Uncharted 3 only.");
    static const char* upscalers[] = {"Off (the game's own)", "FSR 3.1", "FSR 4 (INT8)",
                                     "FSR 4.1.1 (INT8)", "TAA", "DLSS (NVIDIA RTX)", "XeSS (Intel)"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo("Anti-aliasing", upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                : i == BbSettings::UpscalerXess   ? s.xess_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— not supported by this graphics card");
            }
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
    }
    if (const char* problem = s.xess_problem.load(); problem && upscaler == BbSettings::UpscalerXess) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "XeSS: %s", problem);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "FSR 4 unavailable: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("The mode selected above is active. FSR 4 can be selected again.");
        ImGui::PopTextWrapPos();
    }
    Hint("Smooths edges and keeps the picture stable from frame to frame; with an upscaled render "
         "resolution it also rebuilds the full size. DLSS needs a GeForce RTX card.");
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    ImGui::BeginDisabled(!upscaler_on || !s.sharpen);
    Slider("Sharpness", s.sharpness, 0.0f, 2.0f);
    ImGui::EndDisabled();
    Hint("Sharpens the anti-aliased picture (AMD RCAS, also with DLSS). 0 is off; the launcher's "
         "Low, Medium and High are 0.2, 0.4 and 0.7. Ctrl+click to type a value.");

    ImGui::SeparatorText("Frame generation");
    static const char* frame_gens[] = {"Off", "FSR 3 (any graphics card)",
                                       "DLSS (NVIDIA RTX 40/50)"};
    const int frame_gen = s.frame_gen;
    if (ImGui::BeginCombo("Frame generation", frame_gens[frame_gen])) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            const bool supported = i != BbSettings::FrameGenDlss || s.dlssg_max_multiplier > 1;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                Store(s.frame_gen, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— not supported by this graphics card");
            }
        }
        ImGui::EndCombo();
    }
    const int max_multiplier = s.frame_gen == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier.load() : 2;
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff || max_multiplier <= 2);
    int multiplier = std::min(s.frame_gen_multiplier.load(), max_multiplier);
    if (ImGui::SliderInt("Multiplier", &multiplier, 2, std::max(max_multiplier, 2), "%dx")) {
        Store(s.frame_gen_multiplier, multiplier, true);
    }
    ImGui::EndDisabled();
    if (const char* problem = s.frame_gen_problem.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
    }
    Hint("Generated frames between the game's own: 60 FPS on screen becomes 120. Adds a little "
         "input latency; menus and cutscene videos are not generated. The launcher's Frame rate "
         "limit counts generated frames.");

    ImGui::SeparatorText("Other");
    Checkbox("FPS counter in the corner", s.show_fps);
    ImGui::BeginDisabled(!s.show_fps);
    Checkbox("Frame time graph", s.show_fps_graph);
    ImGui::EndDisabled();
    Hint("Top line, the game's own frames: FPS and frame time, 1%L the average of the slowest "
         "1% (stutter), MAX the slowest frame (last 5 seconds). Below: the anti-aliasing, the "
         "render size (Q, B, P: Quality, Balanced, Performance) and with frame generation FG, "
         "the frames on screen. The graph: the game's frame times, a spike is a hitch.");

    // Diagnosis and tuning, kept out of the way: the defaults are right for the three games.
    if (ImGui::CollapsingHeader("Developer options")) {
        Checkbox("Sub-pixel jitter", s.jitter);
        Hint("Each frame the scene shifts by a fraction of a pixel and the anti-aliasing gathers "
             "detail from several frames. Off: smoothing from the history only.");
        Checkbox("Sharpening pass", s.sharpen);
        if (BbSettings::IsFsr4(s.upscaler)) {
            Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
            Checkbox("FSR 4: inverted jitter sign", s.fsr4_invert_jitter);
        }
        const bool taa = s.upscaler == BbSettings::UpscalerTaa;
        ImGui::BeginDisabled(taa || !upscaler_on);
        Checkbox("Reactive mask", s.reactive);
        Hint("Marks transparent effects (particles, haze) so the upscaler relies less on past "
             "frames: fewer trails behind effects, more shimmer under them.");
        ImGui::BeginDisabled(!s.reactive);
        Slider("Mask scale", s.reactive_scale, 0.0f, 4.0f);
        Slider("Mask threshold", s.reactive_threshold, 0.0f, 1.0f);
        Slider("Mask maximum", s.reactive_max, 0.0f, 1.0f);
        bool show_mask = s.debug_view == BbSettings::DebugReactive;
        if (ImGui::Checkbox("Show the mask", &show_mask)) {
            s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        Checkbox("Character motion vectors", s.object_motion);
        Hint("Exact vectors for animated objects (applies after restarting the game).");
        bool show_motion = s.debug_view == BbSettings::DebugMotion;
        if (ImGui::Checkbox("Show the motion vectors", &show_motion)) {
            s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        }
        Hint("Red/green: horizontal/vertical motion (8 pixels = full brightness); blue: an exact "
             "object vector.");
    }
    if (s.object_motion != s.startup_object_motion) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Changes apply after restarting the game");
        if (ImGui::Button("Apply and restart the game")) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * base_scale, 4.0f * base_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * base_scale, 1.0f * base_scale));
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.8f);
    const auto& s = BbSettings::Get();
    UpdateGameStats();
    // "60 FPS 16.7ms 1%L 52 MAX 24ms": the game's own frames; 1%L the average of the slowest
    // 1%, MAX the slowest frame (5 seconds).
    ImGui::Text("%.0f FPS %.1fms  1%%L %.0f  MAX %.0fms", game_fps,
                game_fps > 0.0f ? 1000.0f / game_fps : 0.0f, game_low_fps, game_worst_ms);
    // Offline perf build (feature/native-batch).
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "PERF BUILD");
    // "DLSS 2288x960 Q  FG 2x 120": anti-aliasing, render size (Quality, Balanced,
    // Performance), and with frame generation adding frames (none in menus and movies) the
    // frames on screen.
    // Lighter than TextDisabled, which is hard to read over the game.
    const ImVec4 second_line(0.8f, 0.8f, 0.8f, 1.0f);
    const float shown_fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    if (s.frame_gen != BbSettings::FrameGenOff && shown_fps > game_fps * 1.2f) {
        ImGui::TextColored(second_line, "%s %s  FG %dx %.0f", UpscalerShortName(s.upscaler),
                            ShortRenderText(),
                            s.frame_gen == BbSettings::FrameGenDlss ? s.frame_gen_multiplier.load()
                                                                    : 2,
                            shown_fps);
    } else {
        ImGui::TextColored(second_line, "%s %s", UpscalerShortName(s.upscaler), ShortRenderText());
    }
    // "RAM 6.1/32 GB  VRAM 7.4/16 GB": the game's memory and video memory, once a second.
    UpdateMemory();
    if (ram_total_gib > 0.0 && vram_total_gib > 0.0 && memory_budget) {
        ImGui::TextColored(second_line, "RAM %.1f/%.0f GB  VRAM %.1f/%.0f GB", ram_gib, ram_total_gib,
                           vram_gib, vram_total_gib);
    } else if (ram_total_gib > 0.0) {
        ImGui::TextColored(second_line, "RAM %.1f/%.0f GB", ram_gib, ram_total_gib);
    }
    if (s.show_fps_graph && game_frame_count > 1) {
        static float graph[GraphFrames];
        const int n = std::min(game_frame_count, GraphFrames);
        float top = 0.0f;
        for (int i = 0; i < n; ++i) {
            graph[i] = GameFrameMs(n - 1 - i);
            top = std::max(top, graph[i]);
        }
        // At least 33 ms high, so a steady 60 or 120 sits low and a hitch stands out.
        ImGui::PlotLines("##frametimes", graph, n, 0, nullptr, 0.0f, std::max(top, 33.3f),
                         ImVec2(ImGui::GetContentRegionAvail().x, 28.0f * base_scale));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}


// ---- Trophy notices (NotifyTrophy): the console's "trophy earned" box, one at a time ----------
struct Toast {
    int grade = 4;
    std::string game, name;
    std::vector<unsigned char> png;
};
std::mutex toast_mutex;
std::deque<Toast> toast_queue;       // guarded by toast_mutex
std::atomic<bool> toast_busy{false}; // something is queued or on screen
bool toast_on = false;
Toast toast_now;
std::chrono::steady_clock::time_point toast_start;
ImTextureData* toast_icon = nullptr; // one 256x256 texture, rewritten for every notice
ImVec2 toast_icon_uv{1.0f, 1.0f};
constexpr float ToastSeconds = 6.0f;
constexpr int ToastIconSize = 256;

// 8-bit RGB/RGBA PNG without interlacing (what the trophy packs hold) to RGBA.
bool DecodePng(const std::vector<unsigned char>& png, std::vector<unsigned char>& rgba, int& width,
               int& height) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 33 || std::memcmp(png.data(), signature, 8) != 0) {
        return false;
    }
    const auto be32 = [](const unsigned char* p) {
        return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
    };
    std::vector<unsigned char> idat;
    int color_type = -1;
    for (size_t at = 8; at + 12 <= png.size();) {
        const u32 length = be32(&png[at]);
        const unsigned char* type = &png[at + 4];
        if (length > png.size() || at + 12 + length > png.size()) {
            return false;
        }
        const unsigned char* body = &png[at + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            width = int(be32(body));
            height = int(be32(body + 4));
            color_type = body[9];
            if (body[8] != 8 || (color_type != 2 && color_type != 6) || body[12] != 0) {
                return false;
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + length;
    }
    if (color_type < 0 || width <= 0 || height <= 0 || width > ToastIconSize ||
        height > ToastIconSize || idat.empty()) {
        return false;
    }
    const int bpp = color_type == 6 ? 4 : 3;
    const size_t stride = size_t(width) * bpp;
    std::vector<unsigned char> raw((stride + 1) * height);
    mz_ulong raw_size = mz_ulong(raw.size());
    if (mz_uncompress(raw.data(), &raw_size, idat.data(), mz_ulong(idat.size())) != MZ_OK ||
        raw_size != raw.size()) {
        return false;
    }
    std::vector<unsigned char> pixels(stride * height);
    for (int y = 0; y < height; ++y) {
        const unsigned char* in = &raw[y * (stride + 1)];
        const int filter = *in++;
        unsigned char* row = &pixels[y * stride];
        const unsigned char* up = y ? row - stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(bpp) ? row[x - bpp] : 0;
            const int b = up ? up[x] : 0;
            const int c = (up && x >= size_t(bpp)) ? up[x - bpp] : 0;
            int predictor = 0;
            switch (filter) {
            case 0: break;
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b),
                          pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return false;
            }
            row[x] = static_cast<unsigned char>(in[x] + predictor);
        }
    }
    rgba.assign(size_t(width) * height * 4, 255);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        std::memcpy(&rgba[i * 4], &pixels[i * bpp], bpp);
    }
    return true;
}

// Puts the notice's icon into the shared texture (the first notice creates it).
void SetToastIcon(const std::vector<unsigned char>& png) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    const bool ok = !png.empty() && DecodePng(png, rgba, w, h);
    if (!ok && !toast_icon) {
        return;
    }
    const bool created = toast_icon == nullptr;
    if (created) {
        toast_icon = IM_NEW(ImTextureData)();
        toast_icon->Create(ImTextureFormat_RGBA32, ToastIconSize, ToastIconSize);
        toast_icon->UseColors = true;
    }
    std::memset(toast_icon->Pixels, 0, size_t(ToastIconSize) * ToastIconSize * 4);
    if (ok) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(toast_icon->GetPixelsAt(0, y), &rgba[size_t(y) * w * 4], size_t(w) * 4);
        }
        toast_icon_uv = ImVec2(float(w) / ToastIconSize, float(h) / ToastIconSize);
    } else {
        toast_icon_uv = ImVec2(0.0f, 0.0f); // nothing to show: an empty square
    }
    if (created) {
        ImGui::RegisterUserTexture(toast_icon);
    } else {
        toast_icon->UpdateRect = {0, 0, ToastIconSize, ToastIconSize};
        toast_icon->Updates.push_back(toast_icon->UpdateRect);
        toast_icon->Status = ImTextureStatus_WantUpdates;
    }
}

void TrophyToast() {
    const auto now = std::chrono::steady_clock::now();
    if (!toast_on) {
        std::scoped_lock lock{toast_mutex};
        if (toast_queue.empty()) {
            toast_busy = false;
            return;
        }
        toast_now = std::move(toast_queue.front());
        toast_queue.pop_front();
        toast_on = true;
        toast_start = now;
        SetToastIcon(toast_now.png);
        toast_now.png = {};
    }
    const float t = std::chrono::duration<float>(now - toast_start).count();
    if (t >= ToastSeconds) {
        toast_on = false;
        return;
    }
    const float s = base_scale;
    const float slide = std::min(t / 0.35f, 1.0f);
    const float eased = 1.0f - (1.0f - slide) * (1.0f - slide);
    const float alpha = std::min(1.0f, (ToastSeconds - t) / 0.5f);
    static const ImVec4 colors[] = {ImVec4(0.62f, 0.82f, 0.96f, 1.0f), ImVec4(0.96f, 0.78f, 0.28f, 1.0f),
                                    ImVec4(0.80f, 0.83f, 0.88f, 1.0f), ImVec4(0.82f, 0.52f, 0.24f, 1.0f)};
    static const char* const names[] = {"Platinum trophy earned", "Gold trophy earned",
                                        "Silver trophy earned", "Bronze trophy earned"};
    const int grade = std::clamp(toast_now.grade, 1, 4) - 1;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + (18.0f - (1.0f - eased) * 110.0f) * s),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f * s, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s, 12.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, colors[grade]);
    ImGui::Begin("##trophy_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (toast_icon) {
        ImGui::Image(toast_icon->GetTexRef(), ImVec2(64.0f * s, 64.0f * s), ImVec2(0.0f, 0.0f),
                     toast_icon_uv);
        ImGui::SameLine(0.0f, 12.0f * s);
    }
    ImGui::BeginGroup();
    ImGui::TextColored(colors[grade], "%s", names[grade]);
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(toast_now.name.c_str());
    ImGui::SetWindowFontScale(1.0f);
    if (!toast_now.game.empty()) {
        ImGui::TextDisabled("%s", toast_now.game.c_str());
    }
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    memory_gpu = instance.GetPhysicalDevice();
    memory_budget = instance.CanReportMemoryUsage();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

std::atomic<int> prep_phase{0};
std::atomic<u32> prep_done{0};
std::atomic<u32> prep_total{0};

std::atomic<s64> prep_phase_start_ms{0};

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void SetShaderPreparation(int phase, u32 done, u32 total) {
    if (prep_phase.load(std::memory_order_relaxed) != phase) {
        prep_phase_start_ms.store(NowMs(), std::memory_order_relaxed);
    }
    prep_done.store(done, std::memory_order_relaxed);
    prep_total.store(total, std::memory_order_relaxed);
    prep_phase.store(phase, std::memory_order_release);
}

// "about 12 s left": the phase's time so far, extrapolated over what is left (after a second, once a few are done).
std::string PreparationEta(u32 done, u32 total) {
    const s64 elapsed = NowMs() - prep_phase_start_ms.load(std::memory_order_relaxed);
    if (done < 8 || total <= done || elapsed < 1000) {
        return {};
    }
    const s64 left_s = (elapsed * s64(total - done) / s64(done) + 999) / 1000;
    return left_s >= 60 ? fmt::format("  about {} min {} s left", left_s / 60, left_s % 60)
                        : fmt::format("  about {} s left", left_s);
}

// "Preparing shaders 1240 / 4496": bottom left while the variant list is built behind the game.
void ShaderPreparation() {
    const int phase = prep_phase.load(std::memory_order_acquire);
    if (phase == 0) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 24.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * base_scale, 10.0f * base_scale));
    ImGui::Begin("##prep", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.25f);
    const u32 done = prep_done.load(std::memory_order_relaxed);
    const u32 total = prep_total.load(std::memory_order_relaxed);
    const char* what = phase == 1 ? "shaders" : "pipelines";
    if (total == 0) {
        ImGui::Text("Preparing %s", what);
    } else {
        ImGui::Text("Preparing %s %u / %u%s", what, done, total, PreparationEta(done, total).c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || BbSettings::Get().show_fps ||
                           toast_busy || prep_phase.load(std::memory_order_relaxed) != 0);
}

void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char> png) {
    std::scoped_lock lock{toast_mutex};
    if (toast_queue.size() < 32) {
        toast_queue.push_back({grade, std::move(game), std::move(name), std::move(png)});
        toast_busy = true;
    }
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void NoteGameFrame() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_game_frame).count();
    last_game_frame = now;
    // Pauses longer than a second (loading, the window in the background) are not frames.
    if (ms <= 0.0f || ms >= 1000.0f) {
        return;
    }
    game_frame_ms[game_frame_next] = ms;
    game_frame_next = (game_frame_next + 1) % GameFrameHistory;
    game_frame_count = std::min(game_frame_count + 1, GameFrameHistory);
}

// Called for every presented frame: the interval for the FPS readout and BB_FPS_LOG.
void NotePresent() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_present_ms = ms;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // BB_FPS_LOG=1: presents per second and the slowest frame, every 5 seconds (test runs).
    static const bool fps_log = [] {
        const char* env = std::getenv("BB_FPS_LOG");
        return env && env[0] == '1';
    }();
    if (fps_log) {
        static auto window_start = now;
        static int frames = 0;
        static float worst = 0.0f;
        ++frames;
        worst = std::max(worst, ms < 1000.0f ? ms : 0.0f);
        const float elapsed = std::chrono::duration<float>(now - window_start).count();
        if (elapsed >= 5.0f) {
            std::printf("Frame rate: %.1f FPS, slowest frame %.1f ms\n", frames / elapsed, worst);
            window_start = now;
            frames = 0;
            worst = 0.0f;
        }
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const float ms = last_present_ms;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    TrophyToast();
    ShaderPreparation();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
