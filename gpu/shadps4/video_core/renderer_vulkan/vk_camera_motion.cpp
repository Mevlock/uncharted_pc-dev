// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_settings.h"
#include "bbport_toggles.h"
extern "C" float* runtime_projection_jitter_slot; // runtime_ultrawide.c, or null
// The game's frame number the game side steps the jitter by (its draw script's frame check), or null.
extern "C" const volatile uint32_t* runtime_projection_jitter_frame;
extern "C" float* runtime_projection_jitter_table;
// The game's frame time (world step) per frame number (runtime_ultrawide.c, written by probe.c's frame start hook).
extern "C" volatile uint32_t* runtime_projection_jitter_phases;
namespace Libraries::VideoOut {
extern std::atomic<s64> g_gpu_flip_arg;
}
#include "shader_recompiler/runtime_info.h"

#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
    u32 pad;
    /// xyz: previous clip offset of the stencil-selected pixels (mode bits 8/9), w: stencil
    /// pixels per motion pixel (camera_motion.comp).
    std::array<float, 4> stencil_offset;
};
static_assert(sizeof(PushConstants) == 128, "camera_motion.comp push constants (128 bytes at most)");

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const char* env = std::getenv("BB_DEBUG_MOTION");
    debug_overlay = env && env[0] == '1';
    const char* upscaler = std::getenv("BB_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless BB_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    for (const char* list = std::getenv("BB_VELOCITY_PS"); list && *list;) {
        char* end = nullptr;
        const u64 hash = std::strtoull(list, &end, 16);
        if (end == list) {
            break;
        }
        velocity_shaders.push_back(u64(u32(hash)));
        list = *end == ',' ? end + 1 : end;
    }
    velocity_mode = for_upscaler && !velocity_shaders.empty();
    if (velocity_mode) {
        std::printf("Camera motion: from the game's velocity pass (%zu shaders)\n",
                    velocity_shaders.size());
        // bbport (from BilalReloaded's Uncharted port, #84): characters' own motion (skinning,
        // animation) from their vertices; decided by the rasterizer before the pipeline warm-up
        // (Shader::MotionVectors::characters), which prepares the G-buffer pipelines' motion twins.
        // Off by default (the setting "Character motion vectors"). Off: characters get the camera's motion.
        std::printf("Camera motion: character motion vectors %s\n",
                    Shader::MotionVectors::characters ? "on" : "off");
    }
    const auto device = instance.GetDevice();
    if (for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 4> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 3,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
    if (!debug_overlay) {
        return;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    const auto module = CompileSPV(CAMERA_MOTION_DEBUG_COMP, device);
    overlay_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    for (auto& frame : frames) {
        frame = std::make_unique<VideoCore::Buffer>(instance, 0, 3840ull * 2160 * 4,
                                                    VideoCore::MemoryType::DeviceLocal);
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    // FSR quality fix 6 (2026-11-22): velocity mode (Uncharted) never fills `current` (the far = 3000
    // camera block is Bloodborne's), so the upscalers always got the fixed 1.0. The velocity pass's
    // view from uv: view y = z (b v + d) with v 0..1 across the screen, so tan(fov / 2) = |b| / 2.
    // BB_UPSCALER_REAL_FOV=0: the fixed value.
    static const bool real_fov = [] {
        const char* env = std::getenv("BB_UPSCALER_REAL_FOV");
        return !(env && env[0] == '0');
    }();
    if (real_fov && velocity_mode && velocity_camera.valid) {
        const float b = std::abs(velocity_camera.uv_to_view[1]);
        const float fov = 2.0f * std::atan(0.5f * b);
        if (std::isfinite(fov) && fov > 0.1f && fov < 3.0f) {
            static bool noted = false;
            if (!noted) {
                noted = true;
                std::printf("Camera motion: the game's vertical field of view for the upscalers: %.3f rad "
                            "(%.1f degrees; BB_UPSCALER_REAL_FOV=0: 1.0)\n", fov, fov * 57.29578f);
            }
            return fov;
        }
    }
    if (current.proj[1] == 0.0f) {
        return 1.0f; // no camera constants (velocity mode): a typical 57 degrees
    }
    return 2.0f * std::atan(1.0f / current.proj[1]);
}

// The game's planes in velocity mode (Uncharted): the velocity pass turns depth into view z as z = y / (depth - x)
// (depth_to_z): the near plane is at depth 0, the far plane at depth 1. FSR 3.1 and FSR 4 turn depth into metres
// with them (small-motion cut-off, the tighter history clamp near the camera, disocclusion); they were handed
// fixed 0.1 and 10000 (3000). BB_UPSCALER_REAL_PLANES=0: the fixed values. `fixed` when they are not known.
static bool RealPlanes() {
    static const bool on = [] {
        const char* env = std::getenv("BB_UPSCALER_REAL_PLANES");
        return !(env && env[0] == '0');
    }();
    return on;
}

float CameraMotion::Far(float fixed) const noexcept {
    if (velocity_mode && planes_known && RealPlanes()) {
        const float x = planes[0], y = planes[1];
        const float z = std::abs(y / (1.0f - x));
        if (std::isfinite(z) && z > 1.0f) {
            return std::min(z, 1.0e6f);
        }
    }
    return fixed;
}

float CameraMotion::Near() const noexcept {
    if (velocity_mode && planes_known && RealPlanes()) {
        const float x = planes[0], y = planes[1];
        const float z = x != 0.0f ? std::abs(y / x) : 0.0f;
        if (std::isfinite(z) && z > 1.0e-4f && z < 100.0f) {
            static bool noted = false;
            if (!noted) {
                noted = true;
                std::printf("Camera motion: the game's planes for the upscalers: near %.4f, far %.1f "
                            "(BB_UPSCALER_REAL_PLANES=0: 0.1 and 10000 / 3000)\n", z, Far(10000.0f));
            }
            return z;
        }
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("Camera motion: no near plane from the velocity pass's depth constants (%g, %g): "
                        "the fixed planes for the upscalers\n", x, y);
        }
    }
    if (current.proj[2] == 0.0f) {
        return 0.1f;
    }
    // depth = zs + zo / z is 0 at the near plane.
    return -current.proj[3] / current.proj[2];
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    // Velocity mode (Uncharted): the camera of the game's velocity pass, in the form taa.comp reads
    // (view x, y = ndc z / proj.xy; z = proj.w / (depth - proj.z); the previous view z = previous_z .
    // (view, 1); the previous depth = prev_proj.z + prev_proj.w / z). The pass gives view = z (c0 u +
    // c2, c1 v + c3, 1) and z = c9 / (depth - c8), its previous clip w row the previous view z.
    // Before, TAA got the other mode's camera here (never filled in velocity mode): its test of the
    // history's depth failed everywhere, no history was kept, and the picture was each jittered
    // frame alone (U3 street still view: 60-frame std p50 1.19 with either jitter path).
    if (velocity_mode && velocity_camera.valid) {
        const auto& c = velocity_camera;
        const float sx = c.uv_to_view[0] != 0.0f ? 2.0f / c.uv_to_view[0] : 1.0f;
        const float sy = c.uv_to_view[1] != 0.0f ? -2.0f / c.uv_to_view[1] : 1.0f;
        const std::array<float, 4> proj{sx, sy, c.depth_to_z[0], c.depth_to_z[1]};
        return {proj, proj,
                {c.to_previous_clip[8], c.to_previous_clip[9], c.to_previous_clip[10],
                 c.to_previous_clip[11]}};
    }
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : vk::Image{};
}

void CameraMotion::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView motion_view, u32 width, u32 height) {
    bool object_valid = false;
    const vk::ImageView object_view = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(cmdbuf, width, height, object_valid) : depth_view;
    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = object_valid ? 1u : 0u,
        .pad = 0,
        .stencil_offset = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const vk::DescriptorImageInfo stencil_info{.imageView = ZeroStencil(cmdbuf),
                                               .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &stencil_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

bool CameraMotion::IsVelocityShader(u64 ps_hash) const noexcept {
    return std::find(velocity_shaders.begin(), velocity_shaders.end(), u64(u32(ps_hash))) !=
           velocity_shaders.end();
}

// bbport: the heading (degrees) of the camera the game last rendered with, and when (seconds,
// steady clock).
static std::atomic<float> shown_camera_yaw{0.0f};
static std::atomic<double> shown_camera_time{0.0};

extern "C" int bbgpu_camera_yaw(float* yaw_degrees, double* age_seconds) {
    const double t = shown_camera_time.load(std::memory_order_acquire);
    if (t == 0.0) {
        return 0;
    }
    *yaw_degrees = shown_camera_yaw.load(std::memory_order_relaxed);
    *age_seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now().time_since_epoch()).count() - t;
    return 1;
}

/// bbport diagnosis (Windows, game started under RenderDoc): BB_RDOC_CUT=N[,delay] records N
/// frames in a row, starting delay frames after the next camera cut once F11 has armed it. One-frame
/// glitches just after a cut (U2's green haze in the museum) are too quick to catch with F12.
extern "C" double runtime_time_scale_now(void);

static void RenderDocOnCut(bool cut) {
#ifdef _WIN32
    static const std::pair<u32, u32> setting = [] {
        const char* env = std::getenv("BB_RDOC_CUT");
        if (!env) {
            return std::pair<u32, u32>{0, 0};
        }
        char* end;
        const u32 frames = static_cast<u32>(std::strtoul(env, &end, 10));
        return std::pair<u32, u32>{frames, *end == ',' ? static_cast<u32>(std::atoi(end + 1)) : 0};
    }();
    if (!setting.first) {
        return;
    }
    using TriggerMulti = void(__cdecl*)(u32);
    static TriggerMulti trigger = [] {
        // RENDERDOC_API_1_6_0: TriggerMultiFrameCapture is its 23rd function pointer.
        const HMODULE module = GetModuleHandleA("renderdoc.dll");
        using GetApi = int(__cdecl*)(int, void**);
        auto get_api =
            module ? reinterpret_cast<GetApi>(::GetProcAddress(module, "RENDERDOC_GetAPI"))
                   : nullptr;
        void** api = nullptr;
        if (!get_api || get_api(10600, reinterpret_cast<void**>(&api)) != 1 || !api) {
            std::printf("BB_RDOC_CUT: not running under RenderDoc\n");
            return TriggerMulti{};
        }
        std::printf("BB_RDOC_CUT: press F11, then the next camera cut records %u frames\n",
                    setting.first);
        return reinterpret_cast<TriggerMulti>(api[22]);
    }();
    static bool armed = false;
    static int countdown = -1;
    static u64 frame = 0, last_cut = 0;
    if (!trigger) {
        return;
    }
    ++frame;
    if (cut) {
        last_cut = frame;
    }
    // Slow motion switched on (BB_TIME_SCALE_KEY: F10) arms it too.
    static double last_scale = 1.0;
    const double scale = runtime_time_scale_now();
    if (scale < 1.0 && last_scale >= 1.0 && !armed && countdown < 0) {
        armed = true;
        std::printf("BB_RDOC_CUT: armed by slow motion\n");
    }
    last_scale = scale;
    if (GetAsyncKeyState(0x7A) & 0x8000) { // F11 held
        if (!armed && countdown < 0) {
            std::printf("BB_RDOC_CUT: armed (%llu frames after the last cut)\n",
                        static_cast<unsigned long long>(frame - last_cut));
        }
        armed = countdown < 0;
    }
    if (armed && cut) {
        armed = false;
        countdown = static_cast<int>(setting.second);
        std::printf("BB_RDOC_CUT: camera cut, recording %u frames after %u\n", setting.first,
                    setting.second);
    }
    if (countdown == 0) {
        trigger(setting.first);
    }
    if (countdown >= 0) {
        --countdown;
    }
#else
    (void)cut;
#endif
}

/// The frame the GPU thread draws now: the one after the last flip it reached (the games pass their frame number as the
/// flip argument), or -1.
static s64 DrawnFrame() {
    const s64 flip = Libraries::VideoOut::g_gpu_flip_arg.load(std::memory_order_relaxed);
    return flip < 0 ? -1 : flip + 1;
}

std::array<float, 2> CameraMotion::GiveProjectionJitter() noexcept {
    const s64 frame = DrawnFrame();
    std::array<float, 2> j = projection_jitter;
    // A frame of the game without the velocity pass (its jitter was not read): the game side's value for its number.
    if (frame >= 0 && frame != projection_frame && runtime_projection_jitter_frame && runtime_projection_jitter_table &&
        runtime_projection_jitter_phases) {
        const u32 n = std::clamp(u32(*runtime_projection_jitter_phases), 1u, 128u);
        const float* t = runtime_projection_jitter_table + (u64(frame) % n) * 4;
        j = {t[0], t[1]};
        ++given_without_pass;
    }
    given_jitter = j;
    given_frame = frame;
    return j;
}

void CameraMotion::OnVelocityConstants(const float* c, u32 dwords) {
    // Uncharted's camera velocity pass (its pixel shader, read from the shader dump):
    //   z = c9 / (depth - c8), view = (z (c0 u + c2), z (c1 v + c3), z)
    //   p = M view + t (M, t: rows c12..c23; pixels with stencil bits 0xc0 add c56..c58)
    //   previous clip x, y, w = rows c40..c43, c44..c47, c52..c55 applied to (p, 1)
    //   previous uv = (0.5 x / w + 0.5, 0.5 - 0.5 y / w)
    if (dwords < 60) {
        return;
    }
    auto& camera = velocity_camera;
    // The previous clip (c40..c55) is the game's own value, and the game does not always give last frame's
    // camera there: in U3's horse ride (and other stretches: whole 600-frame windows at 60-144 FPS, 0.3.6.58
    // too) its velocity pass carries this frame's clip rows as the previous ones, so the camera's motion read
    // from it is zero while the camera moves (the upscaler was told the picture stood still: flicker on the
    // sand around the rider, smeared history). The previous clip is therefore taken from last frame's own
    // pass: frame F's c24..c39 (its jittered clip rows, the ones it was drawn with) are frame F+1's previous
    // clip whenever the passes are of consecutive frames (by the flip argument); where the game's value is
    // last frame's camera the two are equal bit for bit. Without last frame's pass (the first frame after a
    // gap) the game's value is all there is. BB_GAME_PREVIOUS_CLIP=1 (diagnosis): the game's value always.
    const s64 pass_frame = DrawnFrame();
    if (pass_frame != own_clip_frame) {
        own_previous_clip = own_clip;
        own_previous_clip_frame = own_clip_frame;
        own_previous_camera = own_camera;
    }
    std::copy(c + 24, c + 40, own_clip.begin());
    own_camera = {c[15], c[19], c[23]};
    own_clip_frame = pass_frame;
    static const bool game_previous = [] {
        const char* env = std::getenv("BB_GAME_PREVIOUS_CLIP");
        const bool v = env && env[0] == '1';
        std::printf("Camera motion: previous clip from %s (BB_GAME_PREVIOUS_CLIP)\n",
                    v ? "the game's velocity pass" : "last frame's own velocity pass (the game's without one)");
        return v;
    }();
    const bool own_previous = !game_previous && pass_frame >= 1 && own_previous_clip_frame == pass_frame - 1;
    // c40..c55: last frame's own clip rows, or the game's
    const float* previous = own_previous ? own_previous_clip.data() : c + 40;
    {
        // The count: frames whose game previous clip was not last frame's clip rows (held: this frame's own).
        static u32 frames = 0, held = 0, other = 0, lines = 0;
        if (own_previous) {
            ++frames;
            const bool same_as_last = std::equal(c + 40, c + 56, own_previous_clip.begin());
            const bool same_as_now = std::equal(c + 40, c + 56, c + 24);
            held += !same_as_last && same_as_now;
            other += !same_as_last && !same_as_now;
            if (frames % 600 == 0) {
                if (lines < 20 && (held || other || lines < 3)) {
                    ++lines;
                    std::printf("Camera motion: the game's previous clip was this frame's own in %u of 600 frames, "
                                "another than last frame's in %u (last frame's own used)\n", held, other);
                }
                held = other = 0;
            }
        }
    }
    const float* rows[3] = {previous, previous + 4, previous + 12};
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 4; ++col) {
            float v = col == 3 ? rows[r][3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += rows[r][k] * c[12 + k * 4 + col];
            }
            camera.to_previous_clip[r * 4 + col] = v;
        }
    }
    // The velocity pass's stencil term (its pixel shader): pixels WITHOUT stencil bits 0xc0 add the
    // world offset c56..c58 before the previous clip; the marked ones (what moves with the camera's
    // platform: U3's rider and horse, U1's jeep, U2's convoy truck) do not. On a platform the game
    // gives this frame's own clip rows as the previous ones: its motion is then taken in the
    // platform's frame, the marked pixels standing still and the world moving by the offset. Here
    // the previous clip is last frame's real one (above), under which the world stands still: the
    // marked pixels moved with the platform, so they were at p - (the platform's step this frame).
    // Without that, the rider got the world's motion past him while he stays on screen: DLSS took
    // his history from the wrong place (flicker on Nate and a band around him on U3's horse ride,
    // from 12eacdd0 on). With the game's own previous clip (not held, or no last frame's pass) the
    // game's form exactly: the unmarked pixels add the offset. BB_STENCIL_MOTION=0 (diagnosis):
    // the term left out (the build before).
    // The platform's step is the camera's own step between the two passes (c15, c19, c23: the
    // view-to-world translation), not the game's offset: that offset points the same way (cosine
    // 0.999-1.000 on U3's horse, U1's jeep, U2's convoy) but is a motion-blur length, not a step:
    // at 120 FPS it alternates 1.66 / 2.49 times the step frame by frame, about the step in a long
    // frame, so scaling it by the frame time left Nate's vectors jumping a few pixels every other
    // frame and 30-60 px off around a long frame (the frame times run a frame apart from the
    // passes). The camera's step comes with the two clips that are used, so it is the step of
    // exactly this frame pair.
    {
        static const bool stencil_motion = [] {
            const char* env = std::getenv("BB_STENCIL_MOTION");
            const bool v = !(env && env[0] == '0');
            std::printf("Camera motion: the velocity pass's stencil (platform) motion %s "
                        "(BB_STENCIL_MOTION)\n", v ? "on" : "off");
            return v;
        }();
        const bool held = std::equal(c + 40, c + 56, c + 24);
        const bool carried = own_previous && held;
        const std::array<float, 3> step{own_camera[0] - own_previous_camera[0],
                                         own_camera[1] - own_previous_camera[1],
                                         own_camera[2] - own_previous_camera[2]};
        const float offset[3] = {carried ? -step[0] : c[56], carried ? -step[1] : c[57],
                                 carried ? -step[2] : c[58]};
        // the platform offset in use (the game's form) or a held clip (ours): the term applies
        const bool platform = c[56] != 0.0f || c[57] != 0.0f || c[58] != 0.0f;
        camera.stencil_select = 0;
        camera.stencil_clip_offset = {};
        if (stencil_motion && platform && std::isfinite(offset[0]) && std::isfinite(offset[1]) &&
            std::isfinite(offset[2])) {
            for (int r = 0; r < 3; ++r) {
                camera.stencil_clip_offset[r] =
                    rows[r][0] * offset[0] + rows[r][1] * offset[1] + rows[r][2] * offset[2];
            }
            camera.stencil_select = carried ? 0x100u : 0x200u;
        }
        // BB_STENCIL_MOTION_LOG=1: every 60 frames the offset against the camera's step; 2: every frame.
        static const char* log_env = std::getenv("BB_STENCIL_MOTION_LOG");
        if (static const bool log = log_env != nullptr; log) {
            const bool every = log_env[0] == '2';
            static u32 frames = 0, lines = 0;
            if ((++frames % 60 == 0 || every) && lines < 8000) {
                ++lines;
                std::printf("Stencil motion: frame %lld held %d own %d offset %g %g %g camera step %g %g %g "
                            "(marked pixels %s)\n", static_cast<long long>(pass_frame), int(held),
                            int(own_previous), c[56], c[57], c[58], step[0], step[1], step[2],
                            camera.stencil_select == 0x100u ? "carry the camera's step"
                            : camera.stencil_select       ? "still, the others add the offset"
                                                          : "-");
            }
        }
    }
    camera.uv_to_view = {c[0], c[1], c[2], c[3]};
    // Projection jitter. The pass's own view-from-uv constants stay symmetric (built from the field of view), but its
    // clip rows are the jittered projection times a rigid motion: clip x = sx (M view).x + jx (M view).z,
    // w = (M view).z, the rows of M orthogonal, so jx = (row x . row w) / |row w|^2, exactly. The pass carries this
    // frame's clip rows (c24..c39: x, y, z, w) and last frame's (c40..c55); the next frame's c40..c55 are this frame's
    // c24..c39. So the jitter this frame is drawn with is read from this frame's own pass and given to the upscaler:
    // whatever the game side did (0.3.6.57 inferred it from the values asked of the game a frame or more ahead, which
    // the game's job threads sometimes did not use in that order: on a Radeon in more than half the frames).
    projection_mode = ProjectionJitterActive(); // patched after the GPU starts
    if (projection_mode) {
        const auto jitter_of = [&](const float* x, const float* y, const float* w) {
            std::array<float, 4> rx{}, ry{}, rw{};
            const float* src[3] = {x, y, w};
            std::array<float, 4>* dst[3] = {&rx, &ry, &rw};
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) {
                    float v = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        v += src[r][k] * c[12 + k * 4 + col];
                    }
                    (*dst[r])[col] = v;
                }
            }
            const float ww = rw[0] * rw[0] + rw[1] * rw[1] + rw[2] * rw[2];
            std::array<float, 2> j{};
            if (ww > 0.0f) {
                j = {(rx[0] * rw[0] + rx[1] * rw[1] + rx[2] * rw[2]) / ww,
                     (ry[0] * rw[0] + ry[1] * rw[1] + ry[2] * rw[2]) / ww};
            }
            for (auto& v : j) {
                if (!std::isfinite(v) || std::abs(v) > 0.01f) {
                    v = 0.0f; // not a sub-pixel shift: a cut, another kind of camera
                }
            }
            return j;
        };
        const std::array<float, 2> last = jitter_of(previous, previous + 4, previous + 12);
        const std::array<float, 2> now = jitter_of(c + 24, c + 28, c + 36);
        // The check: the jitter the upscaler was given for the frame before (GiveProjectionJitter, the frame by the
        // flip argument) against what this frame's pass says that frame was drawn with: one line every 600 frames (20
        // lines), the first 20 differences each. Frames the upscaler did not run for are not counted.
        const s64 frame = DrawnFrame();
        {
            static u32 frames = 0, wrong = 0, lines = 0, shown = 0, without = 0;
            const bool moving = last[0] != 0.0f || last[1] != 0.0f || given_jitter[0] != 0.0f ||
                                given_jitter[1] != 0.0f;
            if (moving && frame >= 1 && given_frame == frame - 1) {
                ++frames;
                const bool differs = std::abs(last[0] - given_jitter[0]) > 2e-6f ||
                                     std::abs(last[1] - given_jitter[1]) > 2e-6f;
                wrong += differs;
                if (differs && shown < 20) {
                    ++shown;
                    std::printf("Projection jitter check: game frame %lld was drawn with %.1f %.1f, the upscaler was "
                                "given %.1f %.1f (NDC x 1e6)\n", static_cast<long long>(frame - 1), last[0] * 1e6f,
                                last[1] * 1e6f, given_jitter[0] * 1e6f, given_jitter[1] * 1e6f);
                }
                if (frames % 600 == 0 && lines < 20) {
                    ++lines;
                    std::printf("Projection jitter check: %u of 600 frames had another jitter than the upscaler was "
                                "given (each frame's own from its velocity pass, %u frames without one: the game "
                                "side's by the frame number%s)\n", wrong, given_without_pass - without,
                                runtime_projection_jitter_frame ? "" : ", not available");
                    wrong = 0;
                    without = given_without_pass;
                }
            }
        }
        projection_frame = frame;
        previous_projection_jitter = last;
        projection_jitter = now;
    }
    // A camera that did not move: the same view-to-world rows and the same view-from-uv constants as
    // in the last frame, up to the rounding of the game's own camera maths (a standing camera's
    // values flip by one unit in the last place every 50-200 frames; four are allowed for the rotation:
    // a turn of 5e-7 radians, under 0.001 pixels; the translation is held to 1e-5 world units). Its motion
    // vectors are zero by definition; computed, they
    // are rounding noise (the previous clip is world to clip times view to world, the world position
    // cancelling in single precision: up to about 0.01 pixels on near surfaces), on which the
    // upscalers do not hold a still picture. RecordVelocity then writes exact zeros for the camera
    // (characters keep their own vectors). BB_STILL_CAMERA=0: off (the computed vectors).
    {
        static const bool on = [] {
            const char* env = std::getenv("BB_STILL_CAMERA");
            const bool v = !(env && env[0] == '0');
            std::printf("Camera motion: exact zero motion for a still camera %s (BB_STILL_CAMERA)\n",
                        v ? "on" : "off");
            return v;
        }();
        std::array<float, 16> now{};
        std::copy(c + 12, c + 24, now.begin());
        std::copy(c, c + 4, now.begin() + 12);
        bool same = has_still_reference;
        for (u32 i = 0; i < now.size() && same; ++i) {
            // The camera's world translation (entries 3, 7, 11) is compared in world units, not relative to its
            // size: a camera far from the origin has coarse floats, and a relative limit there allowed a
            // millimetre a frame (about a pixel on a near surface) to count as still. 1e-5 world units is
            // about 0.01 pixels at a surface a metre away at 1440p; the rotation and the view-from-uv
            // constants keep the relative limit (their rounding does not grow with the position).
            const bool translation = i < 12 && i % 4 == 3;
            if (translation) {
                same = std::abs(now[i] - still_reference[i]) <= 1e-5f;
                continue;
            }
            const float scale = std::max({std::abs(now[i]), std::abs(still_reference[i]), 1.0f});
            same = std::abs(now[i] - still_reference[i]) <= 4.0f * 1.1920929e-7f * scale;
        }
        camera.still = on && same;
        // Diagnosis (BB_STILL_CAMERA_LOG=1): a frame that is not still after still ones: what changed.
        static const bool still_log = std::getenv("BB_STILL_CAMERA_LOG") != nullptr;
        static u32 run = 0, lines = 0;
        if (camera.still) {
            ++run;
        } else {
            if (still_log && on && has_still_reference && run >= 20 && lines < 40) {
                ++lines;
                float worst = 0.0f;
                int index = -1, changed = 0;
                for (int i = 0; i < 16; ++i) {
                    const float d = std::abs(now[i] - still_reference[i]);
                    changed += d != 0.0f;
                    if (d > worst) {
                        worst = d;
                        index = i;
                    }
                }
                std::printf("Camera motion: not still after %u still frames: %d of 16 values changed, the "
                            "largest by %g (value %d: %g -> %g)\n", run, changed, worst, index,
                            index >= 0 ? still_reference[index] : 0.0f, index >= 0 ? now[index] : 0.0f);
            }
            run = 0;
        }
        still_reference = now;
        has_still_reference = true;
        static u32 frames = 0, still_frames = 0, still_lines = 0;
        still_frames += camera.still;
        if (++frames % 600 == 0 && still_lines < 20) {
            ++still_lines;
            std::printf("Camera motion: still camera in %u of 600 frames\n", still_frames);
            still_frames = 0;
        }
    }
    std::copy(c + 12, c + 24, camera.to_world.begin());
    camera.depth_to_z = {c[8], c[9]};
    // Kept for the upscaler's near / far planes (Near, Far): the frame's camera is cleared at the display
    // pass, before the upscaler runs on the path after the post-processing.
    if (c[9] != 0.0f && std::isfinite(c[8]) && std::isfinite(c[9])) {
        planes = {c[8], c[9]};
        planes_known = true;
    }
    camera.valid = c[9] != 0.0f &&
                   std::all_of(camera.to_previous_clip.begin(), camera.to_previous_clip.end(),
                               [](float v) { return std::isfinite(v); });
    // bbport: camera cuts (a melee move's own camera, cutscene shots). The velocity pass's
    // previous camera is then the other shot: motion vectors far off screen, and the upscaler
    // reprojecting its history along them smeared the frame into vertical streaks (U3's pub).
    // A turn of more than ~35 degrees or a jump of more than 2 units in one frame is a cut:
    // no motion that frame, and the history is reset (ConsumeCut).
    {
        const std::array<float, 3> forward{c[14], c[18], c[22]}, position{c[15], c[19], c[23]};
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] +
                                       forward[2] * forward[2]);
        camera.cut = false;
        if (std::isfinite(length) && length > 0.0f) {
            if (has_last_camera) {
                const float dot = (forward[0] * last_forward[0] + forward[1] * last_forward[1] +
                                   forward[2] * last_forward[2]) / length;
                const float dx = position[0] - last_position[0], dy = position[1] - last_position[1],
                            dz = position[2] - last_position[2];
                const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
                // bbport: a cut also when the picture moves more than half the render width in
                // one frame (a swing that turns less than 35 degrees or moves less than 2 units,
                // e.g. a narrow-FOV cutscene shot change): the screen points at view z 5 near the
                // centre and both sides, reprojected into the previous frame.
                // BB_CUT_MOTION=<fraction of the width> (default 0.5; 0: off).
                static const float cut_motion = [] {
                    const char* env = std::getenv("BB_CUT_MOTION");
                    const float v = env && env[0] ? static_cast<float>(std::atof(env)) : 0.5f;
                    std::printf("Camera motion: cut when the picture moves %.2f of the width "
                                "(BB_CUT_MOTION)\n", v);
                    return v;
                }();
                float swing = 0.0f;
                if (camera.valid) {
                    for (const float u : {0.5f, 0.15f, 0.85f}) {
                        constexpr float z = 5.0f;
                        const float vp[4] = {z * (camera.uv_to_view[0] * u + camera.uv_to_view[2]),
                                             z * (camera.uv_to_view[1] * 0.5f + camera.uv_to_view[3]),
                                             z, 1.0f};
                        float clip[3];
                        for (int r = 0; r < 3; ++r) {
                            clip[r] = 0.0f;
                            for (int k = 0; k < 4; ++k) {
                                clip[r] += camera.to_previous_clip[r * 4 + k] * vp[k];
                            }
                        }
                        if (clip[2] <= 1e-4f) {
                            swing = 1.0f; // behind the previous camera: a cut
                            break;
                        }
                        const float du = 0.5f * clip[0] / clip[2] + 0.5f - u;
                        const float dv = 0.5f - 0.5f * clip[1] / clip[2] - 0.5f;
                        const float m = std::sqrt(du * du + dv * dv);
                        swing = std::max(swing, std::isfinite(m) ? m : 0.0f);
                    }
                }
                // The picture's motion this frame (fraction of the width; the jitter fade).
                picture_speed.store(swing, std::memory_order_relaxed);
                const bool swung = cut_motion > 0.0f && swing > cut_motion;
                if (dot < 0.82f || moved > 2.0f || swung) {
                    if (swung && dot >= 0.82f && moved <= 2.0f) {
                        static int swings = 0;
                        if (swings++ < 20) {
                            std::printf("Camera motion: picture moved %.2f of the width in a frame: "
                                        "cut\n", swing);
                        }
                    }
                    camera.cut = true;
                    camera_cut.store(true, std::memory_order_relaxed);
                    static int noted = 0;
                    if (noted++ < 20) {
                        std::printf("Camera motion: camera cut (turned %.0f degrees, moved %.2f): "
                                    "upscaler history reset\n",
                                    std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.29578f, moved);
                    }
                }
            }
            // bbport diagnosis (BB_VELOCITY_LOG=1): for 600 frames after a cut (and at the start),
            // the camera's real change since the last frame against the motion the velocity pass's
            // reprojection predicts at the screen centre (view z 5 and 100, in 1/1000 of the width).
            static const bool velocity_log = std::getenv("BB_VELOCITY_LOG") != nullptr;
            static int log_frames = 600;
            if (camera.cut) {
                log_frames = 600;
            }
            if (velocity_log && log_frames > 0 && has_last_camera) {
                --log_frames;
                const float dot = std::clamp((forward[0] * last_forward[0] + forward[1] * last_forward[1] +
                                              forward[2] * last_forward[2]) / length, -1.0f, 1.0f);
                const float dx = position[0] - last_position[0], dy = position[1] - last_position[1],
                            dz = position[2] - last_position[2];
                const auto predict = [&](float z, float u, float v, float* mu, float* mv) {
                    const float vp[4] = {z * (camera.uv_to_view[0] * u + camera.uv_to_view[2]),
                                         z * (camera.uv_to_view[1] * v + camera.uv_to_view[3]), z, 1.0f};
                    float clip[3];
                    for (int r = 0; r < 3; ++r) {
                        clip[r] = 0.0f;
                        for (int k = 0; k < 4; ++k) {
                            clip[r] += camera.to_previous_clip[r * 4 + k] * vp[k];
                        }
                    }
                    *mu = (0.5f * clip[0] / clip[2] + 0.5f - u) * 1000.0f;
                    *mv = (0.5f - 0.5f * clip[1] / clip[2] - v) * 1000.0f;
                };
                float u5, v5, u100, v100, ue, ve;
                predict(5.0f, 0.5f, 0.5f, &u5, &v5);
                predict(100.0f, 0.5f, 0.5f, &u100, &v100);
                predict(5.0f, 0.9f, 0.5f, &ue, &ve);
                std::printf("Velocity: t=%.4f turned %.4f deg moved %.4f | predicted centre z5 %.3f,%.3f "
                            "z100 %.3f,%.3f edge z5 %.3f,%.3f%s\n",
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now().time_since_epoch()).count(),
                            std::acos(dot) * 57.29578f, std::sqrt(dx * dx + dy * dy + dz * dz), u5, v5,
                            u100, v100, ue, ve, camera.cut ? " CUT" : "");
            }
            last_forward = {forward[0] / length, forward[1] / length, forward[2] / length};
            last_position = position;
            has_last_camera = true;
        }
        RenderDocOnCut(camera.cut);
    }
    // Projection jitter (prototype) diagnosis, a line every 240 frames (10 lines): the jitter read
    // back, the one now asked of the game, last frame's, and the camera motion of the screen centre
    // at view z 100 (all in 1/1000000 of the picture: with the camera still, a centre motion equal
    // to half the jitter difference shows the previous clip carries the previous jitter).
    if (projection_mode && camera.valid) {
        static u32 frames = 0, lines = 0;
        if (++frames % 240 == 0 && lines < 10) {
            ++lines;
            const float z = 100.0f, u = 0.5f, v = 0.5f;
            const float vp[4] = {z * (camera.uv_to_view[0] * u + camera.uv_to_view[2]),
                                 z * (camera.uv_to_view[1] * v + camera.uv_to_view[3]), z, 1.0f};
            float clip[3];
            for (int r = 0; r < 3; ++r) {
                clip[r] = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    clip[r] += camera.to_previous_clip[r * 4 + k] * vp[k];
                }
            }
            const float du = 0.5f * clip[0] / clip[2] + 0.5f - u, dv = 0.5f - 0.5f * clip[1] / clip[2] - v;
            std::printf("Projection jitter: this frame %.1f %.1f, last frame %.1f %.1f (NDC x 1e6, read from the "
                        "velocity pass); centre motion %.1f %.1f, half last jitter %.1f %.1f (uv x 1e6)\n",
                        projection_jitter[0] * 1e6f, projection_jitter[1] * 1e6f,
                        previous_projection_jitter[0] * 1e6f, previous_projection_jitter[1] * 1e6f, du * 1e6f,
                        dv * 1e6f, previous_projection_jitter[0] * 0.5e6f, -previous_projection_jitter[1] * 0.5e6f);
        }
    }
    // The shown camera's heading for the mouse controls (bbgpu_camera_yaw: which game camera is on
    // screen), and with BB_CAMERA_LOG=1 its direction every frame, for measuring how the game turns
    // the camera from the right stick.
    const float fx = c[14], fy = c[18], fz = c[22];
    const double t = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
    if (std::isfinite(fx) && std::isfinite(fz) && (fx != 0.0f || fz != 0.0f)) {
        shown_camera_yaw.store(std::atan2(fx, fz) * 57.29578f, std::memory_order_relaxed);
        shown_camera_time.store(t, std::memory_order_release);
    }
    static const bool camera_log = std::getenv("BB_CAMERA_LOG") != nullptr;
    if (camera_log) {
        // pos: the camera's position (the velocity pass's view, c[15], c[19], c[23]); frame: the game frame
        // the GPU thread is at (submitted), for joining with the frame clock log.
        std::printf("Camera: t=%.4f yaw=%.3f pitch=%.3f pos=%.4f %.4f %.4f frame=%lld\n", t,
                    std::atan2(fx, fz) * 57.29578f,
                    std::asin(std::clamp(fy / std::sqrt(fx * fx + fy * fy + fz * fz), -1.0f, 1.0f)) * 57.29578f,
                    c[15], c[19], c[23],
                    static_cast<long long>(Libraries::VideoOut::g_gpu_flip_arg.load(std::memory_order_relaxed)));
    }
    // The stencil term on a platform: said once.
    static bool reported = false;
    if (!reported && camera.stencil_select == 0x100u) {
        reported = true;
        std::printf("Camera motion: on a platform (offset %g, %g, %g; the game's previous clip is this "
                    "frame's): the stencil-marked pixels move with the camera's step\n", c[56], c[57], c[58]);
    }
    // bbport (DLSS rework 11): with BB_STENCIL_MOTION_LOG=1, every 2 s: frames whose velocity pass
    // moved stencil-marked pixels, and the largest such offset (vehicles, platforms? train,
    // convoy, horse scenes).
    if (static const bool stencil_log = std::getenv("BB_STENCIL_MOTION_LOG") != nullptr;
        stencil_log) {
        static u32 frames = 0, moved = 0;
        static float largest = 0.0f;
        static auto window = std::chrono::steady_clock::now();
        ++frames;
        const float m = std::sqrt(c[56] * c[56] + c[57] * c[57] + c[58] * c[58]);
        if (m > 0.0f) {
            ++moved;
            largest = std::max(largest, m);
        }
        if (const auto now = std::chrono::steady_clock::now(); now - window >= std::chrono::seconds(2)) {
            std::printf("Stencil motion: %u of %u frames moved marked pixels, largest %g (last %g %g %g)\n",
                        moved, frames, largest, c[56], c[57], c[58]);
            frames = moved = 0;
            largest = 0.0f;
            window = now;
        }
    }
}

