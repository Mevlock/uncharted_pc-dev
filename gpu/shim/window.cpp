// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

namespace {
// bbport: mouse input for runtime_pad.c (keyboard and mouse controls). The window thread adds
// the motion, buttons and wheel of its events; the pad sampling takes them (bbgpu_mouse_sample).
std::mutex mouse_mutex;
float mouse_dx{}, mouse_dy{};
// The mouse's movement for the camera hooks (src/runtime_mouselook.c, after BilalReloaded's port): a
// second sum of the same motion that a camera hook takes when the game updates its camera
// (bbgpu_mouse_take_look), independent of the pad read; look_taken_ns: the last take.
float look_dx{}, look_dy{};
u64 look_taken_ns = 0;
int mouse_wheel{};
u32 mouse_buttons{};
std::atomic<bool> mouse_look{false}, mouse_captured{false};
// A game menu is on screen (bbgpu_menu_cursor, the menu mouse by antikmewzik): the cursor is free and shown;
// cursor_state: its look (0 arrow, 1 hand, 2 pressed), applied by the window thread.
std::atomic<bool> menu_cursor{false};
std::atomic<int> cursor_state{0};
bool window_focused{true};
// Without mouse look (controller play) the cursor was left showing over the game: it hides after
// two seconds without mouse movement and comes back when the mouse moves (the menu shows it too).
constexpr u64 CursorIdleNs = 2'000'000'000ull;
u64 cursor_moved_ns = 0;
bool cursor_hidden = false;
// The game window is in front (Windows' own answer, set each event poll): the controller is read
// only then (runtime_pad.c, bbgpu_window_foreground). SDL's keyboard focus can be lost while the
// window stays in front, and with it SDL dropped the gamepad events (from BilalReloaded's port).
std::atomic<bool> window_foreground{true};
// Alt+Enter switched between window and fullscreen: its Enter is kept from the game until released
// (Enter is a default key for Options).
std::atomic<bool> enter_swallowed{false};
// Alt+Enter switches only between shapes the game was set up for: the ultrawide field of view and the
// native render size are taken once at start (runtime_ultrawide.c: the screen in fullscreen, else
// BB_WINDOW_SIZE), so another shape would be stretched or pillarboxed. start_aspect: that shape;
// windowed_w/h: the window's own size (as created, or the last size while not fullscreen).
double start_aspect = 0.0;
int windowed_w = 0, windowed_h = 0;

double DesktopAspect(SDL_Window* window) {
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
    return mode && mode->w > 0 && mode->h > 0 ? double(mode->w) / double(mode->h) : 0.0;
}

u32 MouseBit(u8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return 1u;
    case SDL_BUTTON_RIGHT: return 2u;
    case SDL_BUTTON_MIDDLE: return 4u;
    case SDL_BUTTON_X1: return 8u;
    case SDL_BUTTON_X2: return 16u;
    default: return 0u;
    }
}

#include "bbport_cursor_data.inc"

// The golden arrow of the game menus, by antikmewzik (his window.cpp BuildMenuCursors / UpdateMenuCursor, branch
// u2-mkb-qol; GPL-2.0-or-later, as this port): one 96x96 master picture per state (0 normal, 1 over something that
// answers a click, 2 pressed), box-filtered to about 26 px high at 1080p (16..128) and made SDL colour cursors, built
// again when the window's height changes. Shown while a game menu is on screen and the overlay does not have the input
// (its own cursor is ImGui's); the system arrow and hand when they cannot be made. BB_MENU_CURSOR=0: the system ones.
SDL_Cursor* golden_cursors[3]{};
int golden_size = 0;
bool golden_failed = false;

bool GoldenCursorsOn() {
    static const bool on = [] {
        const char* e = std::getenv("BB_MENU_CURSOR");
        return !(e && e[0] == '0');
    }();
    return on && !golden_failed;
}

