// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_hdr.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "bbport_saves.h"

namespace BbHdr {
namespace {

std::atomic<bool> g_output{false};

float Number(const char* name, float fallback, float lo, float hi) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    return end == value ? fallback : std::clamp(parsed, lo, hi);
}

} // namespace

bool Requested() {
    static const bool on = [] {
        const char* value = std::getenv("BB_HDR");
        return value && value[0] == '1';
    }();
    return on;
}

float PeakNits() {
    static const float nits = Number("BB_HDR_PEAK_NITS", 1000.0f, 400.0f, 10000.0f);
    return nits;
}

float PaperWhite() {
    static const float nits = Number("BB_HDR_PAPER_WHITE", 203.0f, 80.0f, 500.0f);
    return nits;
}

void SetOutput(bool on) {
    g_output.store(on, std::memory_order_release);
    if (on) {
        std::printf("HDR: HDR10 output, peak %.0f nits, paper white %.0f nits\n", PeakNits(),
                    PaperWhite());
        std::fflush(stdout);
    }
}

bool Output() {
    return g_output.load(std::memory_order_acquire);
}

bool Scene() {
    static const int forced = [] {
        const char* value = std::getenv("BB_HDR_SCENE");
        return value && *value ? (value[0] == '1' ? 1 : 0) : -1;
    }();
    if (!Output() || forced == 0) {
        return false;
    }
    return forced == 1 || BbSaves::Game() == 2;
}

bool FrameGenAllowed() {
    static const bool allowed = [] {
        const char* value = std::getenv("BB_HDR_FRAME_GEN");
        return !(value && value[0] == '0');
    }();
    return allowed;
}

} // namespace BbHdr
