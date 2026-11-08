// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's overlay (settings menu, FPS counter, toasts), drawn by ImGui into a cleared
// 8-bit image, blended onto the HDR10 swapchain image at paper white. From moistman7's PR #156
// to BilalReloaded's port.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "hdr_common.glsl"

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D overlay;

layout (push_constant) uniform settings {
    float gamma;
    uint hdr;
    bool srgb_input;
    bool downscale;
    float paper_nits;
    float peak_nits;
    float sdr_gamma;
    float sdr_saturation;
} pp;

void main() {
    // ImGui's blending into transparent black leaves premultiplied colour and its coverage.
    const vec4 o = texture(overlay, uv);
    if (o.a <= 0.0) {
        discard;
    }
    const vec3 stored = clamp(o.rgb / o.a, 0.0, 1.0);
    // Stored values for a gamma 2.2 display: as the picture (Hdr10FromPicture takes linear).
    const vec3 nits = pow(stored, vec3(2.2)) * pp.paper_nits;
    color = vec4(PqFromNits(BT709_TO_BT2020 * nits), o.a);
}
