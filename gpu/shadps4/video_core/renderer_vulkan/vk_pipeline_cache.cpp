// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <csetjmp>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <ranges>
#include <string>
#include <algorithm>
#include <thread>
#include <bit>
#include <unordered_set>

#include "bbport_threads.h"
#include "common/hash.h"
#include "common/thread.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_focus_fix.h"
#include "bbport_nan_guard.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <vk_mem_alloc.h>

// bbport: ultrawide (runtime_ultrawide.c): the horizontal widening factor, and whether frames
// with the 3D scene fill the window.
extern "C" float runtime_ultrawide_scale;
extern "C" int runtime_ultrawide_fill;

namespace Vulkan {

extern std::atomic<u32> g_frame_scene_draws;
extern std::atomic<bool> g_last_frame_scene;
u32 SceneFrameDraws();

/// bbport: a draw of the UI over a widened frame: into the display buffer after the finished
/// picture (BB_HUDLESS_PS), in a frame with the 3D scene (it fills the window). Its vertex
/// shader squeezes the UI back to 16:9 in the middle (HwVertexRuntimeInfo::ui_squeeze_bits).
/// Menus, loading screens and movies have no scene: their frames stay 16:9, the UI as it is.
static bool UiSqueezeDraw(const AmdGpu::Regs& regs, const PipelineSelection& sel) {
    if (!runtime_ultrawide_fill) {
        return false;
    }
    static const u32 hudless = [] {
        const char* env = std::getenv("BB_HUDLESS_PS");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 16)) : 0u;
    }();
    const u32 scene = g_frame_scene_draws.load(std::memory_order_relaxed);
    const bool scene_frame =
        g_last_frame_scene.load(std::memory_order_relaxed) || scene >= SceneFrameDraws();
    const VAddr target = regs.color_buffers[0] ? regs.color_buffers[0].Address() : 0;
    const bool display = target && FrameCapture::IsDisplayBuffer(target);
    const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
    const auto* ps = sel.infos[static_cast<u32>(Shader::SwStage::Fragment)];
    const bool is_hudless = ps && static_cast<u32>(ps->pgm_hash) == hudless;
    const bool squeeze = scene_frame && display && vs && !is_hudless;
    // BB_UI_LOG=1 (diagnosis): each kind of draw in a scene frame once, with the decision.
    static const bool log = [] {
        const char* env = std::getenv("BB_UI_LOG");
        return env && env[0] == '1';
    }();
    if (log && scene_frame && !regs.depth_control.depth_enable) {
        static std::mutex log_mutex;
        static std::unordered_set<u64> seen;
        const u64 vs_hash = vs ? vs->pgm_hash : 0, ps_hash = ps ? ps->pgm_hash : 0;
        const u64 id = (vs_hash * 0x9E3779B97F4A7C15ull) ^ (ps_hash << 1) ^ target;
        std::scoped_lock lock{log_mutex};
        if (seen.size() < 400 && seen.insert(id).second) {
            std::printf("UI log: target %#llx%s %ux%u vs %08x ps %08x scene %u%s -> %s\n",
                        static_cast<unsigned long long>(target), display ? " (display)" : "",
                        regs.color_buffers[0] ? regs.color_buffers[0].Pitch() : 0,
                        regs.color_buffers[0] ? regs.color_buffers[0].Height() : 0,
                        static_cast<u32>(vs_hash), static_cast<u32>(ps_hash), scene,
                        is_hudless ? " (hudless)" : "", squeeze ? "squeezed" : "as is");
        }
    }
    return squeeze;
}

// bbport diagnosis: BB_VS_CHECK=1 — every vertex shader records its hash when it outputs a
// non-finite or huge position (Shader::VertexCheck); VertexCheckPoll prints them per frame.
namespace {
u32* vertex_check_mapped = nullptr;

void SetupVertexCheck(const Instance& instance) {
    const char* env = std::getenv("BB_VS_CHECK");
    if (!env || env[0] != '1' || Shader::VertexCheck::address) {
        return;
    }
    if (!instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        std::printf("VS check: vertexPipelineStoresAndAtomics unsupported, off\n");
        return;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16 + Shader::VertexCheck::Slots * Shader::VertexCheck::RecordWords * 4,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS || !info.pMappedData) {
        std::printf("VS check: buffer creation failed, off\n");
        return;
    }
    vertex_check_mapped = static_cast<u32*>(info.pMappedData);
    std::memset(vertex_check_mapped, 0, 16 + Shader::VertexCheck::Slots * Shader::VertexCheck::RecordWords * 4);
    Shader::VertexCheck::address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    std::printf("VS check: on (vertex shaders compiled now, none from the shader cache)\n");
}
} // namespace

void VertexCheckPoll() {
    if (!vertex_check_mapped) {
        return;
    }
    static u64 frame = 0, total = 0;
    ++frame;
    const u32 count = __atomic_load_n(&vertex_check_mapped[0], __ATOMIC_ACQUIRE);
    // Per shader this flip. Only the first Slots records are kept: past that, the counts are
    // shares of a sample (the total stays exact).
    std::map<u32, u32> per_shader;
    std::map<u32, std::vector<const u32*>> samples;
    for (u32 i = 0; i < std::min(count, Shader::VertexCheck::Slots); ++i) {
        const u32* record = &vertex_check_mapped[4 + i * Shader::VertexCheck::RecordWords];
        ++per_shader[record[0]];
        auto& list = samples[record[0]];
        if (list.size() < 4) {
            list.push_back(record);
        }
    }
    if (count) {
        __atomic_store_n(&vertex_check_mapped[0], 0u, __ATOMIC_RELEASE);
    }
    total += count;
    // Constant output (shaders culling vertices with a NaN position every frame) is printed
    // once; a flip whose shaders or counts differ from the previous one is printed (a glitch
    // for one frame shows as two such lines), and a summary every 600 flips.
    static std::map<u32, u32> previous;
    static u32 reports = 0;
    const auto list_of = [](const std::map<u32, u32>& m) {
        std::string list;
        for (const auto& [hash, n] : m) {
            list += fmt::format(" {:08x}x{}", hash, n);
        }
        return list.empty() ? std::string{" none"} : list;
    };
    if (per_shader != previous && reports++ < 3000) {
        std::printf("VS check: flip %llu: %u bad vertex positions:%s\n",
                    static_cast<unsigned long long>(frame), count, list_of(per_shader).c_str());
        // Samples of the shaders that changed: vertex, instance, clip position.
        for (const auto& [hash, n] : per_shader) {
            const auto it = previous.find(hash);
            if (it != previous.end() && it->second == n) {
                continue;
            }
            std::string text;
            for (const u32* r : samples[hash]) {
                float pos[4];
                std::memcpy(pos, &r[3], sizeof(pos));
                text += fmt::format(" v{} i{} ({:g} {:g} {:g} {:g})", r[1], r[2], pos[0], pos[1],
                                    pos[2], pos[3]);
            }
            std::printf("VS check:   %08x:%s\n", hash, text.c_str());
        }
    }
    previous = std::move(per_shader);
    if (frame % 600 == 0) {
        std::printf("VS check: %llu bad vertex positions in %llu flips\n",
                    static_cast<unsigned long long>(total), static_cast<unsigned long long>(frame));
    }
}

