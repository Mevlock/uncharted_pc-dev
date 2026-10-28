// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device);

/// bbport: the program hash and permutation of the shader this thread is turning into a module (named when the driver
/// refuses the module).
inline thread_local u64 t_spv_program_hash = 0;
inline thread_local u32 t_spv_permutation = 0;

} // namespace Vulkan
