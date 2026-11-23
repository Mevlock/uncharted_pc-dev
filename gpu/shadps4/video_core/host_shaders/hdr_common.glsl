// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HDR10 output helpers (post process, HDR overlay). From moistman7's PR #156 to
// BilalReloaded's port.

const mat3 BT709_TO_BT2020 = mat3(
    0.6274040, 0.0690970, 0.0163916,
    0.3292820, 0.9195400, 0.0880132,
    0.0433136, 0.0113612, 0.8955950);

// Display P3 (D65) to Rec. 2020, linear. Gamut expansion reads the Rec. 709 values as P3 ones.
const mat3 P3_TO_BT2020 = mat3(
    0.7538330, 0.0457438, -0.0012103,
    0.1985974, 0.9417772, 0.0176017,
    0.0475696, 0.0124789, 0.9836086);

// Rec. 2020 luminance (Y) weights.
const vec3 BT2020_Y = vec3(0.2627002, 0.6779981, 0.0593017);

// Gamut expansion (0-2): what a wide-gamut screen does with an SDR picture, its primaries stretched
// to the panel's. 0: Rec. 709 placed exactly in Rec. 2020 (accurate); 1: the values read as
// Display P3; 2: read as Rec. 2020 itself; in between a blend of the two neighbouring matrices.
// Every matrix maps white to white (D65), so greys stay neutral; the result is scaled back to the
// accurate conversion's luminance, so brightness (paper white, highlights) does not change.
// Rec. 709 linear -> Rec. 2020 linear.
vec3 Bt2020FromBt709(vec3 c, float expansion) {
    const vec3 accurate = BT709_TO_BT2020 * c;
    if (expansion <= 0.0) {
        return accurate;
    }
    const vec3 p3 = P3_TO_BT2020 * c;
    const vec3 wide = expansion <= 1.0 ? mix(accurate, p3, expansion)
                                       : mix(p3, c, min(expansion, 2.0) - 1.0);
    const float y = dot(accurate, BT2020_Y), wide_y = dot(wide, BT2020_Y);
    return y > 0.0 && wide_y > 1e-6 ? wide * clamp(y / wide_y, 0.25, 4.0) : wide;
}

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

// Oklab (Bjorn Ottosson) from and to linear Rec. 709.
vec3 OklabFromBt709(vec3 c) {
    const vec3 lms = vec3(dot(c, vec3(0.4122214708, 0.5363325363, 0.0514459929)),
                          dot(c, vec3(0.2119034982, 0.6806995451, 0.1073969566)),
                          dot(c, vec3(0.0883024619, 0.2817188376, 0.6299787005)));
    const vec3 r = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return vec3(dot(r, vec3(0.2104542553, 0.7936177850, -0.0040720468)),
                dot(r, vec3(1.9779984951, -2.4285922050, 0.4505937099)),
                dot(r, vec3(0.0259040371, 0.7827717662, -0.8086757660)));
}

vec3 Bt709FromOklab(vec3 lab) {
    const vec3 r = vec3(dot(lab, vec3(1.0, 0.3963377774, 0.2158037573)),
                        dot(lab, vec3(1.0, -0.1055613458, -0.0638541728)),
                        dot(lab, vec3(1.0, -0.0894841775, -1.2914855480)));
    const vec3 lms = r * r * r;
    return vec3(dot(lms, vec3(4.0767416621, -3.3077115913, 0.2309699292)),
                dot(lms, vec3(-1.2684380046, 2.6097574011, -0.3413193965)),
                dot(lms, vec3(-0.0041960863, -0.7034186147, 1.7076147010)));
}

// The games' pictures are made for a gamma 2.2 display: the stored (sRGB-encoded) value v shows
// at v^gamma of white (2.2; the menu's HDR section can pick another, e.g. 2.4 for a BT.1886 look).
// `linear`: as an sRGB view reads the picture (above 1.0: the HDR scene, already in the domain
// where gamma 2.2 gives its own value). `saturation` scales the Oklab chroma (lightness and hue
// kept; 1.0: none): colours past Rec. 709 land in Rec. 2020's wider gamut instead of clipping.
// `expansion`: gamut expansion (Bt2020FromBt709; 0: accurate).
// Rec. 709, white = paper_nits -> HDR10 signal.
vec3 Hdr10FromPicture(vec3 linear, float paper_nits, float peak_nits, float gamma,
                      float saturation, float expansion) {
    vec3 rel = pow(SrgbEncodeExt(linear), vec3(gamma));
    if (saturation != 1.0) {
        vec3 lab = OklabFromBt709(rel);
        lab.yz *= saturation;
        rel = Bt709FromOklab(lab);
    }
    return PqFromNits(clamp(Bt2020FromBt709(rel * paper_nits, expansion), 0.0, peak_nits));
}