/// bbport: a graphics pipeline being built on a compiler thread, from copies of what the GPU
/// thread selected (the live stage infos change with every draw).
struct AsyncGraphicsCompilation {
    AsyncJob<std::shared_ptr<AsyncGraphicsCompilation>> job;
    PipelineSelection selection;
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> infos;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    std::array<const Shader::Info*, MaxShaderStages> live_infos{};
    GraphicsPipeline::SerializationSupport data;
    std::unique_ptr<GraphicsPipeline> pipeline;
    bool failure_logged = false;
};

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

static PipelineCache* g_exit_pipeline_cache = nullptr;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        info.hw.vs.ui_squeeze_bits =
            sel.ui_squeeze ? std::bit_cast<u32>(runtime_ultrawide_scale) : 0u;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = sel.cs ? *sel.cs : liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    SetupVertexCheck(instance);
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    // bbport: the driver's own pipeline cache lives on disk beside the shader blobs, so a second
    // run does not have the driver compile the same pipelines again. The stored pipelines are
    // preloaded by StartWarmUp once the rasterizer is ready.
    if (const char* env = std::getenv("BB_PREP_COMPILE")) {
        worker_compile = env[0] != '0';
    }
    std::vector<char> initial_data;
    if (EmulatorSettings.IsPipelineCacheEnabled()) {
        auto& db = Storage::DataBase::Instance();
        db.Open();
        // Not warm until StartWarmUp has run and finished: nothing may select pipelines before.
        warm_done.store(false, std::memory_order_release);
        if (!db.Directory().empty()) {
            driver_cache_path = db.Directory() / "vulkan_driver.vkc";
            std::error_code ec;
            const auto size = std::filesystem::file_size(driver_cache_path, ec);
            if (!ec && size > 0 && size < (u64(1) << 30)) {
                std::ifstream file{driver_cache_path, std::ios::binary};
                initial_data.resize(size);
                if (!file.read(initial_data.data(), std::streamsize(size))) {
                    initial_data.clear();
                }
            }
        }
    }
    vk::PipelineCacheCreateInfo cache_info{
        .initialDataSize = initial_data.size(),
        .pInitialData = initial_data.empty() ? nullptr : initial_data.data(),
    };
    auto created = instance.GetDevice().createPipelineCacheUnique(cache_info);
    if (created.result != vk::Result::eSuccess && !initial_data.empty()) {
        initial_data.clear();
        created = instance.GetDevice().createPipelineCacheUnique({});
    }
    ASSERT_MSG(created.result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(created.result));
    pipeline_cache = std::move(created.value);
    driver_cache_loaded_size = DriverCacheSize();
    if (!driver_cache_path.empty()) {
        driver_cache_saver = std::jthread([this](std::stop_token stop) { DriverCacheSaver(stop); });
    }
    g_exit_pipeline_cache = this;

    // bbport: compiler threads for graphics pipelines (from the God of War III port). A new
    // pipeline used to be built on the GPU thread while the game waited: a stutter at every
    // new effect. The draws that need it now skip for the frames it takes to build.
    // Off by default (BB_ASYNC_SHADERS=1: on): a draw the game makes only once is lost if it
    // lands while its pipeline builds. In U3's mine, with a cold shader cache, the flashlight's
    // glow was missing for the whole session in 4 of 6 runs (0 of 4 with the threads off); the
    // background preload and the draw-preparation workers keep the stutter away without them
    // (same worst frames in those runs).
    const char* async_env = std::getenv("BB_ASYNC_SHADERS");
    if (async_env && async_env[0] == '1' && instance.IsVertexInputDynamicState()) {
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        const unsigned count = std::clamp(threads / 2, 1u, 6u);
        for (unsigned i = 0; i < count; ++i) {
            auto [result, worker_cache] = instance.GetDevice().createPipelineCacheUnique({});
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create compiler pipeline cache");
            worker_pipeline_caches.push_back(std::move(worker_cache));
        }
        compiler = std::make_unique<AsyncCompiler>(count, 64, [](unsigned index) {
            Common::SetCurrentThreadName(("bb:PipelineCompile" + std::to_string(index)).c_str());
        });
        std::printf("Pipelines: built on %u compiler threads (BB_ASYNC_SHADERS=1)\n", count);
    }
}

PipelineCache::~PipelineCache() {
    g_exit_pipeline_cache = nullptr;
    // The compiler threads use the device: they finish before anything is released.
    if (compiler) {
        compiler->Stop();
    }
    pending_graphics.clear();
    if (warm_thread.joinable()) {
        warm_thread.join();
    }
}

bool PipelineCache::AsyncCompilation() const {
    return compiler != nullptr;
}

void PipelineCache::PublishGraphics() {
    for (auto it = pending_graphics.begin(); it != pending_graphics.end();) {
        auto build = it.value();
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed() && !build->failure_logged) {
                std::printf("Pipelines: pipeline %#llx failed to build: %s\n",
                            static_cast<unsigned long long>(std::hash<GraphicsPipelineKey>{}(it.key())),
                            build->job.Error().c_str());
                build->failure_logged = true;
            }
            ++it;
            continue;
        }
        const auto key = it.key();
        // bbport: one the driver could not create stays a null entry and is not saved.
        if (build->pipeline->IsValid()) {
            build->pipeline->PublishStages(build->live_infos);
            RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), build->data);
        } else {
            build->pipeline.reset();
        }
        {
            std::unique_lock lock{pipelines_mutex}; // draw-preparation workers insert too
            graphics_pipelines.emplace(key, std::move(build->pipeline));
        }
        ++num_new_pipelines;
        it = pending_graphics.erase(it);
    }
}

size_t PipelineCache::DriverCacheSize() {
    size_t size = 0;
    if (pipeline_cache && instance.GetDevice().getPipelineCacheData(*pipeline_cache, &size,
                                                                   nullptr) != vk::Result::eSuccess) {
        return 0;
    }
    return size;
}

/// Writes the driver's pipeline cache beside the shader blobs (temp file, then rename).
void PipelineCache::SaveDriverCache() {
    if (driver_cache_path.empty() || !pipeline_cache) {
        return;
    }
    std::scoped_lock lock{driver_cache_mutex};
    driver_cache_dirty.store(0);
    auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess || data.empty()) {
        return;
    }
    auto temp = driver_cache_path;
    temp += ".tmp";
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, driver_cache_path, ec);
}

void PipelineCache::DriverCacheSaver(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:PipelineCacheSave");
    BbThreads::MakeBelowNormal();
    std::mutex mutex;
    std::condition_variable_any cv;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock{mutex};
            cv.wait_for(lock, stop, std::chrono::seconds(20), [] { return false; });
        }
        if (stop.stop_requested()) {
            break;
        }
        if (driver_cache_dirty.load() != 0 && IsWarm()) {
            SaveDriverCache();
        }
    }
}