void FreeGoldenCursors() {
    for (SDL_Cursor*& c : golden_cursors) {
        if (c) {
            SDL_DestroyCursor(c);
            c = nullptr;
        }
    }
}

// Every state built at `size` px, or none (false).
bool BuildGoldenCursors(int size) {
    FreeGoldenCursors();
    golden_size = size;
    constexpr int master = CursorMasterSize;
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4);
    for (int s = 0; s < 3; ++s) {
        // box filter on premultiplied colours (the picture is larger than the cursor)
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int x0 = x * master / size, x1 = std::max(x0 + 1, (x + 1) * master / size);
                const int y0 = y * master / size, y1 = std::max(y0 + 1, (y + 1) * master / size);
                float r = 0, g = 0, b = 0, a = 0;
                int n = 0;
                for (int yy = y0; yy < y1 && yy < master; ++yy) {
                    for (int xx = x0; xx < x1 && xx < master; ++xx) {
                        const unsigned char* p = &CursorPixels[s][(yy * master + xx) * 4];
                        const float pa = p[3] / 255.0f;
                        r += p[0] * pa;
                        g += p[1] * pa;
                        b += p[2] * pa;
                        a += pa;
                        ++n;
                    }
                }
                unsigned char* q = &pixels[(static_cast<size_t>(y) * size + x) * 4];
                if (a > 0.0f && n > 0) {
                    q[0] = static_cast<unsigned char>(std::min(255.0f, r / a));
                    q[1] = static_cast<unsigned char>(std::min(255.0f, g / a));
                    q[2] = static_cast<unsigned char>(std::min(255.0f, b / a));
                    q[3] = static_cast<unsigned char>(std::min(255.0f, a / n * 255.0f));
                } else {
                    q[0] = q[1] = q[2] = q[3] = 0;
                }
            }
        }
        SDL_Surface* surface =
            SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32, pixels.data(), size * 4);
        if (surface) {
            golden_cursors[s] = SDL_CreateColorCursor(surface, CursorHotX * size / master,
                                                      CursorHotY * size / master);
            SDL_DestroySurface(surface);
        }
        if (!golden_cursors[s]) {
            std::printf("Window: the golden menu cursor could not be made (%s): system cursors\n", SDL_GetError());
            FreeGoldenCursors();
            return false;
        }
    }
    std::printf("Window: golden menu cursor built at %d px (antikmewzik's; BB_MENU_CURSOR=0: system cursors)\n", size);
    return true;
}
} // namespace

extern "C" void bbgpu_mouse_configure(int look) {
    mouse_look = look != 0;
}

extern "C" void bbgpu_menu_cursor(int open) {
    menu_cursor = open != 0;
}

extern "C" void bbgpu_set_cursor(int state) {
    cursor_state = state;
}

// bbport (src/runtime_mouselook.c, from BilalReloaded's port, gow3_mouse.cpp gow3gpu_mouse_take_look):
// the motion since the last take, for a camera hook taking it when the game updates the camera.
// Motion older than 250 ms (no camera hook ran meanwhile) is dropped: it would turn the camera all at
// once when a hook runs again. 1 while the cursor is captured.
extern "C" int bbgpu_mouse_take_look(float* dx, float* dy) {
    std::scoped_lock lock{mouse_mutex};
    const u64 now = SDL_GetTicksNS();
    const bool fresh = look_taken_ns && now - look_taken_ns < 250'000'000ull;
    look_taken_ns = now;
    *dx = fresh ? look_dx : 0.0f;
    *dy = fresh ? look_dy : 0.0f;
    look_dx = look_dy = 0.0f;
    return mouse_captured ? 1 : 0;
}

extern "C" int bbgpu_mouse_sample(float* dx, float* dy, uint32_t* buttons, int* wheel) {
    std::scoped_lock lock{mouse_mutex};
    *dx = mouse_dx;
    *dy = mouse_dy;
    *buttons = mouse_buttons;
    *wheel = mouse_wheel;
    mouse_dx = mouse_dy = 0.0f;
    mouse_wheel = 0;
    return mouse_captured ? 1 : 0;
}

