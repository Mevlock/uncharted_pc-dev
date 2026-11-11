// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the NaN guard on colour outputs, shared by the shader recompiler and the shader cache.
#pragma once

#include <cstdlib>

namespace BbNanGuard {

/// Colour outputs to float render targets: NaN becomes 0, so one bad pixel cannot grow into a
/// black 16x16 block through the games' tiled post-processing (Uncharted 2 on the Steam Deck).
/// BB_NAN_GUARD: 1 (default) NaN only, 2 also clamps +-Inf to the half-float range (that broke
/// Uncharted 1's lighting and fog: blue trees), 0 off.
inline int NanGuardMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_NAN_GUARD");
        return env && (env[0] == '0' || env[0] == '2') ? env[0] - '0' : 1;
    }();
    return mode;
}

inline bool ExportNanGuard() {
    return NanGuardMode() != 0;
}

/// Uncharted 3's bloom bright pass (compute shader 1e8e11d0, scene colour to the quarter-size
/// bloom image): a treasure glint covering few pixels can reach 65504 (half-float max) at a
/// reduced render size, which the bloom blur turns into a large white diamond. Its writes are
/// limited to BB_BLOOM_CLAMP (default 4; 0 off), NaN to 0; the scene's own highlights stay
/// below 1.
inline float BloomClamp() {
    static const float limit = [] {
        const char* env = std::getenv("BB_BLOOM_CLAMP");
        return env ? float(std::atof(env)) : 4.0f;
    }();
    return limit;
}

/// BB_BLOOM=0: the bright pass writes nothing but zero, so the game has no bloom at all.
inline bool BloomOff() {
    static const bool off = [] {
        const char* env = std::getenv("BB_BLOOM");
        return env && env[0] == '0';
    }();
    return off;
}

inline bool BloomClampShader(unsigned long long hash) {
    return static_cast<unsigned>(hash) == 0x1e8e11d0u && (BloomOff() || BloomClamp() > 0.0f);
}

/// Shaders compiled in each mode are cached apart.
inline const char* ShaderCacheSuffix() {
    return NanGuardMode() == 1 ? "-nan1" : NanGuardMode() == 2 ? "-nan2" : "";
}

} // namespace BbNanGuard
