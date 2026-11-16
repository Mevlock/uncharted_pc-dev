// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Uncharted 3's exposure metering at render sizes above 2048 pixels wide, shared by the
// shader recompiler and the shader cache.
#pragma once

#include <cstdlib>

namespace BbExposureFix {

/// The game meters its exposure in two compute shaders made for the PS4's 1920x1080. aec539f0
/// sums the luminance of each row of the half-size scene with one workgroup of 256 threads x 4
/// taps = 1024 columns, stepping by constants[8] = 1 / half width; 7095d6c2 adds the rows up and
/// multiplies by constants[10] = 1 / (half width x half height). At 1920x1080 the half-size scene
/// is 960 wide: all of it. Above 2048 wide (2560x1440, 3440x1440, 4K) only the LEFT 1024 half-size
/// columns were summed and divided by the full count: the average was too low and came from the
/// left part of the picture only (on a 3440x1440 street scene with a sunlit wall on the right the
/// game metered 1.8 stops under the 1920x1080 value and ran its exposure to the upper limit: the
/// picture about 40% brighter than on the PS4).
/// The translated shaders take the column step as max(constants[8], 1 / 1024), so the 1024 taps
/// span the whole row, and the divisor as constants[10] x max(1, 1 / (1024 x constants[8])), the
/// number of samples really summed: unchanged up to 2048 wide. BB_EXPOSURE_FIX=0: off.
inline bool ExposureShader(unsigned long long hash) {
    static const bool on = [] {
        const char* env = std::getenv("BB_EXPOSURE_FIX");
        return !(env && env[0] == '0');
    }();
    const unsigned low = static_cast<unsigned>(hash);
    return on && (low == 0xaec539f0u || low == 0x7095d6c2u);
}

} // namespace BbExposureFix
