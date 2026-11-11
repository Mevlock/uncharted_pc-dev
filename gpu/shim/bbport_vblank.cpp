// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the guest's vblank on the display's own refresh (bbport_vblank.h, GitHub #26).
#include "bbport_vblank.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <dxgi.h>
#include <windows.h>

#include "sdl_window.h"

extern Frontend::WindowSDL* g_window; // bbgpu.cpp

namespace BbVblank {
namespace {

using Clock = std::chrono::steady_clock;
std::chrono::nanoseconds period{};
IDXGIOutput* output = nullptr;
HMONITOR monitor = nullptr;
bool matches = false;              // the display on `monitor` refreshes at the vblank rate
Clock::time_point checked{};       // when the window's monitor was last looked at
Clock::time_point last_vblank{};
int misses = 0;                    // display vblanks far off the expected interval, in a row

HWND Window() {
    if (!g_window || !g_window->GetSDLWindow()) {
        return nullptr;
    }
    return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_window->GetSDLWindow()),
                                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

void Release() {
    if (output) {
        output->Release();
        output = nullptr;
    }
    matches = false;
}

// The DXGI output of `on` (any adapter: with two GPUs the display may hang off the other one).
IDXGIOutput* FindOutput(HMONITOR on) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    IDXGIOutput* found = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* candidate = nullptr;
        for (UINT o = 0; !found && adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == on) {
                found = candidate;
            } else {
                candidate->Release();
            }
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

// The window's monitor (again, after a move): its output, and whether its refresh measures within 2% of the
// vblank rate (20 vblanks, about a third of a second).
void LookUp() {
    checked = Clock::now();
    const HWND window = Window();
    const HMONITOR now_on = window ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY) : nullptr;
    if (!now_on || (now_on == monitor && output)) {
        return;
    }
    Release();
    monitor = now_on;
    output = FindOutput(monitor);
    if (!output) {
        std::printf("VideoOut: vblank on the display's refresh: this display has no DXGI output; the timer paces it\n");
        return;
    }
    constexpr int Samples = 20;
    if (FAILED(output->WaitForVBlank())) {
        Release();
        return;
    }
    const auto first = Clock::now();
    for (int i = 0; i < Samples; ++i) {
        if (FAILED(output->WaitForVBlank())) {
            Release();
            return;
        }
    }
    const double measured = std::chrono::duration<double>(Clock::now() - first).count() / Samples;
    const double wanted = std::chrono::duration<double>(period).count();
    matches = std::abs(measured - wanted) / wanted < 0.02;
    last_vblank = Clock::now();
    misses = 0;
    std::printf("VideoOut: display refresh measured %.3f Hz, vblank %.3f Hz: %s\n", 1.0 / measured, 1.0 / wanted,
                matches ? "the vblank follows the display's refresh"
                        : "not the same rate, the timer paces the vblank");
}

} // namespace

bool Start(std::chrono::nanoseconds vblank_period) {
    static const bool allowed = [] {
        const char* env = std::getenv("BB_VBLANK_SYNC");
        return !(env && env[0] == '0');
    }();
    if (!allowed) {
        return false;
    }
    period = vblank_period;
    LookUp();
    return true; // Wait() decides each tick (the window may move to a display that matches)
}

bool Wait() {
    if (Clock::now() - checked > std::chrono::seconds(2)) {
        LookUp();
    }
    if (!output || !matches) {
        return false;
    }
    if (FAILED(output->WaitForVBlank())) {
        Release();
        monitor = nullptr;
        return false;
    }
    // A display that stops refreshing at the expected pace (asleep, a mode change, VRR taking over) gives the
    // timer back until the next look-up.
    const auto now = Clock::now();
    const double interval = std::chrono::duration<double>(now - last_vblank).count();
    const double wanted = std::chrono::duration<double>(period).count();
    last_vblank = now;
    if (interval < wanted * 0.5 || interval > wanted * 1.5) {
        if (++misses >= 30) {
            std::printf("VideoOut: the display's vblanks stopped matching the vblank rate: the timer paces it\n");
            Release();
            monitor = nullptr;
            return false;
        }
    } else {
        misses = 0;
    }
    return true;
}

} // namespace BbVblank

#else

namespace BbVblank {
bool Start(std::chrono::nanoseconds) {
    return false;
}
bool Wait() {
    return false;
}
} // namespace BbVblank

#endif
