// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_hdr.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "bbport_saves.h"
#include "bbport_settings.h"

namespace BbHdr {
namespace {

std::atomic<bool> g_output{false};

} // namespace

bool Requested() {
    static const bool on = [] {
        const char* value = std::getenv("BB_HDR");
        return value && value[0] == '1';
    }();
    return on;
}

// BbSettings::Load reads BB_HDR_PEAK_NITS and BB_HDR_PAPER_WHITE (they override bbport.ini).
float PeakNits() {
    return BbSettings::Get().hdr_peak_nits.load(std::memory_order_relaxed);
}

float PaperWhite() {
    return BbSettings::Get().hdr_paper_white.load(std::memory_order_relaxed);
}

Tuning Current() {
    const auto& v = BbSettings::Get();
    constexpr auto r = std::memory_order_relaxed;
    Tuning t{};
    t.peak_nits = v.hdr_peak_nits.load(r);
    t.paper_white = v.hdr_paper_white.load(r);
    t.exposure = v.hdr_exposure.load(r);
    t.saturation = v.hdr_saturation.load(r);
    t.highlight_saturation = v.hdr_highlight_saturation.load(r);
    t.contrast = v.hdr_contrast.load(r);
    t.chrominance = v.hdr_chrominance.load(r);
    t.blowout = v.hdr_blowout.load(r);
    t.hue_correction = v.hdr_hue_correction.load(r);
    t.per_channel = v.hdr_per_channel.load(r) ? 1.0f : 0.0f;
    t.highlight_start = v.hdr_highlight_start.load(r);
    t.sdr_gamma = v.hdr_sdr_gamma.load(r);
    t.sdr_saturation = v.hdr_sdr_saturation.load(r);
    return t;
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