#ifdef _WIN32
// Declared here: windows.h clashes with names used by this file's headers.
extern "C" __declspec(dllimport) void* __stdcall GetForegroundWindow();
extern "C" __declspec(dllimport) void* __stdcall GetAncestor(void* hwnd, unsigned int flags);
#endif

extern "C" int bbgpu_window_foreground(void) {
    return window_foreground.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int bbgpu_window_enter_swallowed(void) {
    return enter_swallowed.load(std::memory_order_relaxed) ? 1 : 0;
}

// bbport (from El Dorado 2bc14455; an option, off by default: BB_PAUSE_ON_FOCUS_LOST=1, launcher "Pause in the
// background"): the window minimized or out of focus stops the game where it is (its threads and clocks, as the
// shader preparation page does: runtime_freeze_reason). Back, it goes on from the same moment and, when it was in
// gameplay as it went (the renderer's scene detection: not a menu, movie or loading screen), the game's pause menu
// opens (runtime_pad.c holds Options once: bbgpu_take_back_request).
extern "C" void runtime_freeze_reason(unsigned reason, int on, const char* why);
namespace Vulkan {
extern std::atomic<bool> g_last_frame_scene; // vk_rasterizer.cpp TakeSceneFrame
}
static std::atomic<int> back_request{0};

// The menu mouse (runtime_menumouse.c): a widened gameplay frame fills the window (its 16:9 layout stretched across it, so
// the pause menu over the play is too); other frames are shown at 16:9 in the middle.
extern "C" int runtime_ultrawide_fill;
extern "C" int bbgpu_frame_fills_window(void) {
    return runtime_ultrawide_fill && Vulkan::g_last_frame_scene.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int bbgpu_take_back_request(void) {
    return back_request.exchange(0, std::memory_order_acq_rel);
}

static void BackgroundPause(bool away) {
    static const bool on = [] {
        const char* env = std::getenv("BB_PAUSE_ON_FOCUS_LOST");
        return env && env[0] == '1';
    }();
    static bool frozen = false, was_gameplay = false;
    if (!on || away == frozen) {
        return;
    }
    if (away) {
        was_gameplay = Vulkan::g_last_frame_scene.load(std::memory_order_relaxed);
    }
    frozen = away;
    runtime_freeze_reason(2, away ? 1 : 0, "the game's window is in the background");
    if (!away && was_gameplay) {
        back_request.store(1, std::memory_order_release);
    }
}

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    // Their events come whatever SDL thinks of the keyboard focus; whether the game is in front
    // is decided from Windows instead (window_foreground). BB_PAD_BACKGROUND=0: as before;
    // BB_PAD_BACKGROUND=1: the game also takes them while another window is in front (runtime_pad.c).
    if (const char* env = std::getenv("BB_PAD_BACKGROUND"); !(env && env[0] == '0')) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    // Controllers are updated on the runtime's pad thread (runtime_pad.c): a rescan when one is switched off or on held
    // up this window's event pump, and presenting with it (0.3.6.44). BB_PAD_THREAD=0: the pump updates them.
    if (const char* env = std::getenv("BB_PAD_THREAD"); !(env && env[0] == '0')) {
        SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    windowed_w = width_;
    windowed_h = height_;
    start_aspect = fullscreen && fullscreen[0] == '1' ? DesktopAspect(window)
                                                      : double(width_) / double(height_ > 0 ? height_ : 1);
    // bbport: a game started from the collection menu (run.py sets BB_RAISE_WINDOW) opened behind
    // other windows, as the menu's process had ended: no input until the window was clicked.
    if (const char* raise = std::getenv("BB_RAISE_WINDOW"); raise && raise[0] == '1') {
        SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
        SDL_RaiseWindow(window);
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
#ifdef _WIN32
    {
        // In front: the game window, a window it owns (an overlay in exclusive fullscreen), or
        // SDL's keyboard focus on it.
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        void* foreground = GetForegroundWindow();
        constexpr unsigned int GA_ROOTOWNER = 3;
        window_foreground = !hwnd || foreground == hwnd ||
                            (foreground && GetAncestor(foreground, GA_ROOTOWNER) == hwnd) ||
                            SDL_GetKeyboardFocus() == window;
    }
#endif
    // Mouse look holds the cursor in the window (relative mode) while the game has the focus;
    // the settings menu, text entry and a held Left Alt give it back.
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const bool want = mouse_look && window_focused && !text_active && !menu_cursor &&
                          !BbOverlay::CapturesInput() && !(keys && keys[SDL_SCANCODE_LALT]);
        if (want != mouse_captured) {
            if (want) {
                // Raw, linear counts for mouse look (as BilalReloaded's port, gow3_mouse.cpp): no system
                // acceleration curve or scale, whatever a user's SDL_* environment variables say.
                SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0", SDL_HINT_OVERRIDE);
                SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1", SDL_HINT_OVERRIDE);
                SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, "0", SDL_HINT_OVERRIDE);
            }
            SDL_SetWindowRelativeMouseMode(window, want);
            mouse_captured = want;
            std::scoped_lock lock{mouse_mutex};
            mouse_dx = mouse_dy = 0.0f;
            look_dx = look_dy = 0.0f;
            mouse_buttons = 0;
        }
        // Mouse look's relative mode hides the cursor itself; otherwise an idle cursor is hidden
        // while the game has the focus and nothing needs it (menu, text entry).
        // While the overlay draws its own cursor (ImGui's), the system one is hidden: one cursor, also in exclusive
        // fullscreen; it comes back with the rules above once the overlay closes.
        const bool overlay_cursor = BbOverlay::DrawsCursor();
        const bool hide = overlay_cursor || (window_focused && !text_active && !BbOverlay::CapturesInput() &&
                                             SDL_GetTicksNS() - cursor_moved_ns > CursorIdleNs);
        // In a game menu the cursor shows a hand over what answers a click (the menu mouse): antikmewzik's golden
        // arrow (normal, hover, pressed), or the system arrow and hand.
        {
            static SDL_Cursor* arrow = nullptr;
            static SDL_Cursor* hand = nullptr;
            static int shown = -1; // 0 arrow, 1 hand, 10 + n golden n
            const int state = menu_cursor ? std::clamp(cursor_state.load(), 0, 2) : 0;
            int want_shape = state == 0 ? 0 : 1;
            if (menu_cursor && GoldenCursorsOn() && !BbOverlay::CapturesInput()) {
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                const int size = std::clamp(static_cast<int>(h * 26.0f / 1080.0f + 0.5f), 16, 128);
                if (size != golden_size || !golden_cursors[0]) {
                    if (shown >= 10) { // (the one shown is about to be freed)
                        SDL_SetCursor(SDL_GetDefaultCursor());
                        shown = -1;
                    }
                    golden_failed = !BuildGoldenCursors(size);
                }
                if (!golden_failed) {
                    want_shape = 10 + state;
                }
            }
            if (want_shape != shown) {
                if (!arrow) {
                    arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
                    hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
                }
                if (SDL_Cursor* c = want_shape >= 10 ? golden_cursors[want_shape - 10] : want_shape ? hand : arrow) {
                    SDL_SetCursor(c);
                }
                shown = want_shape;
            }
        }
        if (!mouse_captured && hide != cursor_hidden) {
            if (hide) {
                SDL_HideCursor();
            } else {
                SDL_ShowCursor();
            }
            cursor_hidden = hide;
        }
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION && !mouse_captured) {
            cursor_moved_ns = SDL_GetTicksNS();
            if (cursor_hidden && !BbOverlay::DrawsCursor()) { // back at once, not at the next poll
                SDL_ShowCursor();
                cursor_hidden = false;
            }
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // Alt+Enter: window <-> fullscreen while playing (the launcher's Fullscreen is SDL's borderless
        // fullscreen on the desktop's mode, as here). The swapchain follows the size change through the
        // resize events below (the presenter recreates it when the window size differs). Leaving
        // fullscreen gives the window its own size back (BB_WINDOW_SIZE as the game started, or the
        // last window size). The Enter is kept from the game (Options) until it is released.
        // Only when the screen and the window have the same shape (within 1%): the game's field of
        // view and render size stay as they were set up at start (start_aspect above).
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) && (event.key.mod & SDL_KMOD_ALT)) {
            const bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
            const double target = full ? double(windowed_w) / double(windowed_h > 0 ? windowed_h : 1)
                                       : DesktopAspect(window);
            if (start_aspect <= 0.0 || target <= 0.0 || std::abs(target / start_aspect - 1.0) > 0.01) {
                LOG_WARNING(Frontend,
                            "Alt+Enter: window and screen shapes differ ({:.3f} set up at start, {:.3f} for {}); "
                            "not switched, change it in the launcher",
                            start_aspect, target, full ? "the window" : "fullscreen");
                std::printf("Alt+Enter: window and screen shapes differ; change it in the launcher\n");
                std::fflush(stdout);
            } else if (SDL_SetWindowFullscreen(window, !full)) {
                LOG_INFO(Frontend, "Alt+Enter: {}", full ? "window" : "fullscreen");
            } else {
                LOG_WARNING(Frontend, "Alt+Enter: could not switch: {}", SDL_GetError());
            }
            enter_swallowed = true;
            continue;
        }
        if (event.type == SDL_EVENT_KEY_UP && (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
            enter_swallowed = false;
        }
        // BB_TEXTURE_DUMP=1: F10 saves the textures of the next frame (user/texture_dump).
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat) {
            Vulkan::FrameCapture::ArmTextureDump();
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            // A release always reaches the game's mouse state, also when the overlay takes the event: the click
            // that opens the in-game save list is released over the list, and its button would stay held for the
            // game (its next click no press).
            std::scoped_lock lock{mouse_mutex};
            mouse_buttons &= ~MouseBit(event.button.button);
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse_captured) {
                std::scoped_lock lock{mouse_mutex};
                mouse_dx += event.motion.xrel;
                mouse_dy += event.motion.yrel;
                look_dx += event.motion.xrel;
                look_dy += event.motion.yrel;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            std::scoped_lock lock{mouse_mutex};
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !BbOverlay::CapturesInput()) {
                mouse_buttons |= MouseBit(event.button.button);
            } else {
                mouse_buttons &= ~MouseBit(event.button.button);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            std::scoped_lock lock{mouse_mutex};
            const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            mouse_wheel += y > 0 ? 1 : y < 0 ? -1 : 0;
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_RESTORED:
            window_focused = true;
            BackgroundPause(false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            window_focused = false;
            enter_swallowed = false; // its key-up may go to another window
            BackgroundPause(true);
            std::scoped_lock lock{mouse_mutex};
            mouse_buttons = 0;
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED:
            BackgroundPause(true);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.data1 > 0 && event.window.data2 > 0 &&
                !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
                windowed_w = event.window.data1;
                windowed_h = event.window.data2;
            }
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    // The window mode change (or another window holding the keys meanwhile) can swallow Enter's key-up:
    // once neither Enter is held, the game gets Enter again.
    if (enter_swallowed.load(std::memory_order_relaxed)) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!keys || (!keys[SDL_SCANCODE_RETURN] && !keys[SDL_SCANCODE_KP_ENTER])) enter_swallowed = false;
    }
    return is_open;
}

} // namespace Frontend
