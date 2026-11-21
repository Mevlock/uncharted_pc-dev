// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <vector>
#include "common/assert.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

// bbport: swaps the buffer address stand-ins (runtime_info.h: MotionVectors in vertex shaders,
// LoopCount in every shader under BB_LOOP_COUNT) for the session's buffer addresses. The cache
// stores the stand-ins.
static void PatchAddresses(std::vector<u32>& code) {
    using Shader::LoopCount;
    using Shader::MotionVectors;
    constexpr u32 OpConstant = 43;
    for (size_t at = 5; at < code.size();) {
        const u32 op = code[at] & 0xFFFF;
        const u32 words = code[at] >> 16;
        if (words == 0 || at + words > code.size()) {
            return;
        }
        if (op == OpConstant && words == 5) {
            const u64 value = u64(code[at + 3]) | u64(code[at + 4]) << 32;
            u64 patched = value;
            const bool motion = MotionVectors::positions_address != 0;
            if (motion && value - MotionVectors::ParamsPlaceholder <= 16) {
                patched = MotionVectors::params_address + (value - MotionVectors::ParamsPlaceholder);
            } else if (motion && value == MotionVectors::PositionsPlaceholder) {
                patched = MotionVectors::positions_address;
            } else if (LoopCount::address && value - LoopCount::Placeholder <= 16) {
                patched = LoopCount::address + (value - LoopCount::Placeholder);
            }
            if (patched != value) {
                code[at + 3] = u32(patched);
                code[at + 4] = u32(patched >> 32);
            }
        }
        at += words;
    }
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    // Test knob: BB_TEST_SPV_SALT=<n> changes the SPIR-V generator word of every module, so the
    // driver's own on-disk shader cache cannot hit (a cold-driver measurement on a used PC).
    static const char* salt_env = std::getenv("BB_TEST_SPV_SALT");
    std::vector<u32> patched;
    if ((Shader::MotionVectors::positions_address != 0 || Shader::LoopCount::address != 0) &&
        code.size() > 5) {
        // Cheap pre-check: only shaders with the motion path or the loop count hold stand-ins.
        const auto holds = [&](u64 placeholder) {
            for (size_t i = 5; i + 1 < code.size(); ++i) {
                if (code[i + 1] == u32(placeholder >> 32) && code[i] == u32(placeholder)) {
                    return true;
                }
            }
            return false;
        };
        if ((Shader::MotionVectors::positions_address != 0 &&
             holds(Shader::MotionVectors::ParamsPlaceholder)) ||
            (Shader::LoopCount::address != 0 && holds(Shader::LoopCount::Placeholder))) {
            patched.assign(code.begin(), code.end());
            PatchAddresses(patched);
            code = patched;
        }
    }
    std::vector<u32> salted;
    if (salt_env && code.size() > 5) {
        salted.assign(code.begin(), code.end());
        salted[2] ^= u32(std::strtoul(salt_env, nullptr, 0)) * 2654435761u;
        code = salted;
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    if (module_result != vk::Result::eSuccess) {
        // The shader and its SPIR-V, for spirv-val: shader_module_failed_<hash>_<perm>.spv in the working folder.
        std::printf("Shader module refused: program %016llx permutation %u, %zu words of SPIR-V (%s)\n",
                    static_cast<unsigned long long>(t_spv_program_hash), t_spv_permutation, code.size(),
                    vk::to_string(module_result).c_str());
        char name[96];
        std::snprintf(name, sizeof(name), "shader_module_failed_%016llx_%u.spv",
                      static_cast<unsigned long long>(t_spv_program_hash), t_spv_permutation);
        if (std::FILE* f = std::fopen(name, "wb")) {
            std::fwrite(code.data(), sizeof(u32), code.size(), f);
            std::fclose(f);
        }
        std::fflush(stdout);
    }
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
