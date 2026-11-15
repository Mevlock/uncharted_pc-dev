// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
#extension GL_GOOGLE_include_directive : require

#include "hdr_common.glsl"

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    uint hdr; // 0: SDR; 1: the guest's own PQ picture; 2 (bbport): HDR10 encode of the game's picture
    bool srgb_input;
    bool downscale; // bbport: a source larger than the output is filtered (Lanczos 2), not point-sampled
    float paper_nits; // bbport (hdr 2): the game's white
    float peak_nits;
    float sdr_gamma; // bbport (hdr 2): the picture's decode gamma (2.2: the games' own display)
    float sdr_saturation; // bbport (hdr 2): Oklab chroma scale, 1.0 none
    float gamut_expansion; // bbport (hdr 2): Bt2020FromBt709, 0 accurate
    float hud_expansion; // bbport (hdr 2, scene HDR composite): the same for HUD pixels (alpha)
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
// The game's display gamma (sceVideoOutAdjustColor, 0.1..2.0; 1 = sRGB) changes the curve's exponent.
// bbport: the linear toe below the cutoff meets the curve at the cutoff for every gamma. It was
// d * rgb / gamma: at gamma 0.1 (Uncharted 2 photo mode Brightness 100) the darkest values rose ~3x
// above the curve, so a channel just under the cutoff came out brighter than one just over it and
// dark colours scrambled ("deep-fried"). At gamma 1 the slope is d, as before.
vec3 gamma(vec3 rgb) {
    const float e = 1.0 / (2.4 + 1.0 - pp.gamma);
    const float slope = pp.gamma == 1.0 ? d : (a * pow(cutoff, e) - b) / cutoff;
    return mix(
        a * pow(rgb, vec3(e)) - b,
        slope * rgb,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// bbport (supersampled render resolutions): a picture larger than the output was sampled with one
// bilinear tap per pixel, which skips most source texels at 1.5x per axis (shimmer, not
// supersampling). Lanczos 2 over every texel the pixel covers, scaled to the ratio (a sharp
// downscale, as the driver supersampling modes do); weights normalised, taps decoded first.
float lanczos2(float x) {
    x = abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 2.0) return 0.0;
    const float pi = 3.14159265;
    return 2.0 * sin(pi * x) * sin(pi * x / 2.0) / (pi * pi * x * x);
}

vec4 fetch_linear(ivec2 p, ivec2 size) {
    vec4 c = texelFetch(texSampler, clamp(p, ivec2(0), size - 1), 0);
    if (pp.hdr != 1u && pp.srgb_input) c.rgb = degamma(c.rgb);
    return c;
}

vec4 downscaled(vec2 scale) {
    const ivec2 size = textureSize(texSampler, 0);
    const vec2 centre = uv * vec2(size) - 0.5; // the pixel's centre in texel units
    const vec2 radius = 2.0 * max(scale, vec2(1.0));
    const ivec2 lo = ivec2(floor(centre - radius)) + 1, hi = ivec2(floor(centre + radius));
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = lo.y; y <= hi.y; ++y) {
        const float wy = lanczos2((float(y) - centre.y) / max(scale.y, 1.0));
        if (wy == 0.0) continue;
        for (int x = lo.x; x <= hi.x; ++x) {
            const float w = wy * lanczos2((float(x) - centre.x) / max(scale.x, 1.0));
            sum += w * fetch_linear(ivec2(x, y), size);
            total += w;
        }
    }
    return max(sum / max(total, 1e-4), vec4(0.0));
}

// bbport: the gamut expansion of a pixel; HUD pixels (alpha, the scene HDR composite) at least hud_expansion.
float Expansion(float hud) {
    return pp.hud_expansion > 0.0 ? max(pp.gamut_expansion, pp.hud_expansion * clamp(hud, 0.0, 1.0))
                                  : pp.gamut_expansion;
}

void main() {
    vec4 color_linear;
    // Source texels per output pixel, from the interpolated coordinates (constant over the quad).
    const vec2 scale = vec2(abs(dFdx(uv.x)), abs(dFdy(uv.y))) * vec2(textureSize(texSampler, 0));
    if (pp.downscale && (scale.x > 1.01 || scale.y > 1.01) && scale.x < 4.0 && scale.y < 4.0) {
        color_linear = downscaled(scale);
        if (pp.hdr == 2u) {
            color = vec4(Hdr10FromPicture(color_linear.rgb, pp.paper_nits, pp.peak_nits, pp.sdr_gamma,
                                               pp.sdr_saturation, Expansion(color_linear.a)), 1.0);
            return;
        }
        if (pp.hdr == 0u && pp.srgb_input) {
            // Already decoded per tap: only the gamma below is applied.
            color = vec4(gamma(color_linear.rgb), color_linear.a);
            return;
        }
    } else {
        color_linear = texture(texSampler, uv);
    }
    if (pp.hdr == 2u) {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(Hdr10FromPicture(color_linear.rgb, pp.paper_nits, pp.peak_nits, pp.sdr_gamma,
                                               pp.sdr_saturation, Expansion(color_linear.a)), 1.0);
    } else if (pp.hdr != 0u) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