void PipelineCache::SaveForExit() {
    // bbport: up to 30 s (was 3): right after a preparation thousands of shader files are still being written,
    // and a game closed then lost them. Usually nothing is pending and this returns at once.
    auto& db = Storage::DataBase::Instance();
    if (const u32 pending = db.PendingWrites(); pending > 0) {
        const auto start = std::chrono::steady_clock::now();
        db.Flush(30000);
        std::printf("Shader cache: %u writes finished at exit in %.1f s\n", pending,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        std::fflush(stdout);
    }
    if (driver_cache_dirty.load() != 0 && IsWarm()) {
        SaveDriverCache();
    }
}

void FlushPipelineCacheForExit() {
    if (auto* cache = g_exit_pipeline_cache) {
        cache->SaveForExit();
    }
}

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
/// bbport (BB_FRAME_STATS): where the compile time goes: GCN translation, SPIR-V emission,
/// shader module creation, compute pipelines and graphics pipelines built on the GPU thread.
std::atomic<u64> g_bb_translate_ns, g_bb_emit_ns, g_bb_module_ns, g_bb_cs_pipeline_ns,
    g_bb_gfx_pipeline_ns;
std::atomic<u32> g_bb_translations, g_bb_cs_pipelines, g_bb_gfx_pipelines;
static std::atomic<s64> g_last_compile_ms{-1000000};
static s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool PipelineCache::CompilingRecently() {
    return NowMs() - g_last_compile_ms.load(std::memory_order_relaxed) < 1000;
}
namespace {
struct SpanTimer {
    std::atomic<u64>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit SpanTimer(std::atomic<u64>& total_) : total{total_} {}
    ~SpanTimer() {
        total += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
    }
};
/// BB_COMPILE_LOG=1 prints one line per compile (kind, ms, thread, whether it ran on the GPU thread).
struct CompileTimer {
    const char* kind;
    u64 id;
    u32 extra;
    bool worker;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    CompileTimer(const char* kind_, u64 id_ = 0, u32 extra_ = 0)
        : kind{kind_}, id{id_}, extra{extra_}, worker{std::strstr(kind_, "-w") != nullptr} {
        g_last_compile_ms.store(NowMs(), std::memory_order_relaxed);
    }
    ~CompileTimer() {
        const auto ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
        // The frame statistics count what the GPU thread itself spent (workers: kind "...-w").
        if (!worker) {
            g_bb_compile_ns += ns;
            ++g_bb_compiles;
        }
        g_last_compile_ms.store(NowMs(), std::memory_order_relaxed);
        static const bool log = std::getenv("BB_COMPILE_LOG") != nullptr;
        if (log) {
            const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
            std::printf("Compile: %s %016llx %u %.2f ms at %lld\n", kind,
                        static_cast<unsigned long long>(id), extra, double(ns) / 1e6,
                        static_cast<long long>(wall));
        }
    }
};
} // namespace

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    if (!IsWarm()) {
        return false;
    }
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    auto& worker = *worker_sel.worker;
    worker.compile = worker_compile;
    if (!RefreshGraphicsKey(worker_sel) || worker.failed) {
        return false;
    }
    if (worker.compile) {
        EnsureWorkerPipeline(worker_sel);
    }
    return true;
}

