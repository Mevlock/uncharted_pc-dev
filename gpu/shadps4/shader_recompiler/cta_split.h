// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include "common/types.h"

namespace Shader {

// bbport: Pascal-only workaround. The split changes the shader binary and the dispatch grid, so it
// is disabled unless BB_PASCAL_CTA_SPLIT is set in the environment (any value except empty/0).
inline bool CtaSplitEnabled() noexcept {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PASCAL_CTA_SPLIT");
        return env != nullptr && *env != '\0' && *env != '0';
    }();
    return enabled;
}

// bbport: a 1024-thread workgroup takes Pascal's whole register file (65536 registers), so a second
// such CTA set in flight aborts the scheduler with SKEDCHECK19_CTA_REGISTER_CONSUMPTION and the
// device is lost. Programs in this list are emitted as two 512-thread workgroups instead: the SPIR-V
// entry point declares a local size of 512, the ids the shader reads are remapped so its own
// arithmetic produces the same global index, and the dispatcher doubles the X grid (see
// vk_rasterizer.cpp). Only programs without workgroup memory, barriers or atomics can be listed.
inline bool IsSplitCtaProgram(u64 pgm_hash) noexcept {
    // Uncharted: Drake's Fortune particle update, 1024x1x1, dispatched directly.
    return CtaSplitEnabled() && pgm_hash == 0x000000007d7e7206ull;
}

inline u32 CtaSplitFactor(u64 pgm_hash) noexcept {
    return IsSplitCtaProgram(pgm_hash) ? 2u : 1u;
}

} // namespace Shader