/// bbport: BB_MOTION_DEADZONE=<pixels> (default 0.01; 0: off): velocity-pass motion below it is
/// none (float rounding left +-0.0002 px alternating per column: DLSS drew stripes from it).
/// With the jitter in the game's projection it is off unless set (the user's tests, 2026-11-15): a
/// still camera gets exact zeros there without a threshold (OnVelocityConstants, camera.still).
/// Without either a still view was less steady with every upscaler (U3 street, 3440x1440 native,
/// 60-frame temporal std p50: DLSS 0.35 against 0.22 with the dead zone, FSR 3.1 0.30-0.40 against
/// 0.22; the viewport-jitter path without it: DLSS 0.25 against 0.15, FSR 3.1 0.40 against 0.23).
/// The jitter is in the game's projection for the upscaler in use: the game code is patched
/// (runtime_ultrawide.c). BB_PROJECTION_JITTER_FSR3=0 (diagnosis): FSR 3.1 alone keeps the viewport /
/// vertex-shader jitter with its fixes (fade, dead zone, small draws). With the projection jitter FSR
/// 3.1's still picture measures about 10% less steady than with the old jitter (U3 street, 3440x1440
/// native, 60-frame std p50 0.21-0.23 against 0.20-0.21), although it is told the frame's own jitter and
/// gets zero motion: cause open.
bool CameraMotion::ProjectionJitterActive() {
    if (!runtime_projection_jitter_slot) {
        return false;
    }
    static const bool fsr3_old = [] {
        const char* env = std::getenv("BB_PROJECTION_JITTER_FSR3");
        return env && env[0] == '0';
    }();
    return !fsr3_old || BbSettings::Get().upscaler != BbSettings::UpscalerFsr3;
}