/// bbport: a draw-preparation worker builds the pipeline of the draw it prepares when it does
/// not exist yet, so the GPU thread finds it (or waits for the worker's compile instead of
/// repeating it). The stages keep pointing at the programs' own Infos, which the GPU thread
/// refreshes per draw; the worker's copies only serve the construction.
void PipelineCache::EnsureWorkerPipeline(PipelineSelection& worker_sel) {
    auto& worker = *worker_sel.worker;
    // Without dynamic vertex input the constructor reads the vertex buffers' sharps.
    if (!instance.IsVertexInputDynamicState()) {
        return;
    }
    const auto& key = worker_sel.graphics_key;
    {
        std::shared_lock lock{pipelines_mutex};
        if (graphics_pipelines.contains(key)) {
            return;
        }
    }
    std::array<const Shader::Info*, MaxShaderStages> program_infos{};
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (!worker_sel.infos[i]) {
            continue;
        }
        for (const auto& [program, info] : worker.infos) {
            if (&info == worker_sel.infos[i]) {
                program_infos[i] = &program->info;
                break;
            }
        }
        if (!program_infos[i]) {
            return;
        }
    }
    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(key);
    const u64 claim_id = pipeline_hash ^ PipelineClaimTag;
    BbThreads::ScopedNormalPriority boost; // the GPU thread may wait for this claim
    AcquireClaim(claim_id);
    {
        std::shared_lock lock{pipelines_mutex};
        if (graphics_pipelines.contains(key)) {
            ReleaseClaim(claim_id);
            return;
        }
    }
    // A guest buffer the worker reads may be unmapped by now: the draw is left to the GPU thread.
    sigjmp_buf recover;
    const size_t claim_mark = ClaimMark();
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        ReleaseClaimsAfter(claim_mark);
        ReleaseClaim(claim_id);
        worker.failed = true;
        return;
    }
    runtime_fault_recover = &recover;
    {
        CompileTimer timer{"gpipe-w", pipeline_hash, key.motion_vectors};
        GraphicsPipeline::SerializationSupport sdata{};
        auto pipeline = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, key, *pipeline_cache, worker_sel.infos,
            worker_sel.runtime_infos, worker_sel.fetch_shader, worker_sel.modules, sdata, false);
        // bbport: a pipeline the driver could not create is kept as a null entry, not saved.
        if (pipeline->IsValid()) {
            pipeline->RebindStages(program_infos);
            RegisterPipelineData(key, pipeline_hash, sdata);
            num_new_pipelines.fetch_add(1, std::memory_order_relaxed);
            driver_cache_dirty.fetch_add(1, std::memory_order_relaxed);
        } else {
            pipeline.reset();
        }
        std::unique_lock lock{pipelines_mutex};
        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (worker_sel.infos[stage]) {
                    module_related_pipelines[worker_sel.modules[stage]].emplace_back(key);
                }
            }
        }
        graphics_pipelines.emplace(key, std::move(pipeline));
    }
    runtime_fault_recover = nullptr;
    ReleaseClaim(claim_id);
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready || !IsWarm() ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    std::shared_lock lock{pipelines_mutex};
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    EnsureWarm();
    if (!pending_graphics.empty()) {
        PublishGraphics();
    }
    used_prepared = nullptr;
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        return nullptr;
    }
    {
        std::shared_lock lock{pipelines_mutex};
        const auto it = graphics_pipelines.find(sel.graphics_key);
        if (it != graphics_pipelines.end()) {
            return it->second.get();
        }
    }
    if (AsyncCompilation() && !sel.regs->stage_enable.hs_en && !EmulatorSettings.IsShaderCollect()) {
        if (!pending_graphics.contains(sel.graphics_key) && compiler->HasCapacity()) {
            auto build = std::make_shared<AsyncGraphicsCompilation>();
            build->selection = sel;
            build->selection.regs = nullptr;
            build->live_infos = sel.infos;
            for (u32 i = 0; i < MaxShaderStages; ++i) {
                if (sel.infos[i]) {
                    build->infos[i] = std::make_unique<Shader::Info>(*sel.infos[i]);
                    build->user_data[i].assign(sel.infos[i]->user_data.begin(),
                                               sel.infos[i]->user_data.end());
                    build->infos[i]->user_data = build->user_data[i];
                    build->selection.infos[i] = build->infos[i].get();
                }
            }
            const bool queued = build->job.Start(*compiler, [this, build] {
                CompileTimer timer{"gpipe-w"}; // a compiler thread, not the GPU thread
                auto& s = build->selection;
                build->pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, s.graphics_key,
                    *worker_pipeline_caches[AsyncCompiler::WorkerIndex()], s.infos,
                    s.runtime_infos, s.fetch_shader, s.modules, build->data, false);
                return build;
            });
            if (queued) {
                pending_graphics.emplace(sel.graphics_key, std::move(build));
            }
        }
        // bbport diagnosis: BB_ASYNC_LOG=1 prints every 2 s how many draws waited for a pipeline
        // building on a compiler thread and how many builds are pending.
        static const bool async_log = std::getenv("BB_ASYNC_LOG") != nullptr;
        if (async_log) {
            static u64 skipped = 0;
            static auto last = std::chrono::steady_clock::now();
            ++skipped;
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(2)) {
                std::printf("Pipelines: %llu draws waited for a compiler thread, %zu pending\n",
                            static_cast<unsigned long long>(skipped), pending_graphics.size());
                skipped = 0;
                last = now;
            }
        }
        sel.fetch_shader.reset();
        return nullptr;
    }
    // A draw-preparation worker may be building it right now: wait for that instead of
    // repeating the compile.
    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    const u64 claim_id = pipeline_hash ^ PipelineClaimTag;
    AcquireClaim(claim_id);
    {
        std::shared_lock lock{pipelines_mutex};
        const auto it = graphics_pipelines.find(sel.graphics_key);
        if (it != graphics_pipelines.end()) {
            ReleaseClaim(claim_id);
            return it->second.get();
        }
    }
    LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
    {
        // bbport (feature/stutter): why the GPU thread builds this one itself: no preparation for the draw, a worker
        // still on it, a worker that failed, or a prepared pipeline that did not fit (BB_FRAME_STATS, every 5 s).
        static std::atomic<u64> none{0}, pending{0}, failed{0}, mismatch{0};
        const u32 state = prepared ? prepared->state.load(std::memory_order_acquire) : ~0u;
        (state == ~0u ? none : state == PreparedDraw::Pending ? pending
                              : state == PreparedDraw::Unavailable ? failed : mismatch) += 1;
        static std::atomic<s64> last{0};
        const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
        s64 seen = last.load();
        if (now - seen >= 5 && last.compare_exchange_strong(seen, now)) {
            std::printf("GPU-thread pipeline builds: %llu with no preparation, %llu a worker was still on, "
                        "%llu a worker failed, %llu prepared but not fitting\n",
                        static_cast<unsigned long long>(none.load()), static_cast<unsigned long long>(pending.load()),
                        static_cast<unsigned long long>(failed.load()), static_cast<unsigned long long>(mismatch.load()));
        }
    }
    CompileTimer timer{"gpipe", pipeline_hash, sel.graphics_key.motion_vectors};
    SpanTimer gfx_timer{g_bb_gfx_pipeline_ns};
    ++g_bb_gfx_pipelines;

    GraphicsPipeline::SerializationSupport sdata{};
    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);
    // bbport: the driver could not create it. A null entry keeps the key from being compiled
    // again on every draw; it is not saved, so the next start does not load it.
    if (pipeline->IsValid()) {
        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        num_new_pipelines.fetch_add(1, std::memory_order_relaxed);
        driver_cache_dirty.fetch_add(1, std::memory_order_relaxed);
    } else {
        pipeline.reset();
    }

    const GraphicsPipeline* result = nullptr;
    {
        std::unique_lock lock{pipelines_mutex};
        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        // The preload may have inserted the same one meanwhile: keep that.
        result = graphics_pipelines.try_emplace(sel.graphics_key, std::move(pipeline))
                     .first->second.get();
    }
    ReleaseClaim(claim_id);
    sel.fetch_shader.reset();
    return result;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    EnsureWarm();
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    {
        std::shared_lock lock{pipelines_mutex};
        const auto it = compute_pipelines.find(compute_key);
        if (it != compute_pipelines.end()) {
            return it->second.get();
        }
    }
    // A draw-preparation worker may be building it right now: wait for that instead of
    // repeating the compile.
    const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
    const u64 claim_id = pipeline_hash ^ PipelineClaimTag;
    AcquireClaim(claim_id);
    {
        std::shared_lock lock{pipelines_mutex};
        const auto it = compute_pipelines.find(compute_key);
        if (it != compute_pipelines.end()) {
            ReleaseClaim(claim_id);
            return it->second.get();
        }
    }
    LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
    CompileTimer timer{"cpipe", pipeline_hash, u32(liverpool->CurrentQueueId())};
    SpanTimer cs_timer{g_bb_cs_pipeline_ns};
    ++g_bb_cs_pipelines;

    ComputePipeline::SerializationSupport sdata{};
    auto pipeline = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                      *pipeline_cache, compute_key, *sel.infos[0],
                                                      sel.modules[0], sdata, false);
    // bbport: as for graphics pipelines, kept null and not saved.
    if (pipeline->IsValid()) {
        RegisterPipelineData(compute_key, sdata);
        num_new_pipelines.fetch_add(1, std::memory_order_relaxed);
        driver_cache_dirty.fetch_add(1, std::memory_order_relaxed);
    } else {
        pipeline.reset();
    }

    const ComputePipeline* result = nullptr;
    {
        std::unique_lock lock{pipelines_mutex};
        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
        // The preload may have inserted the same one meanwhile: keep that.
        result = compute_pipelines.try_emplace(compute_key, std::move(pipeline)).first->second.get();
    }
    ReleaseClaim(claim_id);
    return result;
}

