// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
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
#include "bbport_i18n.h"
#include "bbport_saves.h"
#include "bbport_settings.h"
#include <miniz.h>
#include "imgui.h"

// runtime_thread.c: the game's threads wait while the preparation page shows (BB_PREP_FREEZE=0: they run).
extern "C" void runtime_freeze_guest(int on);
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
// Shader and pipeline compiles since the start (vk_pipeline_cache.cpp): on the GPU thread (the ones a frame waits
// for) and on the draw-preparation workers.
extern std::atomic<u32> g_bb_compiles_total, g_bb_worker_compiles_total;
extern std::atomic<u64> g_bb_compile_ns_total;
} // namespace Vulkan

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

// Noto Sans JP, cut down to the menu's Japanese text (third_party/fonts, SIL OFL 1.1; tools/make_overlay_font_ja.py):
// merged into the font when the menu speaks Japanese.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ja_ttf\n"
    "bb_font_ja_ttf:\n"
    ".incbin \"" BB_FONT_JA_PATH "\"\n"
    ".global bb_font_ja_ttf_end\n"
    "bb_font_ja_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ja_ttf\n"
    ".global bb_font_ja_ttf\n"
    "bb_font_ja_ttf:\n"
    ".incbin \"" BB_FONT_JA_PATH "\"\n"
    ".hidden bb_font_ja_ttf_end\n"
    ".global bb_font_ja_ttf_end\n"
    "bb_font_ja_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ja_ttf[];
extern "C" const unsigned char bb_font_ja_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)
extern "C" void runtime_mouse_get(float* look, float* aim, float* vertical); // runtime_pad.c
extern "C" void runtime_mouse_set(float look, float aim, float vertical);
extern "C" float runtime_mouse_camera_vertical(void);
extern "C" void runtime_mouse_camera_vertical_set(float v);
extern "C" int runtime_dev_text_shown(void);  // runtime_pad.c: developer text 1 shown, 0 hidden, -1 no developer mode

namespace BbOverlay {

namespace {

using BbI18n::T;

// A translated label that keeps the English text as its ImGui ID ("Laden###Load"): two texts that translate alike
// stay two widgets.
const char* TI(const char* english) {
    const char* text = T(english);
    if (text == english) {
        return english;
    }
    static std::unordered_map<const char*, std::string> labels;  // the overlay's thread (imgui_mutex)
    auto [it, added] = labels.try_emplace(english);
    if (added) {
        it->second = std::string(text) + "###" + english;
    }
    return it->second.c_str();
}

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
std::atomic<bool> choice_active{false}; // the save list (ChoiceBox) is open
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
            return std::string("1920x1080 ") + T("(the game's own)");
        }
        if (!output || !output[0] || std::strcmp(render, output) == 0) {
            return std::string(render) + " " + T("(native)");
        }
        std::string text = std::string(render) + " -> " + output;
        // The launcher's scales (RENDER_SCALES): output width over render width.
        const float render_w = float(std::atoi(render)), output_w = float(std::atoi(output));
        if (render_w > 0.0f) {
            const float scale = output_w / render_w;
            text += std::string(" ") +
                    (scale < 1.6f ? T("(Quality)") : scale < 1.85f ? T("(Balanced)") : T("(Performance)"));
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

/// The counter's anti-aliasing name: DLSS at the output size is "DLAA", with the DLSS model after it: the letter
/// BB_DLSS_PRESET asks for (J, K, L, M), or "auto" when the driver chooses (vk_dlss.cpp DlssPreset; an override in
/// the NVIDIA app is not reported to the game). "DLAA K", "DLSS auto", "FSR4".
static const char* CounterUpscalerName(int upscaler) {
    if (upscaler != BbSettings::UpscalerDlss) {
        return UpscalerShortName(upscaler);
    }
    static const std::string dlaa = std::string("DLAA ") + [] {
        const char* env = std::getenv("BB_DLSS_PRESET");
        return std::string(env && env[0] ? std::string(1, char(std::toupper(static_cast<unsigned char>(env[0]))))
                                         : std::string("auto"));
    }();
    static const std::string dlss = "DLSS" + dlaa.substr(4);
    const char* render = std::getenv("BB_RENDER_RES");
    const char* output = std::getenv("BB_OUTPUT_RES");
    const bool native = !render || !render[0] || !output || !output[0] || std::strcmp(render, output) == 0;
    return native ? dlaa.c_str() : dlss.c_str();
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // ImGui draws its own cursor while the menu is open (antikmewzik's SetOpen): it shows in exclusive fullscreen too,
    // where the system cursor may not; window.cpp hides the system one meanwhile (DrawsCursor). The save list draws it too.
    ImGui::GetIO().MouseDrawCursor = value || choice_active;
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

// A grey note that wraps at the window's edge (TextDisabled does not wrap: translated notes ran off the window).
void TextGrey(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
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

// Checkpoints page: chapter, then checkpoint, then Go there (the game's own task jump, bbport_saves.cpp and
// runtime_checkpoint.c). The first jump turns practice mode on: the game saves to a practice copy until it restarts.
void CheckpointsPage() {
    static std::vector<BbSaves::Checkpoint> list;
    static int chapter = -1;
    static int picked = -1;
    static std::string message;
    if (list.empty()) {
        list = BbSaves::Checkpoints();
    }
    const BbSaves::Checkpoint now = BbSaves::Current();
    // The current checkpoint in the list: the entry of its task, or (a cutscene or setup step, which the list leaves
    // out) the last playable checkpoint before it in game order.
    int current = -1;
    if (now.known) {
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            if (list[i].task == now.task) {
                current = i;
                break;
            }
            if (now.row >= 0 && list[i].row >= 0 && list[i].row <= now.row &&
                (current < 0 || list[i].row > list[current].row)) {
                current = i;
            }
        }
    }
    // The page follows the player: opening it, or a new current checkpoint, shows the current chapter and scrolls
    // the list to it. A chapter picked by hand stays until one of those happens.
    static int last_frame = -10;
    static uint32_t last_task = 0;
    const bool opened = ImGui::GetFrameCount() - last_frame > 1;
    last_frame = ImGui::GetFrameCount();
    bool scroll_to_current = false;
    if (opened || chapter < 0 || (now.known && now.task != last_task)) {
        if (current >= 0) {
            if (chapter != list[current].chapter) {
                picked = -1;  // a pick in another chapter would stay behind Go there, out of sight
            }
            chapter = list[current].chapter;
            scroll_to_current = true;
        } else if (chapter < 0) {
            chapter = now.known ? now.chapter : (list.empty() ? 1 : list.front().chapter);
        }
        last_task = now.task;
    }
    switch (BbSaves::JumpResult()) {
    case 1: message = T("Done."); break;
    case -1: message = T("This checkpoint is not in this release of the game."); break;
    case -2: message = T("Checkpoint jumps do not work with this release of the game."); break;
    default: break;
    }

    ImGui::TextWrapped(T("Now: %s"), BbSaves::Describe(now).c_str());
    if (!BbSaves::JumpsAvailable()) {
        ImGui::Spacing();
        TextGrey(T("Checkpoint jumps are not available for this release of the game."));
        return;
    }
    if (BbSaves::Practising()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", T("Practice mode: the game saves to a practice copy until you restart it. Your "
                                   "own games are not touched."));
        ImGui::PopStyleColor();
        static bool confirm_keep = false;
        if (!confirm_keep) {
            if (ImGui::Button(TI("Keep this progress"))) {
                confirm_keep = true;
            }
            Hint(T("To carry on from here for real (for example past a place that does not work): the practice "
                   "save becomes a new game of its own, next to your other games, and the game restarts with "
                   "Continue there. Your other games stay as they are."));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", T("Keep the practice save as a new game? The game restarts and Continue "
                                       "starts there. Your other games stay as they are."));
            ImGui::PopStyleColor();
            if (ImGui::Button(TI("Yes, keep it"))) {
                confirm_keep = false;
                BbSaves::KeepPractice(message);  // restarts the game when it works
            }
            ImGui::SameLine();
            if (ImGui::Button(TI("Cancel"))) {
                confirm_keep = false;
            }
        }
    } else {
        TextGrey(T("Jumping turns on practice mode: until the game restarts, it saves to a "
                                    "practice copy, not to your own save."));
    }

    // Chapters, from the list (game order).
    std::string chapter_label;
    for (const auto& c : list) {
        if (c.chapter == chapter) {
            chapter_label = std::to_string(c.chapter) + "  " + c.chapter_title;
            break;
        }
    }
    if (ImGui::BeginCombo(TI("Chapter"), chapter_label.c_str())) {
        // Each chapter once: a few checkpoints sit out of order (U3: two of chapter 12 come after chapter 13).
        std::vector<int> shown;
        for (const auto& c : list) {
            if (std::find(shown.begin(), shown.end(), c.chapter) != shown.end()) {
                continue;
            }
            shown.push_back(c.chapter);
            const std::string label = std::to_string(c.chapter) + "  " + c.chapter_title;
            if (ImGui::Selectable(label.c_str(), c.chapter == chapter)) {
                chapter = c.chapter;
                picked = -1;
            }
        }
        ImGui::EndCombo();
    }
    // The list takes the room left in the window (the Go there line and the window's Close button below it).
    const float list_h = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f,
                                  ImGui::GetTextLineHeightWithSpacing() * 8.0f);
    if (ImGui::BeginChild("##checkpoints", ImVec2(0.0f, list_h),
                          ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const auto& c = list[i];
            if (c.chapter != chapter) {
                continue;
            }
            const bool is_current = i == current;
            const std::string label = (is_current ? std::string("> ") : std::string("   ")) + std::to_string(c.number) +
                                      "  " + c.name + (is_current ? std::string("  ") + T("(now)") : std::string());
            if (is_current) {
                // Always marked: green text on a soft green row, also while another checkpoint is picked.
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 1.0f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.5f, 0.2f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.6f, 0.25f, 0.7f));
            }
            if (ImGui::Selectable(label.c_str(), i == picked || (is_current && picked < 0),
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                picked = i;
                // A double click goes there at once (the same as picking it and pressing Go there), but never to the
                // checkpoint the player is at (from El Dorado, michelle: the game's task reset there leaves the scene
                // without its characters).
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !(now.known && list[i].task == now.task)) {
                    BbSaves::Jump(list[i], message);
                }
            }
            if (is_current) {
                ImGui::PopStyleColor(3);
                if (scroll_to_current) {
                    ImGui::SetScrollHereY(0.35f);
                }
            }
        }
    }
    ImGui::EndChild();
    const bool already_here = picked >= 0 && now.known && list[picked].task == now.task;
    ImGui::BeginDisabled(picked < 0 || already_here);
    if (ImGui::Button(TI("Go there")) && picked >= 0 && !already_here) {
        BbSaves::Jump(list[picked], message);
    }
    ImGui::EndDisabled();
    Hint(T("Or double-click a checkpoint in the list."));
    if (!message.empty()) {
        ImGui::SameLine();
        ImGui::TextWrapped("%s", message.c_str());
    }
}