float CameraMotion::MotionDeadzone() {
    static const float set = [] {
        const char* env = std::getenv("BB_MOTION_DEADZONE");
        return env && env[0] ? static_cast<float>(std::atof(env)) : -1.0f;
    }();
    return set >= 0.0f ? set : ProjectionJitterActive() ? 0.0f : 0.01f;
}
static float motion_deadzone() {
    return CameraMotion::MotionDeadzone();
}


static bool zero_motion_test() {
    static const bool on = [] {
        const char* env = std::getenv("BB_DLSS_ZERO_MOTION");
        return env && env[0] == '1';
    }();
    return on;
}

vk::ImageView CameraMotion::ZeroStencil(vk::CommandBuffer cmdbuf) {
    if (zero_stencil_view) {
        return *zero_stencil_view;
    }
    const auto device = instance.GetDevice();
    zero_stencil = VideoCore::UniqueImage(device, instance.GetAllocator());
    zero_stencil.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Uint,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    zero_stencil_view = Check(device.createImageViewUnique({
        .image = vk::Image(zero_stencil),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Uint,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(zero_stencil),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    vk::ClearColorValue zero{};
    zero.uint32 = std::array<u32, 4>{0, 0, 0, 0};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmdbuf.clearColorImage(vk::Image(zero_stencil), vk::ImageLayout::eGeneral, zero, range);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eGeneral;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    return *zero_stencil_view;
}

void CameraMotion::RecordVelocity(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                  vk::ImageView motion_view, u32 width, u32 height,
                                  float jitter_scale, vk::ImageView stencil_view) {
    const auto& camera = velocity_camera;
    // Mode 4 (BilalReloaded's #84): the characters' own motion (ObjectMotion,
    // Shader::MotionVectors::characters) where it was written this frame; the camera's elsewhere.
    // Not for motion written at another size than the render (frame generation's larger capture).
    const bool still = zero_motion_test() || !camera.valid || camera.cut;
    bool objects = false;
    vk::ImageView object_view = depth_view;
    if (!still && jitter_scale == 1.0f && Shader::MotionVectors::characters && object_motion &&
        object_motion->Enabled()) {
        object_view = object_motion->PrepareRead(cmdbuf, width, height, objects);
        if (!objects) {
            object_view = depth_view;
        }
    }
    // Projection jitter (prototype): prev_proj.w = 1 tells the shader that depth and both clip
    // positions carry the jitter (pixels here: x right, y down), to be taken out of the motion.
    // BB_PROJECTION_JITTER_PREV=0 (diagnosis): the previous clip is taken as unjittered relative
    // to the current one (no correction).
    static const bool previous_jittered = [] {
        const char* env = std::getenv("BB_PROJECTION_JITTER_PREV");
        return !(env && env[0] == '0');
    }();
    const auto pixels = [&](const std::array<float, 2>& ndc) {
        return std::array<float, 2>{ndc[0] * 0.5f * float(width), -ndc[1] * 0.5f * float(height)};
    };
    const auto now_jitter = projection_mode ? pixels(projection_jitter)
        : std::array<float, 2>{jitter[0] * jitter_scale, jitter[1] * jitter_scale};
    const auto before_jitter = projection_mode
        ? pixels(previous_jittered ? previous_projection_jitter : projection_jitter)
        : std::array<float, 2>{previous_jitter[0] * jitter_scale, previous_jitter[1] * jitter_scale};
    const PushConstants push{
        .reproject = camera.to_previous_clip,
        .proj = camera.uv_to_view,
        // w: 1 = projection jitter, + 2 = a still camera (exact zero camera motion).
        .prev_proj = {camera.depth_to_z[0], camera.depth_to_z[1], motion_deadzone(),
                      (projection_mode ? 1.0f : 0.0f) + (camera.still ? 2.0f : 0.0f)},
        .size = {float(width), float(height)},
        .jitter = now_jitter,
        .previous_jitter = before_jitter,
        // BB_DLSS_ZERO_MOTION=1 (diagnosis): no camera motion vectors at all.
        // Bit 10 (BB_MOTION_GATE_CHECK=1, diagnosis): the object vectors' difference to the camera's is written.
        .mode = still ? 3u
                      : (objects ? 4u : 2u) | (stencil_view ? camera.stencil_select : 0u) |
                            (objects && object_motion->GateCheck() ? 0x400u : 0u),
        .pad = 0,
        .stencil_offset = {camera.stencil_clip_offset[0], camera.stencil_clip_offset[1],
                           camera.stencil_clip_offset[2], 1.0f / jitter_scale},
    };
    const vk::ImageView stencil = stencil_view ? stencil_view : ZeroStencil(cmdbuf);
    if (static const char* log = std::getenv("BB_STENCIL_MOTION_LOG"); log && log[0] == '2') {
        std::printf("Stencil record: mode %#x offset %g %g %g scale %g stencil %d\n", push.mode,
                    push.stencil_offset[0], push.stencil_offset[1], push.stencil_offset[2],
                    push.stencil_offset[3], int(bool(stencil_view)));
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    // Binding 2: the object motion in mode 4 (not read in the others); 3: the stencil.
    const vk::DescriptorImageInfo velocity_info{.imageView = object_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo stencil_info{.imageView = stencil,
                                               .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &velocity_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &stencil_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void CameraMotion::RecordVelocityDeferred(vk::Image motion_image, vk::ImageView depth_view,
                                          vk::ImageView motion_view, u32 width, u32 height,
                                          vk::ImageView stencil_view) {
    const auto& camera = velocity_camera;
    // The zero image is made here, on this thread, before the recording thread runs.
    const vk::ImageView stencil = stencil_view ? stencil_view
                                  : zero_stencil_view ? *zero_stencil_view
                                                      : ZeroStencil(scheduler.CommandBuffer());
    const PushConstants push{
        .reproject = camera.to_previous_clip,
        .proj = camera.uv_to_view,
        .prev_proj = {camera.depth_to_z[0], camera.depth_to_z[1], motion_deadzone(), 0.0f},
        .size = {float(width), float(height)},
        .jitter = {jitter[0], jitter[1]},
        .previous_jitter = {previous_jitter[0], previous_jitter[1]},
        .mode = (zero_motion_test() || !camera.valid || camera.cut)
                    ? 3u : 2u | (stencil_view ? camera.stencil_select : 0u),
        .pad = 0,
        .stencil_offset = {camera.stencil_clip_offset[0], camera.stencil_clip_offset[1],
                           camera.stencil_clip_offset[2], 1.0f},
    };
    const vk::Pipeline pipeline = *motion_pipeline;
    const vk::PipelineLayout layout = *motion_pipeline_layout;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = motion_image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo stencil_info{.imageView = stencil,
                                                   .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 4> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &depth_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &depth_info},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &stencil_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
        b.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        b.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
        b.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        b.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
        b.oldLayout = vk::ImageLayout::eGeneral;
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    });
}

void CameraMotion::OnConstants(const float* data) {
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth) {
    depth_id = depth;
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    if (debug_overlay && frame && depth_id && current.valid && previous.valid) {
        Overlay(frame);
    }
    if (!frame_has_camera) InvalidateHistory();
    frame_has_camera = false;
    depth_id = {};
    velocity_camera.valid = false;
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        color.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height);
        }
        return;
    }
    const auto device = instance.GetDevice();
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();

    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(color.info.size.width), float(color.info.size.height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = BbToggle::Disabled(1u << 20)   ? 1u
                : BbToggle::Disabled(1u << 21) ? 2u
                : BbToggle::Disabled(1u << 22) ? 3u
                : BbToggle::Disabled(1u << 23) ? 4u
                                               : 0u,
    };
    static u32 log_counter = 0;
    if (++log_counter % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], current.view[0],
                    current.view[1], current.view[2], current.view[3], current.view[4],
                    current.view[5], current.view[6], current.view[7], current.view[8],
                    current.view[9], current.view[10], current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto& prev_frame = *frames[frame_index ^ 1];
    const auto& next_frame = *frames[frame_index];
    frame_index ^= 1;
    const vk::DescriptorBufferInfo prev_info{prev_frame.Handle(), 0, prev_frame.SizeBytes()};
    const vk::DescriptorBufferInfo next_info{next_frame.Handle(), 0, next_frame.SizeBytes()};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &prev_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &next_info},
    }};
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *overlay_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    // The previous frame buffer was written by the last dispatch.
    const vk::MemoryBarrier2 frame_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &frame_barrier});
    cmdbuf.dispatch((color.info.size.width + 7) / 8, (color.info.size.height + 7) / 8, 1);

    scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan

