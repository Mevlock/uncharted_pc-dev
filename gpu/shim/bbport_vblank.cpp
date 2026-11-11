// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the guest's vblank steered by the display's own refresh (bbport_vblank.h, GitHub #26).
#include "bbport_vblank.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <dxgi.h>
#include <windows.h>

#include "sdl_window.h"

extern Frontend::WindowSDL* g_window; // bbgpu.cpp

namespace BbVblank {
namespace {

using Clock = std::chrono::steady_clock;
std::chrono::nanoseconds nominal{};
// Written by the watcher thread, read by the present thread.
std::atomic<long long> last_vblank_ns{0};  // the display's last vblank, steady_clock ns
std::atomic<long long> display_period_ns{0}; // its measured refresh interval (smoothed), 0 = no match
// Where the ticks settle: this long after the display's vblank (the flip right after it is shown at the next one).
constexpr long long PhaseNs = 1'000'000;

long long Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

HWND Window() {
    if (!g_window || !g_window->GetSDLWindow()) {
        return nullptr;
    }
    return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_window->GetSDLWindow()),
                                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
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

// The watcher: the display's vblanks, one by one, on the monitor the window is on (looked up again every 2 s).
// The refresh interval is a smoothed average of the plausible intervals; it counts as a match within 2% of the
// vblank rate.
void Watch() {
    SetThreadDescription(GetCurrentThread(), L"bb:VblankWatch");
    const double wanted = double(nominal.count());
    IDXGIOutput* output = nullptr;
    HMONITOR monitor = nullptr;
    long long checked = 0, previous = 0;
    double period = 0.0;
    int samples = 0;
    bool reported = false, matching = false;
    for (;;) {
        const long long now = Now();
        if (now - checked > 2'000'000'000LL || !output) {
            checked = now;
            const HWND window = Window();
            const HMONITOR on = window ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY) : nullptr;
            if (on && (on != monitor || !output)) {
                if (output) {
                    output->Release();
                }
                monitor = on;
                output = FindOutput(on);
                previous = 0;
                period = 0.0;
                samples = 0;
                reported = false;
                display_period_ns.store(0);
            }
            if (!output) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }
        if (FAILED(output->WaitForVBlank())) {
            output->Release();
            output = nullptr;
            display_period_ns.store(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        const long long at = Now();
        last_vblank_ns.store(at);
        if (previous) {
            const double interval = double(at - previous);
            // one refresh (not a missed one, not a mode change): averaged in
            if (interval > wanted * 0.75 && interval < wanted * 1.25) {
                period = samples == 0 ? interval : period + (interval - period) * (samples < 60 ? 1.0 / (samples + 1) : 1.0 / 60);
                ++samples;
            }
        }
        previous = at;
        if (samples >= 30) {
            const bool match = std::abs(period - wanted) / wanted < 0.02;
            display_period_ns.store(match ? (long long)std::llround(period) : 0);
            if (!reported || match != matching) {
                std::printf("VideoOut: display refresh measured %.3f Hz, vblank %.3f Hz: %s\n", 1e9 / period,
                            1e9 / wanted,
                            match ? "the vblank follows the display's refresh"
                                  : "not the same rate, the timer paces the vblank");
                std::fflush(stdout);
                reported = true;
                matching = match;
            }
        }
    }
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
    nominal = vblank_period;
    std::thread(Watch).detach();
    return true;
}

std::chrono::nanoseconds Correction() {
    const long long period = display_period_ns.load();
    const long long vblank = last_vblank_ns.load();
    const long long now = Now();
    // no match, or no vblank lately (the watcher is between displays): the timer as it is
    if (!period || now - vblank > 4 * period) {
        return {};
    }
    // Where this tick falls after the display's vblank, against where it should (PhaseNs), wrapped to half a period
    // either way; a tenth of that is taken off the next interval, and the interval runs at the display's rate.
    long long error = (now - vblank) % period - PhaseNs;
    if (error > period / 2) {
        error -= period;
    } else if (error < -period / 2) {
        error += period;
    }
    return std::chrono::nanoseconds(period - nominal.count() - error / 10);
}

} // namespace BbVblank

#else

namespace BbVblank {
bool Start(std::chrono::nanoseconds) {
    return false;
}
std::chrono::nanoseconds Correction() {
    return {};
}
} // namespace BbVblank

#endif