// Saves page: your games and the Undo copy, as in the game's own Save and Load lists (bbport_saves.cpp); each line
// where it is and when it was saved. Loading and deleting each ask first, on the page itself.
void SavesPage() {
    enum class Action { None, LoadGame, DeleteGame, LoadUndo, DeleteUndo };
    static Action pending = Action::None;
    static int pending_digit = -1;
    static std::string message;
    static std::vector<BbSaves::GameSave> games;
    static BbSaves::GameSave undo;
    static bool has_undo = false;
    static double refreshed = -10.0;
    // (re-read every 2 s; while a save copy holds the saves, the last reading stays and the next frame tries again)
    if (ImGui::GetTime() - refreshed > 2.0 && BbSaves::ReadSaves(games, undo, has_undo)) {
        refreshed = ImGui::GetTime();
    }
    const auto when = [](const std::string& date) { return date.empty() ? std::string() : "  (" + date + ")"; };
    const bool practice = BbSaves::Practising();

    ImGui::TextWrapped(T("Now: %s"), BbSaves::Describe(BbSaves::Current()).c_str());
    if (practice) {
        TextGrey(T("Practice mode: load or delete your games once the game has restarted."));
    }

    ImGui::SeparatorText(T("Your games"));
    if (games.empty()) {
        TextGrey(T("No games yet: they appear once the game has saved."));
    }
    for (const BbSaves::GameSave& g : games) {
        ImGui::PushID(g.digit);
        ImGui::BeginDisabled(practice);
        if (ImGui::Button(TI("Load"))) {
            pending = Action::LoadGame;
            pending_digit = g.digit;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(g.playing);
        if (ImGui::Button(TI("Delete"))) {
            pending = Action::DeleteGame;
            pending_digit = g.digit;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        const std::string name =
            fmt::format(fmt::runtime(g.playing ? T("Game {} - the one you are playing") : T("Game {}")), g.digit + 1);
        ImGui::TextWrapped("%s: %s%s", name.c_str(), BbSaves::Describe(g.checkpoint).c_str(), when(g.date).c_str());
        ImGui::PopID();
    }
    if (has_undo) {
        ImGui::SeparatorText(T("Undo"));
        ImGui::PushID("undo");
        ImGui::BeginDisabled(practice);
        if (ImGui::Button(TI("Load"))) {
            pending = Action::LoadUndo;
            pending_digit = undo.digit;
        }
        ImGui::SameLine();
        if (ImGui::Button(TI("Delete"))) {
            pending = Action::DeleteUndo;
            pending_digit = undo.digit;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const std::string name =
            fmt::format(fmt::runtime(T("Undo - Game {} as it was before it was saved over")), undo.digit + 1);
        ImGui::TextWrapped("%s: %s%s", name.c_str(), BbSaves::Describe(undo.checkpoint).c_str(),
                           undo.date.empty() ? ""
                                             : ("  (" + fmt::format(fmt::runtime(T("Kept {}")), undo.date) + ")").c_str());
        ImGui::PopID();
    }

    if (pending != Action::None) {
        ImGui::Spacing();
        const std::string question =
            pending == Action::LoadGame
                ? fmt::format(fmt::runtime(T("Load game {}? The game restarts and Continue starts there.")),
                              pending_digit + 1)
            : pending == Action::DeleteGame
                ? fmt::format(fmt::runtime(T("Delete game {}? It moves to the savedata-deleted folder (not erased).")),
                              pending_digit + 1)
            : pending == Action::LoadUndo
                ? fmt::format(fmt::runtime(T("Put game {} back as it was before it was saved over? The game restarts "
                                             "and Continue starts there. The save it replaces becomes the next Undo.")),
                              pending_digit + 1)
                : std::string(T("Delete the Undo copy? It moves to the deleted folder (not erased)."));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", question.c_str());
        ImGui::PopStyleColor();
        const bool yes = ImGui::Button(TI("Yes"));
        ImGui::SameLine();
        if (ImGui::Button(TI("Cancel"))) {
            pending = Action::None;
        }
        if (yes) {
            const Action action = pending;
            pending = Action::None;
            if (action == Action::LoadGame) {
                BbSaves::LoadGame(pending_digit, message);  // restarts the game when it works
            } else if (action == Action::DeleteGame) {
                BbSaves::DeleteGame(pending_digit, message);
            } else if (action == Action::LoadUndo) {
                BbSaves::LoadUndo(message);  // restarts the game when it works
            } else {
                BbSaves::DeleteUndo(message);
            }
            refreshed = -10.0;
        }
    }
    if (!message.empty()) {
        ImGui::TextWrapped("%s", message.c_str());
    }
    ImGui::Spacing();
    TextGrey(T("Your games, as in the game's own Save and Load lists. Treasures, unlocks and trophies are not "
               "changed."));
}

// ---- Controls page: the keyboard and mouse bindings and the mouse settings of the launcher's Controls page
// (launcher/uncharted_launcher.py build_controls), in the same input.ini (BB_INPUT_CONFIG, read by src/runtime_pad.c
// load_input). A change is written at once and the game takes the file in again before its next pad read
// (runtime_input_reload): it works straight away. The file's first line says the in-game menu wrote it, and the launcher
// takes those settings over at its next start (import_game_controls). Opened also by the "Keyboard / Mouse" row of the
// games' Options > Controls (antikmewzik's, runtime_exit_menu.c). Guarded by imgui_mutex (the capture: the window thread).

// The actions in the launcher's order (BUTTON_ACTIONS, STICK_ACTIONS) with their default keys ("A|B": two).
struct KeyAction {
    const char* name;
    const char* title;
    const char* keys;
};
constexpr KeyAction KeyActions[] = {
    {"cross", N_("Cross ✕"), "Space"},
    {"circle", N_("Circle ○"), "C"},
    {"square", N_("Square □"), "F"},
    {"triangle", N_("Triangle △"), "E"},
    {"l1", "L1", "Q"},
    {"r1", "R1", "G"},
    {"l2", N_("L2 (aim)"), "Mouse Right"},
    {"r2", N_("R2 (fire)"), "Mouse Left"},
    {"l3", "L3", "Left Shift"},
    {"r3", "R3", "V"},
    {"options", N_("Options"), "Escape|Return"},
    {"touchpad", N_("Touchpad"), "Tab"},
    {"up", N_("D-pad up"), "Up"},
    {"down", N_("D-pad down"), "Down"},
    {"left", N_("D-pad left"), "Left|Wheel Up"},
    {"right", N_("D-pad right"), "Right|Wheel Down"},
    {"ls_up", N_("Move forward"), "W"},
    {"ls_down", N_("Move back"), "S"},
    {"ls_left", N_("Move left"), "A"},
    {"ls_right", N_("Move right"), "D"},
    {"rs_up", N_("Look up"), "I"},
    {"rs_down", N_("Look down"), "K"},
    {"rs_left", N_("Look left"), "J"},
    {"rs_right", N_("Look right"), "L"},
    {"rotate_ccw", N_("Turn valve / wheel counter-clockwise (hold)"), "R"},
    {"rotate_cw", N_("Turn valve / wheel clockwise (hold)"), "T"},
    {"walk", N_("Walk (hold with the movement keys)"), "Left Ctrl"},
};
constexpr int KeyActionCount = int(sizeof(KeyActions) / sizeof(KeyActions[0]));
constexpr const char* GameControlsHeader = "# Uncharted controls (written by the game's in-game menu, Controls page)";

extern "C" void runtime_input_reload(void); // runtime_pad.c

struct ControlsState {
    bool loaded = false;
    std::string path;                         // BB_INPUT_CONFIG ("": the game was not started by the launcher)
    std::vector<std::string> lines;           // the file as read (other settings, the controller's, kept as they are)
    std::string keys[KeyActionCount][2];
    bool mouse_look = true, invert_y = false;
    float sensitivity = 2.8f, aim = 0.6f, vertical = 1.0f, camera_vertical = 2.0f;
    int walk_speed = 65;
    int capture_action = -1, capture_slot = 0; // a binding waits for a key (the window thread takes it)
    u64 capture_since_ms = 0;
    std::string message;                      // the last change, or why it was not saved
};
ControlsState controls;
std::atomic<bool> controls_tab_request{false}; // the next frame selects the Controls tab (OpenControls)

void SplitKeys(const std::string& value, std::string out[2]) {
    out[0].clear();
    out[1].clear();
    const size_t bar = value.find('|');
    out[0] = value.substr(0, bar);
    if (bar != std::string::npos) {
        out[1] = value.substr(bar + 1);
    }
    for (int i = 0; i < 2; ++i) {
        while (!out[i].empty() && out[i].back() == ' ') out[i].pop_back();
        while (!out[i].empty() && out[i].front() == ' ') out[i].erase(0, 1);
    }
}

void ControlsDefaults() {
    for (int a = 0; a < KeyActionCount; ++a) {
        SplitKeys(KeyActions[a].keys, controls.keys[a]);
    }
}

// The file (or the defaults the game uses without one, as load_input).
void ControlsLoad() {
    controls = ControlsState{};
    controls.loaded = true;
    ControlsDefaults();
    const char* path = std::getenv("BB_INPUT_CONFIG");
    controls.path = path && path[0] ? path : "";
    if (controls.path.empty()) {
        return;
    }
    FILE* f = std::fopen(controls.path.c_str(), "rb");
    if (!f) {
        return;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), f)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        controls.lines.push_back(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "mouse_look") controls.mouse_look = std::atoi(value.c_str()) != 0;
        else if (key == "mouse_invert_y") controls.invert_y = std::atoi(value.c_str()) != 0;
        else if (key == "mouse_sensitivity") controls.sensitivity = float(std::atof(value.c_str()));
        else if (key == "mouse_aim_sensitivity") controls.aim = float(std::atof(value.c_str()));
        else if (key == "mouse_vertical") controls.vertical = float(std::atof(value.c_str()));
        else if (key == "mouse_camera_vertical") controls.camera_vertical = float(std::atof(value.c_str()));
        else if (key == "walk_speed") controls.walk_speed = std::clamp(std::atoi(value.c_str()), 50, 100);
        else if (key.rfind("key.", 0) == 0) {
            for (int a = 0; a < KeyActionCount; ++a) {
                if (key.compare(4, std::string::npos, KeyActions[a].name) == 0) {
                    SplitKeys(value, controls.keys[a]);
                }
            }
        }
    }
    std::fclose(f);
    // as the launcher writes them (its slider ranges)
    controls.sensitivity = std::clamp(controls.sensitivity > 0.0f ? controls.sensitivity : 2.8f, 0.1f, 10.0f);
    controls.aim = std::clamp(controls.aim > 0.0f ? controls.aim : 0.6f, 0.25f, 4.0f);
    controls.vertical = std::clamp(controls.vertical > 0.0f ? controls.vertical : 1.0f, 0.25f, 4.0f);
    controls.camera_vertical = std::clamp(controls.camera_vertical > 0.0f ? controls.camera_vertical : 2.0f, 0.25f, 8.0f);
}

// Writes the file (the lines it had, ours changed or added) and has the game read it again.
void ControlsSave() {
    if (controls.path.empty()) {
        controls.message = T("The game was not started from the launcher (no input.ini): changes are not saved.");
        return;
    }
    std::vector<std::pair<std::string, std::string>> values = {
        {"mouse_look", controls.mouse_look ? "1" : "0"},
        {"mouse_sensitivity", fmt::format("{:.2f}", controls.sensitivity)},
        {"mouse_aim_sensitivity", fmt::format("{:.2f}", controls.aim)},
        {"mouse_vertical", fmt::format("{:.2f}", controls.vertical)},
        {"mouse_camera_vertical", fmt::format("{:.2f}", controls.camera_vertical)},
        {"mouse_invert_y", controls.invert_y ? "1" : "0"},
        {"walk_speed", std::to_string(controls.walk_speed)},
    };
    for (int a = 0; a < KeyActionCount; ++a) {
        const std::string& k0 = controls.keys[a][0];
        const std::string& k1 = controls.keys[a][1];
        values.emplace_back(std::string("key.") + KeyActions[a].name,
                            k0.empty() ? k1 : k1.empty() ? k0 : k0 + "|" + k1);
    }
    std::vector<bool> written(values.size(), false);
    std::string text = std::string(GameControlsHeader) + "\n";
    for (const std::string& line : controls.lines) {
        if (!line.empty() && line[0] == '#') {
            continue; // (the header: ours)
        }
        const size_t eq = line.find('=');
        std::string out = line;
        if (eq != std::string::npos) {
            for (size_t i = 0; i < values.size(); ++i) {
                if (line.compare(0, eq, values[i].first) == 0 && eq == values[i].first.size()) {
                    out = values[i].first + "=" + values[i].second;
                    written[i] = true;
                }
            }
        }
        text += out + "\n";
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (!written[i]) {
            text += values[i].first + "=" + values[i].second + "\n";
        }
    }
    // tmp + rename: the game and the launcher never read half a file
    const std::string tmp = controls.path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) {
        ok = std::fclose(f) == 0 && ok;
    }
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp.c_str(), controls.path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ok && std::rename(tmp.c_str(), controls.path.c_str()) == 0;
#endif
    if (!ok) {
        std::remove(tmp.c_str());
        controls.message = T("Could not write input.ini.");
        return;
    }
    // the lines as now written, for the next change
    controls.lines.clear();
    size_t start = 0;
    for (size_t end; (end = text.find('\n', start)) != std::string::npos; start = end + 1) {
        controls.lines.push_back(text.substr(start, end - start));
    }
    runtime_input_reload();
    std::printf("Overlay: controls saved to %s\n", controls.path.c_str());
}

