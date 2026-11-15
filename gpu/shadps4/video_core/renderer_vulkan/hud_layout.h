// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// bbport: the wide HUD at ultrawide sizes (BB_HUD_LAYOUT=wide, launcher -> Display -> HUD at wide
// screens). Ported from BilalReloaded's Uncharted port (gow3_pc hud_layout.h, 0.8.5), where
// Ferosnow95 wrote it for Uncharted 2 (his pull request #191): which draws are the in-game HUD
// (Tracker, HalfToFloat) and the idea of drawing a HUD draw once per third of its layout, each
// pass cut to the screen columns its third is shown at. Fitted to this port's own UI squeeze:
// here the vertex shader of a UI draw over a widened frame already squeezes it into the middle
// 16:9 (HwVertexRuntimeInfo::ui_squeeze_bits: clip x *= k, vertices on the screen's left or right
// edge stay), so a pass only moves the squeezed picture sideways (viewport x) and cuts it
// (scissor):
//   - the left third of the 16:9 layout goes to the left edge of the wide area,
//   - the middle third stays in the middle,
//   - the right third goes to the right edge.
// The wide area is the screen, or a centred narrower area (BB_HUD_WIDE_ASPECT, e.g. 21:9 on a
// 32:9 screen). Menus (drawn after the frame's copy into the display buffer), draws across the
// whole width (fades, letterbox bars) and draws touching a screen edge (their edge vertices are
// not squeezed, a sideways move would cut them) keep the 16:9 layout.
namespace Vulkan::HudLayout {

/// An IEEE half float (binary16) as a float.
inline float HalfToFloat(uint16_t h) {
    const uint32_t exponent = (h >> 10) & 0x1f;
    const uint32_t mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) {
        value = std::ldexp(float(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? std::nanf("") : HUGE_VALF;
    } else {
        value = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    }
    return (h & 0x8000) ? -value : value;
}

/// The thirds of the 16:9 layout (None: the draw is not split).
enum class Pass : uint8_t { None, Left, Centre, Right };

/// What a HUD draw's horizontal extent (its vertex positions: NDC, or beyond +-4 the 1280-wide UI
/// space) allows: Split (drawn per third), or Keep (16:9 as before): across the whole width,
/// touching a screen edge, or unknown.
enum class Extent : uint8_t { Split, Full, Edge, Unknown };
inline Extent ClassifyExtent(bool valid, float min_x, float max_x) {
    if (!valid || !(min_x <= max_x)) {
        return Extent::Unknown;
    }
    if (std::max(std::fabs(min_x), std::fabs(max_x)) > 4.0f) {
        min_x = min_x / 640.0f - 1.0f;
        max_x = max_x / 640.0f - 1.0f;
    }
    if (min_x <= -0.995f && max_x >= 0.995f) {
        return Extent::Full;
    }
    // The vertex shader keeps vertices at |x| >= 0.999 |w| where they are.
    if (min_x <= -0.998f || max_x >= 0.998f) {
        return Extent::Edge;
    }
    return Extent::Split;
}

/// The wide area's half width in NDC (1: the screen), at least the 16:9 layout's (k).
inline float AreaHalf(float screen_aspect, float area_aspect, float k) {
    if (area_aspect <= 0.0f || screen_aspect <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(area_aspect / screen_aspect, k, 1.0f);
}

/// NDC shift of a pass's squeezed picture (x' = k x + shift): the layout's left edge (-1 -> -k
/// squeezed) to the area's left edge, or its right edge to the area's right edge.
inline float Shift(Pass pass, float k, float area) {
    switch (pass) {
    case Pass::Left:
        return k - area;
    case Pass::Right:
        return area - k;
    default:
        return 0.0f;
    }
}

/// The screen columns (NDC, [x0, x1]) a pass shows: its third of the layout, squeezed and moved.
struct Span {
    float x0, x1;
};
inline Span Columns(Pass pass, float k, float area) {
    const float third = 2.0f * k / 3.0f;
    switch (pass) {
    case Pass::Left:
        return {-area, -area + third};
    case Pass::Right:
        return {area - third, area};
    default:
        return {-k / 3.0f, k / 3.0f};
    }
}

/// A target x in pixels (viewport [vx, vx + vw]) drawn by a pass: where the squeezed, moved
/// layout puts the unsqueezed position x (for the game's own scissor).
inline float Place(float x, float vx, float vw, float k, float shift) {
    const float ndc = (x - vx) / vw * 2.0f - 1.0f;
    return vx + (ndc * k + shift + 1.0f) * 0.5f * vw;
}

/// The pass's scissor columns in pixels: the game's scissor [gx0, gx1) (a scissor across the
/// whole viewport stays as it is; a narrower one moves with the layout), cut to the pass's columns.
struct Pixels {
    int32_t x0, x1;
};
inline Pixels PassScissor(Pass pass, int32_t gx0, int32_t gx1, float vx, float vw, float k,
                          float area) {
    const float shift = Shift(pass, k, area);
    float x0 = float(gx0), x1 = float(gx1);
    if (gx0 > int32_t(std::lround(vx)) || gx1 < int32_t(std::lround(vx + vw))) {
        x0 = Place(x0, vx, vw, k, shift);
        x1 = Place(x1, vx, vw, k, shift);
    }
    const Span c = Columns(pass, k, area);
    const float c0 = vx + (c.x0 + 1.0f) * 0.5f * vw, c1 = vx + (c.x1 + 1.0f) * 0.5f * vw;
    const int32_t a = int32_t(std::lround(std::max(x0, c0)));
    const int32_t b = int32_t(std::lround(std::min(x1, c1)));
    return {a, std::max(a, b)};
}

/// BB_HUD_LAYOUT: "wide" (side elements at the edges); anything else the 16:9 layout.
inline bool WideFromEnv() {
    const char* env = std::getenv("BB_HUD_LAYOUT");
    return env && std::strcmp(env, "wide") == 0;
}

/// BB_HUD_WIDE_ASPECT: the wide area as W:H or a float ("screen", unset or 0: the screen).
inline float AspectFromEnv(const char* name = "BB_HUD_WIDE_ASPECT") {
    const char* env = std::getenv(name);
    if (!env || !*env) {
        return 0.0f;
    }
    float w = 0.0f, h = 0.0f;
    char colon = 0;
    if (std::sscanf(env, "%f%c%f", &w, &colon, &h) == 3 && colon == ':' && h > 0.0f) {
        return w / h;
    }
    return std::max(0.0f, float(std::atof(env)));
}

/// Which draws are the in-game HUD's (from Ferosnow95's Tracker). The games finish a frame with
/// the tonemap draw (pixel shader 484cdbf5, BB_HUDLESS_PS) into its target, draw the HUD,
/// subtitles and prompts over that target, copy it into a display buffer (749bbec9) and draw
/// menus into the display buffer after the copy. HUD draws: indexed draws into the tonemap target
/// after the tonemap draw, until the copy (a full-screen pass is not indexed).
struct Tracker {
    uint64_t tonemap_ps = 0x484cdbf5ull;
    uint64_t composite_ps = 0x749bbec9ull;
    uint64_t frame_target = 0; ///< the tonemap draw's target
    bool over_frame = false;   ///< after the tonemap draw, before the copy
    // Trace counters (BB_HUD_TRACE).
    uint64_t frames = 0, over_frame_draws = 0, over_frame_skipped = 0, after_copy_draws = 0;

    /// One draw: its pixel shader, colour target and whether it is indexed; true for a HUD draw.
    bool Draw(uint64_t ps, uint64_t target, bool display, bool indexed) {
        if (ps == tonemap_ps) {
            ++frames;
            frame_target = target;
            over_frame = true;
            return false;
        }
        if (display && ps == composite_ps) {
            over_frame = false;
            return false;
        }
        if (over_frame && target == frame_target) {
            (indexed ? over_frame_draws : over_frame_skipped)++;
            return indexed;
        }
        after_copy_draws += display && !over_frame ? 1 : 0;
        return false; // other targets (the scene, offscreen HUD work) leave the phases alone
    }
};

} // namespace Vulkan::HudLayout