/// bbport: a draw-preparation worker translates the compute program a dispatch will run and builds
/// its pipeline, like EnsureWorkerPipeline does for draws; the GPU thread then finds both.
void PipelineCache::PrepareComputePipeline(PipelineSelection& worker_sel) {
    if (!IsWarm() || !worker_sel.cs || !worker_sel.worker) {
        return;
    }
    auto& worker = *worker_sel.worker;
    worker.compile = worker_compile;
    if (!worker.compile) {
        return;
    }
    worker.stages.clear();
    worker.failed = false;
    // The program's code is read from guest memory, which may be reused by now.
    sigjmp_buf recover;
    const size_t claim_mark = ClaimMark();
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        ReleaseClaimsAfter(claim_mark);
        return;
    }
    runtime_fault_recover = &recover;
    const auto& cs_pgm = *worker_sel.cs;
    if (!cs_pgm.Address<u32*>()) {
        runtime_fault_recover = nullptr;
        return;
    }
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    runtime_fault_recover = nullptr;
    Shader::Backend::Bindings binding{};
    ComputePipelineKey key{};
    std::tie(worker_sel.infos[0], worker_sel.modules[0], worker_sel.fetch_shader, key.value) =
        GetProgram(worker_sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    if (worker.failed || !worker_sel.infos[0] || worker.stages.empty()) {
        return;
    }
    const Program* program = worker.stages.back().program;
    {
        std::shared_lock lock{pipelines_mutex};
        if (compute_pipelines.contains(key)) {
            return;
        }
    }
    const auto pipeline_hash = std::hash<ComputePipelineKey>{}(key);
    const u64 claim_id = pipeline_hash ^ PipelineClaimTag;
    BbThreads::ScopedNormalPriority boost; // the GPU thread may wait for this claim
    AcquireClaim(claim_id);
    {
        std::shared_lock lock{pipelines_mutex};
        if (compute_pipelines.contains(key)) {
            ReleaseClaim(claim_id);
            return;
        }
    }
    // The sharps of the program's buffers are read from guest memory.
    sigjmp_buf recover_build;
    const size_t build_mark = ClaimMark();
    if (sigsetjmp(recover_build, 0)) {
        runtime_fault_recover = nullptr;
        ReleaseClaimsAfter(build_mark);
        ReleaseClaim(claim_id);
        return;
    }
    runtime_fault_recover = &recover_build;
    {
        CompileTimer timer{"cpipe-w", pipeline_hash, u32(liverpool->CurrentQueueId())};
        ComputePipeline::SerializationSupport sdata{};
        auto pipeline = std::make_unique<ComputePipeline>(
            instance, scheduler, desc_heap, profile, *pipeline_cache, key, *worker_sel.infos[0],
            worker_sel.modules[0], sdata, false);
        if (pipeline->IsValid()) {
            pipeline->RebindStage(&program->info);
            RegisterPipelineData(key, sdata);
            num_new_pipelines.fetch_add(1, std::memory_order_relaxed);
            driver_cache_dirty.fetch_add(1, std::memory_order_relaxed);
        } else {
            pipeline.reset();
        }
        std::unique_lock lock{pipelines_mutex};
        if (EmulatorSettings.IsShaderCollect()) {
            module_related_pipelines[worker_sel.modules[0]].emplace_back(key);
        }
        compute_pipelines.try_emplace(key, std::move(pipeline));
    }
    runtime_fault_recover = nullptr;
    ReleaseClaim(claim_id);
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;
    sel.ui_squeeze = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }
    if (UiSqueezeDraw(regs, sel)) {
        sel.ui_squeeze = true;
        if (!RefreshGraphicsStages(sel)) {
            return false;
        }
        key.ui_squeeze = 1;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;
    // bbport (BB_GPL=1, study step 2a): every graphics pipeline uses the split layout.
    sel.split_sets = instance.IsGraphicsPipelineLibrary();
    key.split_sets = sel.split_sets ? 1u : 0u;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (sel.split_sets) {
        // The pre-raster stages get descriptor set 1 of their own, numbered from 0; the push data counters
        // (buffer offsets, user data) stay shared, as the push constant block is.
        binding.unified = 0;
        binding.descriptor_set = 1;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    // bbport: a draw whose vertex buffer table pointer is null (reported before a crash at the
    // cutscene after Rom and in normal play) has no vertex input to build: skipped. From
    // SuperMedo's 15a31fb7, via drakolordx7's community fork.
    if (vs_info && sel.fetch_shader) {
        for (const auto& attrib : sel.fetch_shader->attributes) {
            if (vs_info->UdPointerNull(attrib.sgpr_base)) {
                static std::atomic<u32> reported{0};
                if (reported.fetch_add(1, std::memory_order_relaxed) < 5) {
                    std::printf("GPU: draw skipped: its vertex buffer table pointer is null\n");
                }
                return false;
            }
        }
    }
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

bool BbMovieShaderScaled(u64 pgm_hash);
static std::vector<u32> ScaleMovieShader(std::span<const u32> code);

/// bbport (native batch): a profile for the translations of this thread (GetBatchedComputePipeline).
static thread_local const Shader::Profile* t_profile_override = nullptr;

const ComputePipeline* PipelineCache::GetBatchedComputePipeline(const ComputePipeline* base) {
    {
        std::shared_lock lock{pipelines_mutex};
        if (const auto it = batched_pipelines.find(base); it != batched_pipelines.end()) {
            return it->second.pipeline.get();
        }
    }
    if (!batched_profile.batched_compute) {
        batched_profile = profile;
        batched_profile.batched_compute = true;
    }
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto params = AmdGpu::GetParams(cs_pgm);
    BatchedVariant variant;
    variant.info = std::make_unique<Shader::Info>(HwStage::Compute, SwStage::Compute, params);
    auto runtime_info = BuildRuntimeInfo(sel, HwStage::Compute, SwStage::Compute);
    Shader::Backend::Bindings binding{};
    std::vector<u32> spv;
    t_profile_override = &batched_profile;
    const auto module =
        TranslateModule(*variant.info, runtime_info, params.code, 0xBA7C, binding, pools, spv);
    t_profile_override = nullptr;
    // The table is filled in the base program's buffer order: both must list the same buffers.
    const auto& base_info = base->GetStage(Shader::SwStage::Compute);
    bool same = variant.info->buffers.size() == base_info.buffers.size() &&
                variant.info->ud_mask.mask == base_info.ud_mask.mask;
    for (size_t i = 0; same && i < base_info.buffers.size(); ++i) {
        same = variant.info->buffers[i].buffer_type == base_info.buffers[i].buffer_type &&
               variant.info->buffers[i].is_written == base_info.buffers[i].is_written &&
               variant.info->buffers[i].is_formatted == base_info.buffers[i].is_formatted &&
               variant.info->buffers[i].used_types == base_info.buffers[i].used_types;
    }
    if (same && module) {
        ComputePipeline::SerializationSupport sdata{};
        const ComputePipelineKey key{HashCombine(params.hash, 0xBA7C4ED0ull)};
        variant.pipeline = std::make_unique<ComputePipeline>(
            instance, scheduler, desc_heap, batched_profile, *pipeline_cache, key, *variant.info,
            module, sdata, false);
        if (!variant.pipeline->IsValid()) {
            variant.pipeline.reset();
        }
    }
    std::printf("Native batch: cs %016llx batched variant %s\n",
                static_cast<unsigned long long>(params.hash),
                variant.pipeline ? "ready" : (same ? "failed to build" : "refused (buffers differ)"));
    std::unique_lock lock{pipelines_mutex};
    const auto [it, inserted] = batched_pipelines.try_emplace(base, std::move(variant));
    return it->second.pipeline.get();
}

const GraphicsPipeline* PipelineCache::GetBatchedGraphicsPipeline(const GraphicsPipeline* base) {
    if (base->batch_tried.load(std::memory_order_relaxed)) {
        return base->batch_variant.load(std::memory_order_acquire);
    }
    base->batch_tried.store(true, std::memory_order_relaxed);
    const auto& stages = base->GetStages();
    const auto* base_vs = stages[u32(SwStage::Vertex)];
    const auto* base_fs = stages[u32(SwStage::Fragment)];
    const char* refused = nullptr;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (stages[i] && i != u32(SwStage::Vertex) && i != u32(SwStage::Fragment)) {
            refused = "other stages";
        }
    }
    const auto& regs = liverpool->regs;
    if (!refused && (!base_vs || !base_fs)) {
        refused = "no vertex or fragment shader";
    } else if (!refused && base->GetFetchShader()) {
        refused = "fetch shader (needs BB_INLINE_FETCH=1)";
    } else if (!refused && regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        refused = "not VS+PS";
    } else if (!refused && base_vs->stores.GetAny(Shader::IR::Attribute::Param0 +
                                                 Shader::Profile::BatchDrawLocation)) {
        refused = "location taken";
    }
    if (refused) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1) < 20) {
            std::printf("Draw batch: pipeline %016llx not batched (%s)\n",
                        static_cast<unsigned long long>(base_vs ? base_vs->pgm_hash : 0), refused);
        }
        return nullptr;
    }
    if (!batched_graphics_profile.batched_graphics) {
        batched_graphics_profile = profile;
        batched_graphics_profile.batched_graphics = true;
    }
    PipelineSelection bsel{};
    bsel.regs = &regs;
    bsel.graphics_key = base->GetGraphicsKey();
    bsel.motion = bsel.graphics_key.motion_vectors != 0;
    bsel.ui_squeeze = bsel.graphics_key.ui_squeeze != 0;
    BatchedGraphicsVariant variant;
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Backend::Bindings binding{};
    std::vector<u32> spv;
    std::array<bool, 2> added{};
    bool same = true;
    // The fragment shader first, as RefreshGraphicsStages binds them (one binding counter).
    const std::array<std::pair<HwStage, SwStage>, 2> order{
        std::pair{HwStage::Fragment, SwStage::Fragment}, std::pair{HwStage::Vertex, SwStage::Vertex}};
    for (u32 k = 0; k < 2 && same; ++k) {
        const auto [hw, sw] = order[k];
        const auto* pgm = regs.ProgramForStage(u32(hw));
        const auto params = AmdGpu::GetParams(*pgm);
        auto& info = variant.infos[k];
        info = std::make_unique<Shader::Info>(hw, sw, params);
        auto runtime_info = BuildRuntimeInfo(bsel, hw, sw);
        t_profile_override = &batched_graphics_profile;
        modules[u32(sw)] =
            TranslateModule(*info, runtime_info, params.code, 0xBA7D, binding, pools, spv);
        t_profile_override = nullptr;
        // The table is filled in the normal variant's buffer order: the batched one lists the
        // same buffers, plus at most a flat buffer at the end.
        const auto& base_info = *stages[u32(sw)];
        added[k] = info->buffers.size() == base_info.buffers.size() + 1;
        same = modules[u32(sw)] && info->ud_mask.mask == base_info.ud_mask.mask &&
               info->images.size() == base_info.images.size() &&
               info->samplers.size() == base_info.samplers.size() &&
               (info->buffers.size() == base_info.buffers.size() || added[k]);
        for (size_t i = 0; same && i < base_info.buffers.size(); ++i) {
            same = info->buffers[i].buffer_type == base_info.buffers[i].buffer_type &&
                   info->buffers[i].is_written == base_info.buffers[i].is_written &&
                   info->buffers[i].is_formatted == base_info.buffers[i].is_formatted;
        }
        infos[u32(sw)] = info.get();
    }
    if (same) {
        GraphicsPipeline::SerializationSupport sdata{};
        variant.pipeline = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, batched_graphics_profile, bsel.graphics_key,
            *pipeline_cache, infos, bsel.runtime_infos, std::nullopt, modules, sdata, false);
        if (!variant.pipeline->IsValid()) {
            variant.pipeline.reset();
        } else {
            variant.pipeline->batch_added_flatbuf = added;
        }
    }
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 40) {
        std::printf("Draw batch: vs %016llx ps %016llx batched variant %s\n",
                    static_cast<unsigned long long>(base_vs->pgm_hash),
                    static_cast<unsigned long long>(base_fs->pgm_hash),
                    variant.pipeline ? "ready" : (same ? "failed to build" : "refused (resources differ)"));
    }
    const GraphicsPipeline* result = variant.pipeline.get();
    if (result) {
        std::unique_lock lock{pipelines_mutex};
        batched_graphics.push_back(std::move(variant));
    }
    base->batch_variant.store(result, std::memory_order_release);
    return result;
}