// One key for one binding: the key `name` given to `action`'s `slot` is taken from any other action that had it (the
// launcher's rule), and a lone second binding moves to the first place.
void ControlsSetKey(int action, int slot, const std::string& name) {
    if (!name.empty()) {
        for (int a = 0; a < KeyActionCount; ++a) {
            for (int i = 0; i < 2; ++i) {
                if ((a != action || i != slot) && controls.keys[a][i] == name) {
                    controls.keys[a][i].clear();
                }
            }
            if (controls.keys[a][0].empty() && !controls.keys[a][1].empty()) {
                controls.keys[a][0].swap(controls.keys[a][1]);
            }
        }
    }
    controls.keys[action][slot] = name;
    if (controls.keys[action][0].empty() && !controls.keys[action][1].empty()) {
        controls.keys[action][0].swap(controls.keys[action][1]);
    }
    controls.capture_action = -1;
    controls.message = std::string(T(KeyActions[action].title)) + ": " + (name.empty() ? T("binding removed") : name);
    ControlsSave();
}

// The name of a pressed key as input.ini has it (the launcher's names, which runtime_pad.c parse_bind reads): a character
// key by the character the keyboard layout gives it ("Z" on the key labelled Z), any other key by its SDL name ("Left
// Shift", "Keypad 8", "F5"); "" for keys that cannot be bound here.
std::string KeyName(const SDL_KeyboardEvent& key) {
    const char* layout = SDL_GetKeyName(key.key);
    if (layout && layout[0] && !layout[1] && static_cast<unsigned char>(layout[0]) > ' ' &&
        static_cast<unsigned char>(layout[0]) < 127) {
        return std::string(1, char(std::toupper(static_cast<unsigned char>(layout[0]))));
    }
    const char* name = SDL_GetScancodeName(key.scancode);
    return name && name[0] ? name : "";
}

// Window thread, while a binding waits for a key: the event is taken (true) as the binding, or as Esc to cancel.
bool ControlsCaptureEvent(const SDL_Event& event) {
    if (controls.capture_action < 0) {
        return false;
    }
    const int action = controls.capture_action, slot = controls.capture_slot;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            return true;
        }
        if (event.key.key == SDLK_ESCAPE) {
            controls.capture_action = -1;
            controls.message = T("Cancelled.");
        } else if (event.key.key == SDLK_INSERT) {
            controls.message = T("Insert opens the in-game settings menu.");
        } else if (event.key.scancode == SDL_SCANCODE_LALT) {
            controls.message = T("Left Alt frees the mouse cursor while held.");
        } else if (const std::string name = KeyName(event.key); !name.empty()) {
            ControlsSetKey(action, slot, name);
        }
        return true;
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        // (the click that started the capture is over by then: ImGui buttons act on the release; a short grace anyway)
        if (SDL_GetTicks() - controls.capture_since_ms < 150) {
            return true;
        }
        const char* name = event.button.button == SDL_BUTTON_LEFT     ? "Mouse Left"
                           : event.button.button == SDL_BUTTON_RIGHT  ? "Mouse Right"
                           : event.button.button == SDL_BUTTON_MIDDLE ? "Mouse Middle"
                           : event.button.button == SDL_BUTTON_X1     ? "Mouse 4"
                           : event.button.button == SDL_BUTTON_X2     ? "Mouse 5"
                                                                      : nullptr;
        if (name) {
            ControlsSetKey(action, slot, name);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // the release of the press just bound (or of the click that started the capture) must not reach ImGui as a click
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.y != 0.0f) {
            ControlsSetKey(action, slot, event.wheel.y > 0.0f ? "Wheel Up" : "Wheel Down");
        }
        return true;
    default:
        return false;
    }
}

