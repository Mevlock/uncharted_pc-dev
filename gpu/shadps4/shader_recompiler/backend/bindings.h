// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 buffer{};
    u32 user_data{};
    /// bbport: the descriptor set the stage's resources are declared in (0 normally; 1 for the pre-raster stages of a
    /// split-layout pipeline built from pipeline libraries, BB_GPL). Part of a permutation's identity.
    u32 descriptor_set{};

    auto operator<=>(const Bindings&) const = default;
};

} // namespace Shader::Backend