vk::ShaderModule PipelineCache::TranslateModule(Shader::Info& info,
                                                Shader::RuntimeInfo& runtime_info,
                                                const std::span<const u32>& code, size_t perm_idx,
                                                Shader::Backend::Bindings& binding,
                                                Shader::Pools& translate_pools,
                                                std::vector<u32>& spv) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");

    ++g_bb_translations;
    auto translate_timer = std::make_optional<SpanTimer>(g_bb_translate_ns);
    std::vector<u32> scaled_code;
    std::span<const u32> program_code = code;
    if (BbMovieShaderScaled(info.pgm_hash)) {
        scaled_code = ScaleMovieShader(code);
        program_code = scaled_code;
    }
    // bbport (native batch): the batched variant translates with its own profile.
    const Shader::Profile& translate_profile = t_profile_override ? *t_profile_override : profile;
    const auto ir_program = Shader::TranslateProgram(program_code, translate_pools, info,
                                                     runtime_info, translate_profile);
    translate_timer.reset();
    {
        SpanTimer emit_timer{g_bb_emit_ns};
        spv = Shader::Backend::SPIRV::EmitSPIRV(translate_profile, runtime_info, ir_program,
                                                binding);
    }
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;
    SpanTimer module_timer{g_bb_module_ns};

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    t_spv_program_hash = info.pgm_hash;
    t_spv_permutation = u32(perm_idx);
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

/// bbport: BB_RENDER_RES (runtime_ultrawide.c) renders U3 at another size, but its movie
/// compute shader (YUV to RGB over the display buffer, c85d9816) samples the 1920x1080 movie at
/// (x + 0.5) / 1920, (y + 0.5) / 1088 with those reciprocals as literals: they become the render
/// size's, so the movie fills the frame. That shader is then neither loaded from nor saved to
/// the shader cache (its translation depends on the size).
static bool MovieShaderSize(u32* width, u32* height) {
    static u32 w = 0, h = 0;
    static const bool on = [] {
        const char* res = std::getenv("BB_RENDER_RES");
        unsigned a = 0, b = 0;
        if (!res || std::sscanf(res, "%ux%u", &a, &b) != 2 || a < 640 || b < 360 || a > 7680 ||
            b > 4320) {
            return false;
        }
        w = a & ~15u;
        h = b & ~7u;
        return w != 1920 || h != 1080;
    }();
    if (on && width) {
        *width = w;
        *height = h;
    }
    return on;
}