void ControlsPage() {
    if (!controls.loaded) {
        ControlsLoad();
    }
    TextGrey(T("Keyboard and mouse: the same settings as the launcher's Controls page, saved in input.ini. Changes "
               "work at once; the launcher takes them over the next time it starts."));
    if (controls.path.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           T("The game was not started from the launcher (no input.ini): changes are not saved."));
    }

    ImGui::SeparatorText(T("Mouse"));
    bool changed = false;
    changed |= ImGui::Checkbox(TI("Look around with the mouse"), &controls.mouse_look);
    Hint(T("The cursor stays in the game window; hold Left Alt (or switch windows) to free it."));
    const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
    // The sliders turn the camera at once (runtime_mouse_set) and are written when let go.
    if (ImGui::SliderFloat(TI("Mouse sensitivity"), &controls.sensitivity, 0.1f, 10.0f, "%.2f", log)) {
        runtime_mouse_set(controls.sensitivity, 0.0f, 0.0f);
    }
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::SliderFloat(TI("Aiming sensitivity"), &controls.aim, 0.25f, 4.0f, "%.2f", log)) {
        runtime_mouse_set(0.0f, controls.aim, 0.0f);
    }
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::SliderFloat(TI("Vertical sensitivity"), &controls.vertical, 0.25f, 4.0f, "%.2f", log)) {
        runtime_mouse_set(0.0f, 0.0f, controls.vertical);
    }
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::SliderFloat(TI("Camera up and down"), &controls.camera_vertical, 0.25f, 8.0f, "%.2f", log)) {
        runtime_mouse_camera_vertical_set(controls.camera_vertical);
    }
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    Hint(T("Aiming: times the mouse sensitivity while aiming (1.00 the same as looking around). Vertical: "
           "times it for up and down. Camera up and down: the normal camera's up and down only (it moves along "
           "the game's arc, slower than sideways). Ctrl+click to type a value."));
    changed |= ImGui::Checkbox(TI("Invert the mouse vertically"), &controls.invert_y);

    ImGui::SeparatorText(T("Keyboard and mouse buttons"));
    TextGrey(T("Click a box, then press a key or a mouse button (the wheel counts too); Esc cancels. "
               "x removes a binding. Each action can have two."));
    if (controls.capture_action >= 0 && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        controls.capture_action = -1; // (another window took the focus)
    }
    if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("##first", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##second", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        const float clear_w = ImGui::GetFrameHeight();
        for (int a = 0; a < KeyActionCount; ++a) {
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(KeyActions[a].title));
            for (int slot = 0; slot < 2; ++slot) {
                ImGui::TableNextColumn();
                ImGui::PushID(slot);
                const bool waiting = controls.capture_action == a && controls.capture_slot == slot;
                const std::string& key = controls.keys[a][slot];
                const float w = ImGui::GetContentRegionAvail().x - clear_w - ImGui::GetStyle().ItemSpacing.x;
                if (waiting) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.03f, 1.0f));
                }
                const std::string text = waiting ? std::string(T("Press a key…")) : key.empty() ? std::string("—") : key;
                if (ImGui::Button((text + "###bind").c_str(), ImVec2(std::max(w, 40.0f), 0.0f))) {
                    controls.capture_action = a;
                    controls.capture_slot = slot;
                    controls.capture_since_ms = SDL_GetTicks();
                    controls.message.clear();
                }
                if (waiting) {
                    ImGui::PopStyleColor(2);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(key.empty());
                if (ImGui::Button("x###clear", ImVec2(clear_w, 0.0f))) {
                    ControlsSetKey(a, slot, "");
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(TI("Default keys"))) {
        ControlsDefaults();
        controls.capture_action = -1;
        controls.message = T("Default keys");
        changed = true;
    }
    if (!controls.message.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", controls.message.c_str());
    }
    int walk = controls.walk_speed;
    if (ImGui::SliderInt(TI("Walk speed"), &walk, 50, 100, "%d%%")) {
        controls.walk_speed = walk;
    }
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    Hint(T("How far the walk key pushes the stick. Default is 65%; below about 55% the games do not move the "
           "character."));
    TextGrey(T("The controller's buttons, vibration and the button pictures: in the launcher's Controls page."));
    if (changed) {
        ControlsSave();
    }
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    // The window follows the screen: about 42% of its width (700 to 1000 at 1080p scale) and three quarters of its
    // height, the same for every tab (the Checkpoints and Save slots pages need the room); it can still be resized.
    const float menu_w = std::clamp(viewport->WorkSize.x * 0.42f, 700.0f * base_scale, 1000.0f * base_scale);
    const float menu_h = std::max(viewport->WorkSize.y * 0.75f - 40.0f * base_scale, 420.0f * base_scale);
    ImGui::SetNextWindowSize(ImVec2(std::min(menu_w, viewport->WorkSize.x - 80.0f * base_scale), menu_h),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin(TI("Uncharted — settings  (Insert / L3+R3)"), &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    UpdateGameStats();
    ImGui::Text(T("%.0f FPS  (%.1f ms)   1%% low %.0f"), game_fps,
                game_fps > 0.0f ? 1000.0f / game_fps : 0.0f, game_low_fps);
    Hint(T("Changes here apply at once, for this session. The launcher's settings apply each time "
           "the game starts: set the same there to keep a change."));

    ImGui::BeginTabBar("##pages");
    const bool settings_page = ImGui::BeginTabItem(TI("Settings"));
    if (settings_page) {
    ImGui::SeparatorText(T("Picture"));
    ImGui::Text(T("Rendering %s"), RenderSizeText());
    Hint(T("Set in the launcher (Display: Render resolution); it applies when the game starts. "
           "Quality, Balanced and Performance render smaller and let the anti-aliasing (DLSS or "
           "FSR) rebuild the full size: Uncharted 3 only."));
    static const char* upscalers[] = {N_("Off (the game's own)"), "FSR 3.1", "FSR 4 (INT8)",
                                     "FSR 4.1.1 (INT8)", "TAA", "DLSS (NVIDIA RTX)", "XeSS (Intel)"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(TI("Anti-aliasing"), T(upscalers[upscaler]))) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                : i == BbSettings::UpscalerXess   ? s.xess_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(T(upscalers[i]), i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T("— not supported by this graphics card"));
            }
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", T(problem));
    }
    if (const char* problem = s.xess_problem.load(); problem && upscaler == BbSettings::UpscalerXess) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "XeSS: %s", T(problem));
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), T("FSR 4 unavailable: %s"), T(problem));
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(T("The mode selected above is active. FSR 4 can be selected again."));
        ImGui::PopTextWrapPos();
    }
    Hint(T("Smooths edges and keeps the picture stable from frame to frame; with an upscaled render "
           "resolution it also rebuilds the full size. DLSS needs a GeForce RTX card."));
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    ImGui::BeginDisabled(!upscaler_on || !s.sharpen);
    Slider(TI("Sharpness"), s.sharpness, 0.0f, 2.0f);
    ImGui::EndDisabled();
    Hint(T("Sharpens the anti-aliased picture (AMD RCAS, also with DLSS). 0 is off; the launcher's "
           "Low, Medium and High are 0.2, 0.4 and 0.7. Ctrl+click to type a value."));

    ImGui::SeparatorText(T("Frame generation"));
    static const char* frame_gens[] = {N_("Off"), N_("FSR 3 (any graphics card)"),
                                       "DLSS (NVIDIA RTX 40/50)"};
    const int frame_gen = s.frame_gen;
    if (ImGui::BeginCombo(TI("Frame generation"), T(frame_gens[frame_gen]))) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            const bool supported = i != BbSettings::FrameGenDlss || s.dlssg_max_multiplier > 1;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(T(frame_gens[i]), i == frame_gen)) {
                Store(s.frame_gen, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T("— not supported by this graphics card"));
            }
        }
        ImGui::EndCombo();
    }
    const int max_multiplier = s.frame_gen == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier.load() : 2;
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff || max_multiplier <= 2);
    int multiplier = std::min(s.frame_gen_multiplier.load(), max_multiplier);
    if (ImGui::SliderInt(TI("Multiplier"), &multiplier, 2, std::max(max_multiplier, 2), "%dx")) {
        Store(s.frame_gen_multiplier, multiplier, true);
    }
    ImGui::EndDisabled();
    if (const char* problem = s.frame_gen_problem.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", T(problem));
    }
    Hint(T("Generated frames between the game's own: 60 FPS on screen becomes 120. Adds a little "
           "input latency; menus and cutscene videos are not generated. The launcher's Frame rate "
           "limit counts generated frames."));

    // The mouse's settings are on the Controls tab (saved in input.ini there).
    ImGui::SeparatorText(T("Other"));
    Checkbox(TI("FPS counter in the corner"), s.show_fps);
    ImGui::BeginDisabled(!s.show_fps);
    Checkbox(TI("Frame time graph"), s.show_fps_graph);
    ImGui::EndDisabled();
    Hint(T("Top line, the game's own frames: FPS and frame time, 1%L the average of the slowest "
           "1% (stutter), MAX the slowest frame (last 5 seconds). Below: the anti-aliasing, the "
           "render size (Q, B, P: Quality, Balanced, Performance) and with frame generation FG, "
           "the frames on screen. The graph: the game's frame times, a spike is a hitch."));

    // Diagnosis and tuning, kept out of the way: the defaults are right for the three games.
    if (ImGui::CollapsingHeader(TI("Developer options"))) {
        Checkbox(TI("Sub-pixel jitter"), s.jitter);
        Hint(T("Each frame the scene shifts by a fraction of a pixel and the anti-aliasing gathers "
               "detail from several frames. Off: smoothing from the history only."));
        Checkbox(TI("Sharpening pass"), s.sharpen);
        if (BbSettings::IsFsr4(s.upscaler)) {
            Checkbox(TI("FSR 4: auto exposure"), s.fsr4_auto_exposure);
            Checkbox(TI("FSR 4: inverted jitter sign"), s.fsr4_invert_jitter);
        }
        const bool taa = s.upscaler == BbSettings::UpscalerTaa;
        ImGui::BeginDisabled(taa || !upscaler_on);
        Checkbox(TI("Reactive mask"), s.reactive);
        Hint(T("Marks transparent effects (particles, haze) so the upscaler relies less on past "
               "frames: fewer trails behind effects, more shimmer under them."));
        ImGui::BeginDisabled(!s.reactive);
        Slider(TI("Mask scale"), s.reactive_scale, 0.0f, 4.0f);
        Slider(TI("Mask threshold"), s.reactive_threshold, 0.0f, 1.0f);
        Slider(TI("Mask maximum"), s.reactive_max, 0.0f, 1.0f);
        bool show_mask = s.debug_view == BbSettings::DebugReactive;
        if (ImGui::Checkbox(TI("Show the mask"), &show_mask)) {
            s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        Checkbox(TI("Character motion vectors"), s.object_motion);
        Hint(T("Exact vectors for animated objects (applies after restarting the game)."));
        bool show_motion = s.debug_view == BbSettings::DebugMotion;
        if (ImGui::Checkbox(TI("Show the motion vectors"), &show_motion)) {
            s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        }
        Hint(T("Red/green: horizontal/vertical motion (8 pixels = full brightness); blue: an exact "
               "object vector."));
    }
    if (s.object_motion != s.startup_object_motion) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "%s", T("Changes apply after restarting the game"));
        if (ImGui::Button(TI("Apply and restart the game"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }
    ImGui::EndTabItem();
    }
    {
        // the Controls tab: keyboard and mouse (also opened by the games' Options > Controls > Keyboard / Mouse)
        const ImGuiTabItemFlags flags = controls_tab_request.exchange(false) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(TI("Controls"), nullptr, flags)) {
            ControlsPage();
            ImGui::EndTabItem();
        } else if (controls.capture_action >= 0) {
            controls.capture_action = -1;
        }
    }
    // Checkpoints and Saves: Uncharted 1, 2 and 3 (the collection's menu has no campaign save).
    if (BbSaves::Game() && ImGui::BeginTabItem(TI("Checkpoints"))) {
        CheckpointsPage();
        ImGui::EndTabItem();
    }
    if (BbSaves::Game() && ImGui::BeginTabItem(TI("Saves"))) {
        SavesPage();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();

    ImGui::Spacing();
    if (ImGui::Button(TI("Close"))) {
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
    TextGrey(T("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel"));
    TextGrey(T("Controller: Cross (A) = OK, Circle (B) = cancel"));
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
    // Developer mode with Naughty Dog's debug text hidden (launcher, or F7 in play): "DEV MODE" on the top line,
    // green like the game's own text, so it is still plain that the mode is on.
    if (runtime_dev_text_shown() == 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), " DEV MODE");
    }
    // "DLSS 2288x960 Q  FG 2x 120": anti-aliasing, render size (Quality, Balanced,
    // Performance), and with frame generation adding frames (none in menus and movies) the
    // frames on screen.
    // Lighter than TextDisabled, which is hard to read over the game.
    const ImVec4 second_line(0.8f, 0.8f, 0.8f, 1.0f);
    // bbport: low latency mode: "Reflex" or "Anti-Lag 2" at the end while it is on.
    const char* latency = Vulkan::LowLatency::Name();
    const float shown_fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    if (s.frame_gen != BbSettings::FrameGenOff && shown_fps > game_fps * 1.2f) {
        ImGui::TextColored(second_line, "%s %s  FG %dx %.0f%s%s", CounterUpscalerName(s.upscaler),
                            ShortRenderText(),
                            s.frame_gen == BbSettings::FrameGenDlss ? s.frame_gen_multiplier.load()
                                                                    : 2,
                            shown_fps, latency ? "  " : "", latency ? latency : "");
    } else {
        ImGui::TextColored(second_line, "%s %s%s%s", CounterUpscalerName(s.upscaler), ShortRenderText(),
                           latency ? "  " : "", latency ? latency : "");
    }
    // "RAM 6.1/32 GB  VRAM 7.4/16 GB": the game's memory and video memory, once a second.
    UpdateMemory();
    if (ram_total_gib > 0.0 && vram_total_gib > 0.0 && memory_budget) {
        ImGui::TextColored(second_line, "RAM %.1f/%.0f GB  VRAM %.1f/%.0f GB", ram_gib, ram_total_gib,
                           vram_gib, vram_total_gib);
    } else if (ram_total_gib > 0.0) {
        ImGui::TextColored(second_line, "RAM %.1f/%.0f GB", ram_gib, ram_total_gib);
    }
    // "Compiles in play 3 (12 ms)  10 s: 1  bg 40": the shader and pipeline compiles since the shader preparation
    // finished, the ones the drawing thread did (each can hitch a frame; the 10 s figure counts the recent ones),
    // and bg those the draw-preparation workers did off it. Orange for 2 s after a new one on the drawing thread.
    if (PreparationFinished()) {
        using Clock = std::chrono::steady_clock;
        static bool based = false;
        static u32 base_compiles = 0, base_worker = 0, seen = 0;
        static u64 base_ns = 0;
        static Clock::time_point last_new{};
        static std::deque<std::pair<Clock::time_point, u32>> recent; // (time seen, count then)
        const u32 total = Vulkan::g_bb_compiles_total.load(std::memory_order_relaxed);
        const u32 worker_total = Vulkan::g_bb_worker_compiles_total.load(std::memory_order_relaxed);
        const u64 ns_total = Vulkan::g_bb_compile_ns_total.load(std::memory_order_relaxed);
        if (!based) {
            based = true;
            base_compiles = seen = total;
            base_worker = worker_total;
            base_ns = ns_total;
        }
        const auto now = Clock::now();
        if (total != seen) {
            seen = total;
            last_new = now;
        }
        recent.emplace_back(now, total);
        while (recent.size() > 1 && now - recent.front().first > std::chrono::seconds(10)) {
            recent.pop_front();
        }
        const u32 in_play = total - base_compiles;
        const u32 last_10s = total - recent.front().second;
        const bool fresh = in_play && now - last_new < std::chrono::seconds(2);
        ImGui::TextColored(fresh ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : second_line,
                           "Compiles in play %u (%.0f ms)  10 s: %u  bg %u", in_play,
                           double(ns_total - base_ns) / 1e6, last_10s, worker_total - base_worker);
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
               int& height, int max_width = ToastIconSize, int max_height = ToastIconSize) {
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
    if (color_type < 0 || width <= 0 || height <= 0 || width > max_width ||
        height > max_height || idat.empty()) {
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
    static const char* const names[] = {N_("Platinum trophy earned"), N_("Gold trophy earned"),
                                        N_("Silver trophy earned"), N_("Bronze trophy earned")};
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
    ImGui::TextColored(colors[grade], "%s", T(names[grade]));
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


// ---- The game's save list (sceSaveDataDialog list mode, runtime_services.c) -----------------------------------
// Ported from BilalReloaded's port (gow3_overlay.cpp ChoiceBox / HandleChoiceEvent, PR #349 by the-best-is-best;
// GPL-2.0-or-later), with the mouse added: hover highlights an entry, a click picks it, the wheel scrolls, the right
// button cancels; OK / Cancel / Delete buttons below. Keyboard: Up/Down (W/S), Enter or Space, Esc or Backspace,
// Delete. Controller: D-pad, Cross (A), Circle (B), Triangle (Y). A delete takes a second press (Delete, Triangle or
// the button) on the same entry. Guarded by imgui_mutex.
constexpr int ChoicePictureW = 456, ChoicePictureH = 256; // a save picture (bbport_thumbnail.h); icon0.png is smaller
std::string choice_title;
std::vector<std::string> choice_items;
std::vector<std::string> choice_sections; // a heading above an entry (empty = none)
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, <= -3 delete -3 - index, else the chosen index
int choice_deletable = -1, choice_delete_armed = -1; // entries from choice_deletable on can be deleted
bool choice_scroll = false;                           // the selection moved by key: scroll it into view
struct ChoicePicture {
    std::vector<unsigned char> rgba; // decoded, waiting for the present thread to upload
    int w = 0, h = 0;
    bool shown = false; // the entry has a picture
};
std::vector<ChoicePicture> choice_pictures;
std::vector<ImTextureData*> choice_textures; // kept and rewritten for later lists (never destroyed while running)
bool choice_uploads = false;
// The cursor moved since the list opened: until then ImGui's mouse position is an old one (or none) and a click
// does not pick (it would take whatever entry was under the cursor of an earlier menu).
bool choice_mouse_seen = false;

void ChoiceClose(int result) {
    choice_result = result;
    choice_active = false;
    choice_delete_armed = -1;
    if (!menu_open) {
        ImGui::GetIO().MouseDrawCursor = false;
    }
}

// Present thread: the decoded pictures into their textures (one per entry, made on first use).
void ChoiceUploadPictures() {
    if (!choice_uploads) {
        return;
    }
    choice_uploads = false;
    if (choice_textures.size() < choice_pictures.size()) {
        choice_textures.resize(choice_pictures.size(), nullptr);
    }
    for (size_t i = 0; i < choice_pictures.size(); ++i) {
        ChoicePicture& p = choice_pictures[i];
        if (p.rgba.empty()) {
            continue;
        }
        ImTextureData*& tex = choice_textures[i];
        const bool created = tex == nullptr;
        if (created) {
            tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, ChoicePictureW, ChoicePictureH);
            tex->UseColors = true;
        }
        std::memset(tex->Pixels, 0, size_t(ChoicePictureW) * ChoicePictureH * 4);
        for (int y = 0; y < p.h; ++y) {
            std::memcpy(tex->GetPixelsAt(0, y), &p.rgba[size_t(y) * p.w * 4], size_t(p.w) * 4);
        }
        if (created) {
            ImGui::RegisterUserTexture(tex);
        } else {
            tex->UpdateRect = {0, 0, u16(ChoicePictureW), u16(ChoicePictureH)};
            tex->Updates.push_back(tex->UpdateRect);
            tex->Status = ImTextureStatus_WantUpdates;
        }
        p.rgba.clear();
        p.rgba.shrink_to_fit();
    }
}

// The entry's label (translated when the list opens, ChoiceTranslate).
const char* ChoiceLabel(const std::string& item) {
    return item.c_str();
}
// The runtime's texts in the list (runtime_services.c, in English there): the headings, translated where shown (T
// leaves any other text, the game's own, as it is).
[[maybe_unused]] static const char* const ChoiceRuntimeTexts[] = {N_("Load game"), N_("Save game"),
                                                                  N_("Where to save"), N_("Undo")};
// An entry's name line from the runtime (its first line): "New save", "Game 2", "Game 2 - the one you are playing";
// the game's own lines after it stay as they are.
std::string ChoiceTranslate(const std::string& item) {
    const size_t end = item.find('\n');
    const std::string first = item.substr(0, end), rest = end == std::string::npos ? std::string() : item.substr(end);
    static const std::string playing = " - the one you are playing";
    if (first == "New save") {
        return T("New save") + rest;
    }
    if (first.rfind("Game ", 0) == 0) {
        const bool in_play = first.size() > playing.size() &&
                             first.compare(first.size() - playing.size(), playing.size(), playing) == 0;
        const std::string number = first.substr(5, first.size() - 5 - (in_play ? playing.size() : 0));
        if (!number.empty() && number.find_first_not_of("0123456789") == std::string::npos) {
            return fmt::format(fmt::runtime(in_play ? T("Game {} - the one you are playing") : T("Game {}")),
                               number) +
                   rest;
        }
    }
    return item;
}

void ChoiceDeletePress() {
    const int count = int(choice_items.size());
    if (choice_deletable < 0 || choice_index < choice_deletable || choice_index >= count) {
        return;
    }
    if (choice_delete_armed != choice_index) {
        choice_delete_armed = choice_index;
        return;
    }
    ChoiceClose(-3 - choice_index);
}

void ChoiceBox() {
    ChoiceUploadPictures();
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // As wide as its cards and help lines (UI scale: the same look at every resolution and on an ultrawide; a share of
    // the screen's width left room to the right of the cards there).
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(560.0f * s, viewport->WorkSize.x * 0.9f), 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * s, 14.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.45f, 0.28f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.08f, 0.07f, 1.0f));
    ImGui::Begin("##save_list", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.50f, 1.0f), "%s", T(choice_title.c_str()));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
    const int count = int(choice_items.size());
    bool pictures = false;
    for (int i = 0; i < count && i < int(choice_pictures.size()); ++i) {
        pictures = pictures || (choice_pictures[i].shown && i < int(choice_textures.size()) && choice_textures[i]);
    }
    const ImVec2 picture_size(228.0f * s, 128.0f * s);
    // The entries scroll in a child of their own height (up to what fits): the help below stays on screen.
    const ImGuiStyle& style = ImGui::GetStyle();
    float rows_height = 0.0f;
    for (size_t i = 0; i < choice_items.size(); ++i) {
        const std::string& item = choice_items[i];
        const float lines = float(1 + std::count(item.begin(), item.end(), '\n'));
        rows_height += std::max(pictures ? picture_size.y : 0.0f, lines * ImGui::GetTextLineHeight()) +
                       style.ItemSpacing.y;
        if (i < choice_sections.size() && !choice_sections[i].empty()) {
            rows_height += ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
        }
    }
    const float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f +
                                style.ItemSpacing.y * 3.0f;
    const float title_height = ImGui::GetCursorPosY();
    const float room = viewport->WorkSize.y * 0.8f - title_height - footer_height - style.WindowPadding.y * 2.0f;
    // The rows' own padding keeps every card's frame inside the scrolled area (drawn outside an entry, the selected
    // frame was cut off at the edges and on the first and last rows).
    const float card_pad = 8.0f * s, card_round = 6.0f * s;
    rows_height += count * (card_pad * 2.0f + 4.0f * s) + card_pad * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(card_pad, card_pad));
    ImGui::BeginChild("##save_list_rows",
                      ImVec2(0.0f, std::max(pictures ? picture_size.y : ImGui::GetTextLineHeight(),
                                            std::min(rows_height, room))),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    int clicked = -1;
    // Every entry is a card of the same width: the widest entry's, found on the frame before (cards that grow with
    // their text looked ragged).
    static float card_width = 0.0f;
    float widest = 0.0f;
    // ...and as wide as the help lines below (or the window's least width) when they are wider: the cards' right edge
    // meets the window's. From the texts, not the window: cards sized by the window would hold it at that width.
    {
        float help = std::min(560.0f * s, viewport->WorkSize.x * 0.9f) - 2.0f * 18.0f * s;
        for (const char* line : {"Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel",
                                 "Mouse: point to choose, click = OK, right button = cancel"}) {
            help = std::max(help, ImGui::CalcTextSize(T(line)).x);
        }
        card_width = std::max(card_width, help - 2.0f * card_pad);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; ++i) {
        const bool selected = i == choice_index;
        if (i < int(choice_sections.size()) && !choice_sections[i].empty()) {
            if (i) {
                ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.72f, 0.45f, 1.0f));
            ImGui::SeparatorText(T(choice_sections[i].c_str()));
            ImGui::PopStyleColor();
        }
        ImGui::PushID(i);
        // The card's background goes behind its contents (channel 0), drawn once its size is known.
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        const ImVec2 card_min = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(card_min.x + card_pad, card_min.y + card_pad));
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) && i < int(choice_pictures.size()) &&
                                     choice_pictures[i].shown
                                 ? choice_textures[i]
                                 : nullptr;
        if (pictures) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(picture_size);
            const ImVec2 box_max(at.x + picture_size.x, at.y + picture_size.y);
            // a dark box behind every picture (a smaller icon sits centred in it; no picture: the box says so)
            draw->AddRectFilled(at, box_max, IM_COL32(18, 16, 14, 255), 4.0f * s);
            if (tex) {
                const ChoicePicture& p = choice_pictures[i];
                const float fit = std::min(picture_size.x / float(p.w), picture_size.y / float(p.h));
                const ImVec2 size(float(p.w) * fit, float(p.h) * fit);
                const ImVec2 min(at.x + (picture_size.x - size.x) * 0.5f, at.y + (picture_size.y - size.y) * 0.5f);
                draw->AddImageRounded(tex->GetTexRef(), min, ImVec2(min.x + size.x, min.y + size.y), ImVec2(0.0f, 0.0f),
                                      ImVec2(float(p.w) / ChoicePictureW, float(p.h) / ChoicePictureH),
                                      IM_COL32_WHITE, 4.0f * s);
            } else {
                const char* none = T("No picture");
                const ImVec2 text = ImGui::CalcTextSize(none);
                draw->AddText(ImVec2(at.x + (picture_size.x - text.x) * 0.5f, at.y + (picture_size.y - text.y) * 0.5f),
                              IM_COL32(110, 104, 96, 255), none);
            }
            draw->AddRect(at, box_max, IM_COL32(70, 62, 50, 255), 4.0f * s, 0, 1.0f * s);
            ImGui::SameLine(0.0f, 12.0f * s);
        }
        // The first line is the entry's name (bright, gold when selected), the lines after it its details (dimmer).
        {
            const std::string label = ChoiceLabel(choice_items[i]);
            const size_t cut = label.find('\n');
            ImGui::BeginGroup();
            ImGui::TextColored(selected ? ImVec4(1.0f, 0.9f, 0.55f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s %s",
                               selected ? ">" : " ", T(label.substr(0, cut).c_str()));
            if (cut != std::string::npos) {
                // in line with the name (the group starts beside the picture, so no Indent)
                std::string details = "  " + label.substr(cut + 1);
                for (size_t at = details.find('\n'); at != std::string::npos; at = details.find('\n', at + 3)) {
                    details.insert(at + 1, "  ");
                }
                ImGui::TextColored(selected ? ImVec4(0.92f, 0.85f, 0.65f, 1.0f) : ImVec4(0.72f, 0.72f, 0.72f, 1.0f),
                                   "%s", details.c_str());
            }
            ImGui::EndGroup();
        }
        ImGui::EndGroup();
        // The card: the contents plus its padding, as wide as the widest card; the hover and click below use it.
        const ImVec2 content_max = ImGui::GetItemRectMax();
        widest = std::max(widest, content_max.x - card_min.x + card_pad);
        const ImVec2 card_max(card_min.x + std::max(card_width, content_max.x - card_min.x + card_pad),
                              content_max.y + card_pad);
        ImGui::SetCursorScreenPos(card_min);
        ImGui::InvisibleButton("##card", ImVec2(card_max.x - card_min.x, card_max.y - card_min.y));
        const bool card_hovered = ImGui::IsItemHovered();
        draw->ChannelsSetCurrent(0);
        const bool lit = selected || (choice_mouse_seen && card_hovered);
        draw->AddRectFilled(card_min, card_max,
                            selected ? IM_COL32(74, 60, 34, 235) : lit ? IM_COL32(50, 45, 38, 220)
                                                                      : IM_COL32(38, 34, 30, 200),
                            card_round);
        if (selected) {
            draw->AddRect(card_min, card_max, IM_COL32(255, 214, 120, 255), card_round, 0, 2.0f * s);
        } else {
            draw->AddRect(card_min, card_max, IM_COL32(64, 58, 50, 255), card_round, 0, 1.0f * s);
        }
        draw->ChannelsMerge();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s)); // the gap to the next card
        // The mouse: resting on an entry selects it (0.15 s: passing over entries on the way to the OK or Delete
        // button below leaves the selection alone), a click picks it at once.
        if (choice_mouse_seen && card_hovered) {
            // (once per arrival: keys that move the selection while the mouse rests keep it)
            static int hover_entry = -1;
            static double hover_since = 0.0;
            static bool hover_taken = false;
            const double now = ImGui::GetTime();
            if (hover_entry != i && mouse_moved) {
                hover_entry = i;
                hover_since = now;
                hover_taken = false;
            }
            if (hover_entry == i && !hover_taken && now - hover_since >= 0.15) {
                hover_taken = true;
                if (choice_index != i) {
                    choice_index = i;
                    choice_delete_armed = -1;
                }
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                clicked = i;
            }
        }
        if (selected && choice_scroll) {
            // the selected card into view (its middle)
            ImGui::SetScrollFromPosY(0.5f * (card_min.y + card_max.y) - ImGui::GetWindowPos().y, 0.5f);
            choice_scroll = false;
        }
        ImGui::PopID();
    }
    card_width = widest;
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
    const bool can_delete = choice_deletable >= 0 && choice_index >= choice_deletable && choice_index < count;
    const ImVec2 button(110.0f * s, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * s, 5.0f * s));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.53f, 0.24f, 1.0f));
    bool ok = ImGui::Button(T("OK"), button);
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("Cancel"), button);
    bool erase = false;
    if (can_delete) {
        ImGui::SameLine();
        const bool armed = choice_delete_armed == choice_index;
        if (armed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.14f, 1.0f));
        }
        erase = ImGui::Button(armed ? T("Delete: press again") : T("Delete"), ImVec2(armed ? 0.0f : button.x, 0.0f));
        if (armed) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
    TextGrey(T("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel"));
    TextGrey(T("Mouse: point to choose, click = OK, right button = cancel"));
    if (choice_delete_armed >= 0 && choice_delete_armed == choice_index) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           T("Press Delete or Triangle (Y) AGAIN to delete this save (it goes to savedata-deleted)"));
    } else if (can_delete) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.45f, 1.0f), "%s", T("Delete or Triangle (Y): delete this save"));
    } else if (choice_deletable >= 0) {
        TextGrey(T("Select a save, then Delete or Triangle (Y) to delete it"));
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (clicked >= 0) {
        choice_index = clicked;
        ok = true;
    }
    if (erase) {
        ChoiceDeletePress();
    } else if (ok && count) {
        ChoiceClose(choice_index);
    } else if (cancel || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ChoiceClose(-2);
    }
}

