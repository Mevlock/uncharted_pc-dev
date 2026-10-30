// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader {

struct Profile {
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    u32 sparse_page_shift{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_float16{};
    bool support_float64{};
    bool supports_denorm_behavior_independence{};
    bool supports_rounding_mode_independence{};
    bool support_fp16_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp16_round_to_zero{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_fp64_denorm_preserve{};
    bool support_fp64_denorm_flush{};
    bool support_fp64_round_to_zero{};
    bool support_fp16_signed_zero_inf_nan_preserve{};
    bool support_fp32_signed_zero_inf_nan_preserve{};
    bool support_fp64_signed_zero_inf_nan_preserve{};
    bool supports_image_load_store_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool supports_shader_subgroup_clock{};
    bool has_broken_spirv_clamp{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    /// bbport: NVIDIA without fragment shader barycentrics (Pascal and older).
    bool needs_integer_interpolation_fix{};
    /// bbport: NVIDIA: compute workgroups above 256 threads run as workgroups of 256 (see
    /// WorkgroupSplit in runtime_info.h).
    bool split_large_workgroups{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    /// bbport (native batch): a compute shader run as one workgroup per object of a batch; the
    /// flat buffer holds [stride][per object: buffer offsets, user data, flat data] and each
    /// workgroup reads its object's entry (Rasterizer::FlushNativeBatch).
    bool batched_compute{};
    /// bbport (draw batching): a vertex + fragment pair drawing a run of guest draws with one
    /// indirect multi-draw. Draw i's buffer offsets, user data and flat data are entry i of a
    /// table in each stage's flat buffer (as batched_compute): the vertex shader finds it with
    /// the DrawIndex built-in and hands it to the fragment shader as a flat varying at
    /// BatchDrawLocation.
    bool batched_graphics{};
    static constexpr u32 BatchDrawLocation = 29;
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};
    bool supports_shader_stencil_export{};

    bool operator==(const Profile&) const = default;
};

} // namespace Shader
