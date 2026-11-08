// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_hdr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

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
    t.gamut_expansion = v.hdr_gamut_expansion.load(r);
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

void PollTuneFile() {
    namespace fs = std::filesystem;
    static const std::string path = [] {
        if (const char* env = std::getenv("BB_HDR_TUNE_FILE"); env && *env) {
            return std::string{env};
        }
        const char* dir = std::getenv("BB_GPU_USER_DIR");
        return dir && *dir ? std::string{dir} + "/hdr_tune.txt" : std::string{"hdr_tune.txt"};
    }();
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next{};
    const Clock::time_point now = Clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::milliseconds(250);

    // The state at the first check is the baseline: a file left from an earlier session does not
    // override the launcher's paper white and peak; edit (or touch) it to apply it.
    static bool started = false;
    static fs::file_time_type last_time{};
    static std::uintmax_t last_size = 0;
    std::error_code error;
    const fs::file_time_type time = fs::last_write_time(path, error);
    const std::uintmax_t size = error ? 0 : fs::file_size(path, error);
    if (error) {
        if (!started) {
            started = true;
            std::printf("HDR tune: watching %s\n", path.c_str());
        }
        last_time = {};
        last_size = 0;
        return;
    }
    const bool changed = time != last_time || size != last_size;
    last_time = time;
    last_size = size;
    if (!started) {
        started = true;
        std::printf("HDR tune: watching %s (present; applied when it changes)\n", path.c_str());
        std::fflush(stdout);
        return;
    }
    if (!changed) {
        return;
    }
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return;
    }
    bool any = false;
    char line[256];
    std::string applied;
    while (std::fgets(line, sizeof(line), file)) {
        std::string text{line};
        if (const auto hash = text.find('#'); hash != std::string::npos) {
            text.erase(hash);
        }
        const auto eq = text.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto trim = [](const std::string& s) {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return std::string{};
            }
            return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
        };
        const std::string key = trim(text.substr(0, eq));
        const std::string value = trim(text.substr(eq + 1));
        if (value.empty() || !BbSettings::SetHdrKey(key, value, applied) || applied.empty()) {
            continue;
        }
        std::printf("HDR tune: %s (file)\n", applied.c_str());
        any = true;
    }
    std::fclose(file);
    if (any) {
        BbSettings::Save();
        std::fflush(stdout);
    }
}

} // namespace BbHdr