// Window thread: keys and buttons move and confirm; the mouse goes to ImGui (ChoiceBox reads it). The rest stays
// away from the game (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    ImGuiIO& io = ImGui::GetIO();
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    bool erase = false;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
        erase = key == SDLK_DELETE && !event.key.repeat;
        if (event.key.repeat && finish) {
            finish = 0; // a held Enter from the game's own menu does not answer the list
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
        erase = button == SDL_GAMEPAD_BUTTON_NORTH;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        choice_mouse_seen = true;
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
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
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
    const int count = int(choice_items.size());
    if (move || finish) {
        choice_delete_armed = -1;
    }
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
        choice_scroll = true;
    }
    if (erase) {
        ChoiceDeletePress();
        return true;
    }
    if (finish) {
        ChoiceClose(finish == 1 && count ? choice_index : -2);
    }
    return true;
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
    // Japanese: the kana and kanji DejaVu lacks come from the Noto Sans JP subset (ImGui 1.92 renders glyphs as they
    // are first drawn, so the atlas holds only what the menu shows).
    if (std::strcmp(BbI18n::Language(), "ja") == 0) {
        ImFontConfig ja_config;
        ja_config.FontDataOwnedByAtlas = false;
        ja_config.MergeMode = true;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ja_ttf),
                                       int(bb_font_ja_ttf_end - bb_font_ja_ttf), 18.0f, &ja_config);
    }

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
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    if (menu_open && ControlsCaptureEvent(event)) {
        return true;
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
std::atomic<bool> prep_screen{false};

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Step 1 has two parts on a full preparation: loading what the cache holds (phase 3), then building the listed
// shaders from the game files (phase 1). The bar gives them 0-5 % and 5-15 % (PreparationFraction).
static std::atomic<bool> prep_built{false};
// Between compiling and saving the phase is 0 for a moment: that is not the end (the bar would show 100 % and, as it
// never goes back, stay there while saving).
static std::atomic<bool> prep_saving_seen{false};

void SetShaderPreparation(int phase, u32 done, u32 total) {
    if (prep_phase.load(std::memory_order_relaxed) != phase) {
        prep_phase_start_ms.store(NowMs(), std::memory_order_relaxed);
    }
    if (phase == 1) {
        prep_built.store(true, std::memory_order_relaxed);
    }
    if (phase == 4) {
        prep_saving_seen.store(true, std::memory_order_relaxed);
    }
    prep_done.store(done, std::memory_order_relaxed);
    prep_total.store(total, std::memory_order_relaxed);
    prep_phase.store(phase, std::memory_order_release);
}

static std::atomic<u32> scan_done{0}, scan_total{0};
static std::atomic<s64> scan_start_ms{0};

void SetScanProgress(u32 done_mib, u32 total_mib) {
    if (total_mib && !scan_total.load(std::memory_order_relaxed)) {
        scan_start_ms.store(NowMs(), std::memory_order_relaxed);
    }
    scan_done.store(done_mib, std::memory_order_relaxed);
    scan_total.store(total_mib, std::memory_order_release);
}

/// "  about 3 min left" for the scan: its average rate so far (reading is steady, unlike pipeline builds).
static std::string ScanEta(u32 done, u32 total) {
    const s64 elapsed = NowMs() - scan_start_ms.load(std::memory_order_relaxed);
    if (done < 64 || total <= done || elapsed < 3000) {
        return {};
    }
    const double left_s = double(total - done) * double(elapsed) / double(done) / 1000.0;
    return "  " + (left_s >= 90.0 ? fmt::format(fmt::runtime(T("about {:.0f} min left")), left_s / 60.0)
                                  : fmt::format(fmt::runtime(T("about {:.0f} s left")), std::max(1.0, left_s)));
}

static const char* const ScanStep = N_("Step 1 of 2: finding the shaders in your game files (only this once)");
static const char* const ScanStep3 = N_("Step 1 of 3: finding the shaders in your game files (only this once)");

static std::atomic<bool> prep_rebuild{false};

void SetPreparationRebuild(bool on) {
    prep_rebuild.store(on, std::memory_order_release);
}

static std::atomic<float> prep_shown_fraction{0.0f}; // the bar's highest point on this page (PreparationFraction)

void SetPreparationScreen(bool on) {
    if (on) {
        prep_shown_fraction.store(0.0f, std::memory_order_relaxed); // a new page starts its bar empty
    }
    prep_screen.store(on, std::memory_order_release);
    runtime_freeze_guest(on ? 1 : 0); // the game's threads wait for the page (runtime.h; BB_PREP_FREEZE=0: they run)
}

bool PreparationScreenActive() {
    return prep_screen.load(std::memory_order_acquire);
}

static std::atomic<bool> prep_finished{false};

void SetPreparationFinished() {
    prep_finished.store(true, std::memory_order_release);
}

bool PreparationFinished() {
    return prep_finished.load(std::memory_order_acquire);
}

// One bar for the whole preparation: step 1 (loading or building shaders) fills the first 15 %, step 2 (the driver
// compiling pipelines, most of the time) the rest; between the steps it holds at 15 %.
// Without a window (BB_PREPARE_ONLY) the cache is written before the process ends: saving (phase 4) is the last 15 %.
// The page saves too on a full preparation in play (SetPreparationSaves): the same last 15 %.
static std::atomic<bool> page_saves{false};

void SetPreparationSaves(bool on) {
    page_saves.store(on, std::memory_order_relaxed);
}

/// Preparation in 3 steps (the third saves what was prepared): without a window, or a full preparation on the page.
/// The driver-rebuild page of a later launch saves nothing: 2 steps.
static bool PreparationSavesStep() {
    static const bool prepare_only = [] {
        const char* env = std::getenv("BB_PREPARE_ONLY");
        return env && env[0] == '1';
    }();
    return prepare_only || page_saves.load(std::memory_order_relaxed);
}

// The bar never goes back while the page is up (a phase starting over would empty it: on an empty cache, step 1's
// loading part filled 0-15 % in a moment and its building part began again at 0). prep_shown_fraction (declared
// above SetPreparationScreen) is reset when the page opens.

static float PreparationFraction(int phase, u32 done, u32 total, bool finishing, bool between) {
    const float step_part = total ? float(done) / float(total) : 0.0f;
    const bool full = PreparationSavesStep();
    const float compiled = full ? 0.85f : 1.0f;
    const float loaded = full ? 0.05f : 0.15f; // a full preparation builds after loading: 5-15 % for that
    const bool saving_ahead = full && !prep_saving_seen.load(std::memory_order_relaxed);
    const float fraction = phase == 4   ? 0.85f + 0.15f * step_part
                           : finishing  ? (saving_ahead ? compiled : 1.0f)
                           : phase == 2 ? 0.15f + (compiled - 0.15f) * step_part
                           : phase == 3 ? loaded * step_part
                           : phase == 1 ? (full ? 0.05f + 0.10f * step_part : 0.15f * step_part)
                           : between    ? (full && !prep_built.load(std::memory_order_relaxed) ? loaded : 0.15f)
                                        : 0.0f;
    float shown = prep_shown_fraction.load(std::memory_order_relaxed);
    while (fraction > shown && !prep_shown_fraction.compare_exchange_weak(shown, fraction)) {
    }
    return std::max(fraction, shown);
}

// "about 25 s left". Pipelines differ a lot in cost and finish in bursts on several threads, so the rate is taken over
// the last ~10 s (half weight) and the whole phase (half weight); the shown value is recomputed once a second, eases
// toward the estimate, counts down between updates and only rises slowly, so the countdown does not jump around.
// Called from the overlay's draw (one thread).
std::string PreparationEta(u32 done, u32 total) {
    static s64 phase_start = -1, last_update = 0;
    static double shown_s = -1.0;
    static std::array<std::pair<s64, u32>, 24> samples{};  // (ms, done) every 0.5 s
    static size_t next_sample = 0, num_samples = 0;
    const s64 now = NowMs();
    const s64 start = prep_phase_start_ms.load(std::memory_order_relaxed);
    if (start != phase_start) {  // a new phase: start over
        phase_start = start;
        shown_s = -1.0;
        num_samples = next_sample = 0;
        last_update = 0;
    }
    const s64 elapsed = now - start;
    if (done < 8 || total <= done || elapsed < 1500) {
        return {};
    }
    const auto& newest = samples[(next_sample + samples.size() - 1) % samples.size()];
    if (num_samples == 0 || now - newest.first >= 500) {
        samples[next_sample] = {now, done};
        next_sample = (next_sample + 1) % samples.size();
        num_samples = std::min(num_samples + 1, samples.size());
    }
    if (now - last_update >= 1000 || shown_s < 0.0) {
        const double overall = double(done) / double(elapsed);  // per ms
        const auto& oldest = samples[num_samples < samples.size() ? 0 : next_sample];
        const double recent = now > oldest.first && done > oldest.second
                                  ? double(done - oldest.second) / double(now - oldest.first)
                                  : overall;
        const double estimate_s = double(total - done) / (0.5 * overall + 0.5 * recent) / 1000.0;
        if (shown_s < 0.0) {
            shown_s = estimate_s;
        } else {
            const double counted = std::max(0.0, shown_s - double(now - last_update) / 1000.0);
            // ease toward the estimate; going up only a little per update
            shown_s = estimate_s < counted ? counted + 0.3 * (estimate_s - counted)
                                           : counted + std::min(1.0, 0.3 * (estimate_s - counted));
        }
        last_update = now;
    }
    const s64 left_s = s64(shown_s + 0.5);
    if (left_s < 5) {
        return std::string("  ") + T("a few seconds left");
    }
    if (left_s >= 60) {
        const s64 rounded = (left_s + 4) / 5 * 5;
        return "  " + fmt::format(fmt::runtime(T("about {} min {} s left")), rounded / 60, rounded % 60);
    }
    return "  " + fmt::format(fmt::runtime(T("about {} s left")), left_s);
}

// The full-screen page shown before the game starts (SetPreparationScreen): what is happening, that it happens once,
// a progress bar with the step, the count and the countdown. Centred on a dark background.
void FullScreenPreparation() {
    // The launcher's bar follows the page: PREP lines 4 times a second while it is up (BB_PREPARE_ONLY prints its own).
    static s64 last_prep_line = 0;
    if (const s64 now = NowMs(); now - last_prep_line >= 250) {
        last_prep_line = now;
        PrintPreparationProgress();
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.62f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.17f, 0.19f, 1.0f));
    ImGui::Begin("##prep_screen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float w = viewport->Size.x;
    const float h = viewport->Size.y;
    const auto centered = [&](const char* text, float scale, ImVec4 color) {
        ImGui::SetWindowFontScale(scale);
        const float tw = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX((w - tw) * 0.5f);
        ImGui::TextColored(color, "%s", text);
    };
    const ImVec4 white{0.95f, 0.95f, 0.95f, 1.0f};
    const ImVec4 grey{0.62f, 0.64f, 0.67f, 1.0f};
    const int phase = prep_phase.load(std::memory_order_acquire);
    const u32 done = prep_done.load(std::memory_order_relaxed);
    const u32 total = prep_total.load(std::memory_order_relaxed);
    // After the last step the phase goes back to 0 while the page is still up: show it finished, not "Starting".
    static bool had_step = false, seen_step2 = false;
    had_step = had_step || phase != 0;
    seen_step2 = seen_step2 || phase == 2;
    const bool finishing = phase == 0 && seen_step2;
    const bool between = phase == 0 && had_step && !seen_step2;  // step 1 done, step 2 not started yet

    ImGui::SetCursorPosY(h * 0.36f);
    centered(T("Preparing shaders"), 2.4f, white);
    ImGui::Dummy(ImVec2(0.0f, 8.0f * base_scale));
    centered(prep_rebuild.load(std::memory_order_acquire)
                 ? T("Your graphics driver is compiling the shaders again (after a driver update or when its "
                     "cache was cleared). The game continues when this is done.")
                 : T("This happens once, before your first launch. Later launches start straight away."),
             1.15f, grey);
    ImGui::Dummy(ImVec2(0.0f, 28.0f * base_scale));

    const bool three = PreparationSavesStep();
    const char* step = phase == 4 ? T("Step 3 of 3: saving the prepared shaders")
                       : phase == 3 ? (three ? T("Step 1 of 3: loading shaders") : T("Step 1 of 2: loading shaders"))
                       : phase == 1 ? (three ? T("Step 1 of 3: building shaders from your game files")
                                             : T("Step 1 of 2: building shaders from your game files"))
                       : phase == 2 || between ? (three ? T("Step 2 of 3: compiling them for your graphics card")
                                                        : T("Step 2 of 2: compiling them for your graphics card"))
                       : finishing ? T("Finishing...")
                                   : T("Starting...");
    const u32 scan_of = scan_total.load(std::memory_order_acquire);
    const u32 scan_at = std::min(scan_done.load(std::memory_order_relaxed), scan_of);
    centered(scan_of ? (three ? T(ScanStep3) : T(ScanStep)) : step, 1.2f, white);
    ImGui::Dummy(ImVec2(0.0f, 10.0f * base_scale));
    const float bar_w = std::min(w * 0.55f, 900.0f * base_scale);
    ImGui::SetCursorPosX((w - bar_w) * 0.5f);
    const float fraction = scan_of ? float(scan_at) / float(scan_of)  // the scan fills the bar on its own
                                   : PreparationFraction(phase, done, total, finishing, between);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::ProgressBar(fraction, ImVec2(bar_w, 22.0f * base_scale), "");
    ImGui::Dummy(ImVec2(0.0f, 8.0f * base_scale));
    // the countdown only in step 2: before it the remaining time cannot be told
    const std::string count =
        scan_of ? fmt::format("{:.1f} / {:.1f} GB{}", scan_at / 1024.0, scan_of / 1024.0, ScanEta(scan_at, scan_of))
        : !finishing && total
            ? fmt::format("{} / {}{}", done, total, phase == 2 ? PreparationEta(done, total) : std::string{})
            : std::string{};
    centered(count.c_str(), 1.1f, grey);
    // Where it is switched off (the launcher's Advanced page). For now also the way out if a large shader cache
    // causes trouble (temporary, while shader preparation is being polished).
    ImGui::SetCursorPosY(h * 0.86f);
    centered(T("If a large shader cache causes problems: in the launcher, clear the shader cache and turn off"),
             1.0f, grey);
    centered(T("Advanced > Shader preparation."), 1.0f, grey);
    ImGui::End();
    ImGui::PopStyleColor(3);
}

// "PREP 37 1240 4496 2|  about 25 s left": the launcher's bar while preparing without a window (one thread, the
// page's PreparationEta is not drawn then).
void PrintPreparationProgress() {
    if (const u32 total = scan_total.load(std::memory_order_acquire)) {
        const u32 done = std::min(scan_done.load(std::memory_order_relaxed), total);
        // The 6th number: how many steps this preparation has (3 when it saves at the end, 2 when not).
        std::printf("PREP %d %u %u 5 %d|%s\n", int(100.0f * float(done) / float(total)), done, total,
                    PreparationSavesStep() ? 3 : 2, ScanEta(done, total).c_str());
        std::fflush(stdout);
        return;
    }
    const int phase = prep_phase.load(std::memory_order_acquire);
    const u32 done = prep_done.load(std::memory_order_relaxed);
    const u32 total = prep_total.load(std::memory_order_relaxed);
    static bool had_step = false, seen_step2 = false;
    had_step = had_step || phase != 0;
    seen_step2 = seen_step2 || phase == 2;
    const bool finishing = PreparationFinished() || (phase == 0 && seen_step2);
    const bool between = phase == 0 && had_step && !seen_step2;
    const int step = phase == 4 ? 4 : finishing ? 3 : phase == 2 || between ? 2 : had_step ? 1 : 0;
    const std::string eta = phase == 2 && total ? PreparationEta(done, total) : std::string{};
    std::printf("PREP %d %u %u %d %d|%s\n",
                int(100.0f * PreparationFraction(phase, done, total, finishing, between)), done, total, step,
                PreparationSavesStep() ? 3 : 2, eta.c_str());
    std::fflush(stdout);
}

// "Preparing shaders 1240 / 4496": bottom left while the variant list is built behind the game.
void ShaderPreparation() {
    if (prep_screen.load(std::memory_order_acquire)) {
        FullScreenPreparation();
        return;
    }
    const int phase = prep_phase.load(std::memory_order_acquire);
    const u32 scan_of = scan_total.load(std::memory_order_acquire);
    // A start with a warm cache prepares for a second or so while the game waits: no box for that. It shows when the
    // work lasts longer (the one-time file scan shows at once).
    static s64 busy_since_ms = 0;
    if (phase == 0 && scan_of == 0) {
        busy_since_ms = 0;
        return;
    }
    if (busy_since_ms == 0) {
        busy_since_ms = NowMs();
    }
    if (scan_of == 0 && NowMs() - busy_since_ms < 1500) {
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
    if (scan_of) {
        const u32 scan_at = std::min(scan_done.load(std::memory_order_relaxed), scan_of);
        ImGui::Text(T("Finding the shaders in your game files (only this once) %.1f / %.1f GB%s"), scan_at / 1024.0,
                    scan_of / 1024.0, ScanEta(scan_at, scan_of).c_str());
    } else if (total == 0) {
        ImGui::TextUnformatted(phase == 1 ? T("Preparing shaders") : T("Preparing pipelines"));
    } else {
        ImGui::Text(phase == 1 ? T("Preparing shaders %u / %u%s") : T("Preparing pipelines %u / %u%s"), done, total,
                    PreparationEta(done, total).c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || choice_active || BbSettings::Get().show_fps ||
                           toast_busy || prep_phase.load(std::memory_order_relaxed) != 0 ||
                           scan_total.load(std::memory_order_relaxed) != 0 ||
                           prep_screen.load(std::memory_order_relaxed));
}

void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char> png) {
    std::scoped_lock lock{toast_mutex};
    if (toast_queue.size() < 32) {
        toast_queue.push_back({grade, std::move(game), std::move(name), std::move(png)});
        toast_busy = true;
    }
}

void OpenControls() {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return;
    }
    controls_tab_request = true;
    SetOpen(true);
}

bool DrawsCursor() {
    return initialized && (menu_open || choice_active);
}

bool CapturesInput() {
    // the preparation page covers the game: its input is held so the title screen cannot be passed unseen
    return menu_open || text_entry_active || choice_active || prep_screen.load(std::memory_order_relaxed);
}

bool BeginChoice(const std::string& title, std::vector<std::string> items,
                 std::vector<std::vector<unsigned char>> pngs, std::vector<std::string> sections, int focus,
                 int deletable_from) {
    sections.resize(items.size());
    // The pictures are decoded on the caller's thread; the present thread uploads them.
    std::vector<ChoicePicture> pictures(items.size());
    for (size_t i = 0; i < items.size() && i < pngs.size(); ++i) {
        ChoicePicture& p = pictures[i];
        if (!pngs[i].empty() && DecodePng(pngs[i], p.rgba, p.w, p.h, ChoicePictureW, ChoicePictureH)) {
            p.shown = true;
        } else {
            p.rgba.clear();
        }
    }
    std::scoped_lock lock{imgui_mutex};
    if (!initialized || items.empty()) {
        return false;
    }
    choice_pictures = std::move(pictures);
    choice_uploads = true;
    choice_title = title;
    choice_items = std::move(items);
    for (std::string& item : choice_items) {
        item = ChoiceTranslate(item);
    }
    choice_sections = std::move(sections);
    const int count = int(choice_items.size());
    choice_index = focus >= 0 && focus < count ? focus : 0;
    choice_deletable = deletable_from >= 0 && deletable_from < count ? deletable_from : -1;
    choice_delete_armed = -1;
    choice_scroll = true;
    choice_result = -1;
    // A new list waits for the cursor to move before the mouse picks (ImGui's position is an old one).
    if (!choice_active) {
        choice_mouse_seen = false;
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        // ...and with no button held: a release that went to the game (the right button whose press closed the
        // last list) would leave ImGui's button down, and the next click would not count as one.
        for (int button = 0; button < 3; ++button) {
            ImGui::GetIO().AddMouseButtonEvent(button, false);
        }
    }
    choice_active = true;
    ImGui::GetIO().MouseDrawCursor = true;
    return true;
}

int PollChoice() {
    return choice_result.load();
}

void CancelChoice() {
    std::scoped_lock lock{imgui_mutex};
    if (choice_active) {
        ChoiceClose(-2);
    }
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
    if (choice_active && !menu_open) {
        ChoiceBox();
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
        // Texture uploads (new font glyphs, save pictures, trophy icons) submit to the graphics queue themselves and
        // wait for it: the queue's own lock too, as the async submits and sparse binds take only that one
        // (vk_scheduler.cpp). Without it an upload could run at the same moment as one of them and be lost: glyphs
        // of the font, once drawn, stayed blank for the session (letters missing in the save list), a picture
        // black. Same order as Scheduler::SubmitExecution (submit lock, then queue lock).
        ImDrawData* draw_data = ImGui::GetDrawData();
        bool uploads = false;
        if (draw_data && draw_data->Textures) {
            for (const ImTextureData* tex : *draw_data->Textures) {
                uploads = uploads || tex->Status != ImTextureStatus_OK;
            }
        }
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        std::unique_lock<std::mutex> queue_lock;
        if (uploads) {
            queue_lock = std::unique_lock{Vulkan::g_graphics_queue_mutex};
        }
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