namespace Vulkan {

namespace {
using Mat4 = std::array<std::array<double, 4>, 4>;
Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r[i][j] += a[i][k] * b[k][j];
    return r;
}
/// Gauss-Jordan with partial pivoting; false when singular.
bool Invert(const Mat4& m, Mat4& out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
        if (std::abs(a[pivot][col]) < 1e-12) return false;
        if (pivot != col)
            for (int j = 0; j < 8; ++j) std::swap(a[col][j], a[pivot][j]);
        const double d = a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = a[i][j + 4];
    return true;
}
/// Column-vector matrix -> NGX's row-vector layout (the transpose).
void Store(float (&dst)[4][4], const Mat4& m) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) dst[i][j] = float(m[j][i]);
}
} // namespace

bool CameraMotion::RealCamera(Dlss::FrameGenCamera& out) const {
    const auto& camera = velocity_camera;
    out.valid = false;
    if (!velocity_mode || !camera.valid || camera.cut) {
        return false;
    }
    // The velocity pass: view x = z (a u + c), view y = z (b v + d), z = y0 / (depth - x0), u, v
    // the D3D-style screen uv (v down). As a projection with w = view z:
    //   ndc x = 2u - 1, ndc y = 1 - 2v, depth = x0 + y0 / z.
    const double a = camera.uv_to_view[0], b = camera.uv_to_view[1], c = camera.uv_to_view[2],
                 d = camera.uv_to_view[3], x0 = camera.depth_to_z[0], y0 = camera.depth_to_z[1];
    if (a == 0.0 || b == 0.0) {
        return false;
    }
    const Mat4 proj{{{2.0 / a, 0.0, -2.0 * c / a - 1.0, 0.0},
                     {0.0, -2.0 / b, 1.0 + 2.0 * d / b, 0.0},
                     {0.0, 0.0, x0, y0},
                     {0.0, 0.0, 1.0, 0.0}}};
    // Previous clip: the pass's rows x, y, w of (view, 1); z with the same depth mapping.
    const auto& p = camera.to_previous_clip;
    const Mat4 previous{{{p[0], p[1], p[2], p[3]},
                         {p[4], p[5], p[6], p[7]},
                         {x0 * p[8], x0 * p[9], x0 * p[10], x0 * p[11] + y0},
                         {p[8], p[9], p[10], p[11]}}};
    Mat4 inverse_proj{}, to_previous{}, from_previous{};
    if (!Invert(proj, inverse_proj)) {
        return false;
    }
    to_previous = Multiply(previous, inverse_proj);
    if (!Invert(to_previous, from_previous)) {
        return false;
    }
    Store(out.view_to_clip, proj);
    Store(out.clip_to_view, inverse_proj);
    Store(out.clip_to_prev_clip, to_previous);
    Store(out.prev_clip_to_clip, from_previous);
    const auto& m = camera.to_world;
    const auto unit = [](float (&v)[3], float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        const float s = l > 0.0f ? 1.0f / l : 0.0f;
        v[0] = x * s;
        v[1] = y * s;
        v[2] = z * s;
    };
    unit(out.right, m[0], m[4], m[8]);
    unit(out.up, m[1], m[5], m[9]);
    unit(out.forward, m[2], m[6], m[10]);
    out.position[0] = m[3];
    out.position[1] = m[7];
    out.position[2] = m[11];
    out.aspect = float(std::abs(a / b));
    out.valid = true;
    return true;
}

} // namespace Vulkan
