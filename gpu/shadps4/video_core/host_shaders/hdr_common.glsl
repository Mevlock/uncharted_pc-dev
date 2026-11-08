// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HDR10 output helpers (post process, HDR overlay). From moistman7's PR #156 to
// BilalReloaded's port.

const mat3 BT709_TO_BT2020 = mat3(
    0.6274040, 0.0690970, 0.0163916,
    0.3292820, 0.9195400, 0.0880132,
    0.0433136, 0.0113612, 0.8955950);

// SMPTE ST 2084 inverse EOTF: absolute nits -> PQ signal.
vec3 PqFromNits(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// The sRGB curve, extended past 1.0 (the HDR scene) by its power segment.
vec3 SrgbEncodeExt(vec3 linear) {
    linear = max(linear, 0.0);
    return mix(1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, linear * 12.92,
               lessThan(linear, vec3(0.0031308)));
}

// The games' pictures are made for a gamma 2.2 display: the stored (sRGB-encoded) value v shows
// at v^2.2 of white. `linear`: as an sRGB view reads the picture (above 1.0: the HDR scene, already
// in the domain where this gives its own value). Rec. 709, white = paper_nits -> HDR10 signal.
vec3 Hdr10FromPicture(vec3 linear, float paper_nits, float peak_nits) {
    const vec3 nits = pow(SrgbEncodeExt(linear), vec3(2.2)) * paper_nits;
    return PqFromNits(clamp(BT709_TO_BT2020 * nits, 0.0, peak_nits));
}