bool BbMovieShaderScaled(u64 pgm_hash) {
    return u32(pgm_hash) == 0xc85d9816u && MovieShaderSize(nullptr, nullptr);
}

static std::vector<u32> ScaleMovieShader(std::span<const u32> code) {
    std::vector<u32> out(code.begin(), code.end());
    u32 w = 1920, h = 1080;
    MovieShaderSize(&w, &h);
    const float vh = float(h) * 1088.0f / 1080.0f;
    const struct {
        float from, to;
    } literals[] = {{1.0f / 1920.0f, 1.0f / float(w)},
                    {1.0f / 3840.0f, 0.5f / float(w)},
                    {1.0f / 1088.0f, 1.0f / vh},
                    {1.0f / 2176.0f, 0.5f / vh}};
    int replaced = 0;
    for (auto& word : out) {
        for (const auto& l : literals) {
            u32 from, to;
            std::memcpy(&from, &l.from, 4);
            std::memcpy(&to, &l.to, 4);
            if (word == from) {
                // One replacement per word: at 3840x2160 1/1920 becomes 1/3840, which the next
                // literal matched again (0.5/3840): movies showed 2x zoomed (issue #3).
                word = to;
                ++replaced;
                break;
            }
        }
    }
    std::printf("Render resolution: movie shader scaled to %ux%u (%d constants)\n", w, h, replaced);
    return out;
}

/// bbport: whether a shader is saved to the shader cache.
static bool SaveShaderBinary(u64 pgm_hash, Shader::HwStage stage) {
    return !Shader::Backend::SPIRV::IsForcedShader(pgm_hash) && !BbMovieShaderScaled(pgm_hash) &&
           !BbNanGuard::BloomClampShader(pgm_hash) &&
           !BbFocusFix::FocusCentreShader(pgm_hash) &&
           !(Shader::VertexCheck::address && stage == Shader::HwStage::Vertex);
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              Shader::Pools& translate_pools) {
    // Draw-preparation workers translate with their own pools ("-w": not the GPU thread).
    const bool motion = (info.hw_stage == HwStage::Vertex && runtime_info.hw.vs.motion_vectors) ||
                        (info.hw_stage == HwStage::Fragment && runtime_info.hw.fs.motion_vectors);
    CompileTimer timer{&translate_pools == &pools ? "shader" : "shader-w", info.pgm_hash,
                       u32(perm_idx) | (u32(info.hw_stage) << 16) | (motion ? 0x80000000u : 0u)};
    std::vector<u32> spv;
    // bbport (shader variant list): the reads this translation makes through user-data table pointers are
    // recorded and stored with the variant's meta, with its user data, so it can be translated again from the
    // code alone (TranslateFromRecord). Thread-local storage: a fault jumping out leaves nothing dangling.
    static thread_local std::vector<Shader::UdRead> reads;
    reads.clear();
    Shader::t_translation_record = &reads;
    const auto module =
        TranslateModule(info, runtime_info, code, perm_idx, binding, translate_pools, spv);
    Shader::t_translation_record = nullptr;
    // The inputs are kept also for the shaders whose binary depends on settings (the scaled movie shader, the
    // clamped bloom shader, ...): a variant list then holds them, and preparation translates them for the current
    // settings, as the game would at first use.
    RegisterShaderInputs(info, reads, perm_idx);
    if (SaveShaderBinary(info.pgm_hash, info.hw_stage)) {
        RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
    }
    return module;
}

// bbport: claims this thread holds, in order (see ReleaseClaimsAfter).
static thread_local std::vector<u64> t_held_claims;

void PipelineCache::AcquireClaim(u64 id) {
    const auto self = std::this_thread::get_id();
    std::unique_lock lock{claim_mutex};
    if (const auto held = claims.find(id); held != claims.end() && held->second == self) {
        // Left behind on this thread by a jump out of an earlier build (it would wait for
        // itself forever): take it over.
        std::printf("Claim: %016llx was still held by this thread, taken over\n",
                    static_cast<unsigned long long>(id));
        std::erase(t_held_claims, id);
        claims.erase(held);
    }
    if (claims.contains(id)) {
        // Somebody else is building it: the time spent waiting is a stall of the caller.
        const auto start = std::chrono::steady_clock::now();
        while (!claim_cv.wait_for(lock, std::chrono::seconds(1),
                                  [&] { return !claims.contains(id); })) {
            const auto owner = claims.find(id);
            std::printf("Claim wait: %016llx held for over %lld ms (%s thread; holder %08x)\n",
                        static_cast<unsigned long long>(id),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - start)
                                                   .count()),
                        BbThreads::IsBackground() ? "worker" : "GPU",
                        owner != claims.end()
                            ? static_cast<u32>(std::hash<std::thread::id>{}(owner->second))
                            : 0u);
        }
        const auto ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
        if (!BbThreads::IsBackground()) {
            g_bb_compile_ns += ns;
        }
        static const bool log = std::getenv("BB_COMPILE_LOG") != nullptr;
        if (log && ns > 200000) {
            std::printf("Compile: wait%s %016llx 0 %.2f ms\n", BbThreads::IsBackground() ? "-w" : "",
                        static_cast<unsigned long long>(id), double(ns) / 1e6);
        }
    }
    claims.emplace(id, self);
    t_held_claims.push_back(id);
}

void PipelineCache::ReleaseClaim(u64 id) {
    {
        std::scoped_lock lock{claim_mutex};
        claims.erase(id);
    }
    if (const auto it = std::ranges::find(t_held_claims.rbegin(), t_held_claims.rend(), id);
        it != t_held_claims.rend()) {
        t_held_claims.erase(std::next(it).base());
    }
    claim_cv.notify_all();
}

size_t PipelineCache::ClaimMark() {
    return t_held_claims.size();
}

void PipelineCache::ReleaseClaimsAfter(size_t mark) {
    while (t_held_claims.size() > mark) {
        const u64 id = t_held_claims.back();
        std::printf("Claim: %016llx released after a fault\n", static_cast<unsigned long long>(id));
        ReleaseClaim(id);
    }
}

Program* PipelineCache::FindProgram(u64 hash) {
    std::shared_lock lock{programs_mutex};
    const auto it = program_cache.find(hash);
    return it != program_cache.end() ? it.value().get() : nullptr;
}

/// Translates a program seen for the first time (permutation 0) and publishes it. The caller
/// holds the program's claim; `binding` is not advanced.
PipelineCache::NewProgram PipelineCache::CreateProgram(HwStage hw_stage, SwStage sw_stage,
                                                       const Shader::ShaderParams& params,
                                                       Shader::RuntimeInfo& runtime_info,
                                                       Shader::Backend::Bindings& binding,
                                                       Shader::Pools& translate_pools) {
    auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
    auto start = binding;
    const auto module = CompileModule(new_program->info, runtime_info, params.code, 0, binding,
                                      translate_pools);
    auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
    const auto perm_hash = HashCombine(params.hash, 0);

    RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, 0);
    NewProgram result{nullptr, module, spec.fetch_shader_data, perm_hash};
    new_program->AddPermut(module, std::move(spec));
    new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
    result.program = new_program.get();
    std::unique_lock lock{programs_mutex};
    program_cache.emplace(params.hash, std::move(new_program));
    return result;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // bbport: draw-preparation worker, with the worker's own Info copy. A program or
        // permutation that does not exist yet is translated here (the GPU thread then finds
        // it ready) when the worker may compile; otherwise the draw is left to the GPU thread.
        auto& worker = *sel.worker;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const Program* program = nullptr;
            {
                std::shared_lock lk{programs_mutex};
                const auto found_program = program_cache.find(params.hash);
                if (found_program != program_cache.end() && found_program->second->info_template) {
                    program = found_program->second.get();
                }
            }
            if (!program) {
                if (!worker.compile || !WorkerTranslate(worker, hw_stage, sw_stage, params,
                                                        runtime_info, binding, nullptr)) {
                    worker.failed = true;
                    return {};
                }
                continue;
            }
            auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
            auto& info = it_info->second;
            info.pgm_base = params.Base();
            info.user_data = params.user_data;
            // The walk of resource tables and the fetch shader parse read guest memory through
            // pointers in the registers; ahead of the GPU thread that memory may already be
            // reused. A fault returns here (runtime_fault_recover) and the draw is left to the
            // GPU thread. A jump out of the specialization leaks its partial allocations (rare).
            sigjmp_buf recover;
            const size_t claim_mark = ClaimMark();
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                ReleaseClaimsAfter(claim_mark);
                worker.failed = true;
                return {};
            }
            runtime_fault_recover = &recover;
            info.RefreshFlatBuf();
            auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
            runtime_fault_recover = nullptr;
            size_t perm_idx = 0;
            vk::ShaderModule module{};
            std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
            bool found = false;
            {
                std::shared_lock lk{programs_mutex};
                const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
                if (it != program->modules.end()) {
                    found = true;
                    perm_idx = std::distance(program->modules.begin(), it);
                    module = it->module;
                    fetch_shader = it->spec.fetch_shader_data;
                }
            }
            if (!found) {
                if (!worker.compile || !WorkerTranslate(worker, hw_stage, sw_stage, params,
                                                        runtime_info, binding, &spec)) {
                    worker.failed = true;
                    return {};
                }
                continue;
            }
            info.AddBindings(binding);
            worker.stages.push_back(
                {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
            return std::make_tuple(&info, module, fetch_shader,
                                   HashCombine(params.hash, perm_idx));
        }
        worker.failed = true;
        return {};
    }

    Program* program = FindProgram(params.hash);
    if (!program) {
        AcquireClaim(params.hash);
        program = FindProgram(params.hash);
        if (!program) {
            const auto created = CreateProgram(hw_stage, sw_stage, params, runtime_info, binding,
                                               pools);
            ReleaseClaim(params.hash);
            return std::make_tuple(&created.program->info, created.module, created.fetch_shader,
                                   created.perm_hash);
        }
        ReleaseClaim(params.hash);
    }

    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = 0;
    vk::ShaderModule module{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
    bool found = false;
    {
        std::shared_lock lk{programs_mutex};
        // bbport: consecutive draws of a program almost always use the same permutation.
        auto it = program->last_used < program->modules.size() &&
                          program->modules[program->last_used].spec == spec
                      ? program->modules.begin() + program->last_used
                      : std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it != program->modules.end()) {
            found = true;
            program->last_used = std::distance(program->modules.begin(), it);
            perm_idx = program->last_used;
            module = it->module;
            fetch_shader = it->spec.fetch_shader_data;
        }
    }
    if (!found) {
        AcquireClaim(params.hash);
        {
            std::shared_lock lk{programs_mutex};
            const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
            if (it != program->modules.end()) {
                found = true;
                perm_idx = std::distance(program->modules.begin(), it);
                module = it->module;
                fetch_shader = it->spec.fetch_shader_data;
            }
        }
        if (!found) {
            // Only the holder of the claim appends, so the size is the new permutation's index.
            perm_idx = program->modules.size();
            const u64 perm_hash = HashCombine(params.hash, perm_idx);
            auto new_info = Shader::Info(hw_stage, sw_stage, params);
            module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding, pools);
            fetch_shader = spec.fetch_shader_data;

            RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
            std::unique_lock lk{programs_mutex};
            program->AddPermut(module, std::move(spec));
            ReleaseClaim(params.hash);
            return std::make_tuple(&program->info, module, fetch_shader, perm_hash);
        }
        ReleaseClaim(params.hash);
    }
    info.AddBindings(binding);
    return std::make_tuple(&program->info, module, fetch_shader, HashCombine(params.hash, perm_idx));
}

/// bbport: a draw-preparation worker translates a missing program (`spec` null) or permutation
/// of `params` for the GPU thread to find. The worker's copy of the Info supplies the specialization.
bool PipelineCache::WorkerTranslate(PrepWorker& worker, HwStage hw_stage, SwStage sw_stage,
                                    const Shader::ShaderParams& params,
                                    Shader::RuntimeInfo& runtime_info,
                                    const Shader::Backend::Bindings& binding,
                                    Shader::StageSpecialization* spec) {
    if (!worker.pools) {
        worker.pools = std::make_unique<Shader::Pools>();
    }
    BbThreads::ScopedNormalPriority boost; // the GPU thread may wait for this claim
    sigjmp_buf recover;
    AcquireClaim(params.hash);
    const size_t claim_mark = ClaimMark();
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        ReleaseClaimsAfter(claim_mark);
        ReleaseClaim(params.hash);
        return false;
    }
    runtime_fault_recover = &recover;
    auto local_binding = binding; // the lookup that follows advances the real one
    Program* program = FindProgram(params.hash);
    if (!program) {
        CreateProgram(hw_stage, sw_stage, params, runtime_info, local_binding, *worker.pools);
    } else if (spec) {
        bool exists = false;
        {
            std::shared_lock lk{programs_mutex};
            exists = std::ranges::find(program->modules, *spec, &Program::Module::spec) !=
                     program->modules.end();
        }
        if (!exists) {
            const size_t perm_idx = program->modules.size();
            const u64 perm_hash = HashCombine(params.hash, perm_idx);
            auto new_info = Shader::Info(hw_stage, sw_stage, params);
            const auto module = CompileModule(new_info, runtime_info, params.code, perm_idx,
                                              local_binding, *worker.pools);
            const auto& info = worker.infos.at(program);
            RegisterShaderMeta(info, spec->fetch_shader_data, *spec, perm_hash, perm_idx);
            std::unique_lock lk{programs_mutex};
            Shader::StageSpecialization stored{*spec};
            stored.info = &program->info;
            program->AddPermut(module, std::move(stored));
        }
    }
    runtime_fault_recover = nullptr;
    ReleaseClaim(params.hash);
    return true;
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    std::scoped_lock replace_lock{programs_mutex, pipelines_mutex};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
