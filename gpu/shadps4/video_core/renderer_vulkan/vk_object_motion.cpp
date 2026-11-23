// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_object_motion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <mutex>
#include <string>
#include <unordered_map>

#include <vk_mem_alloc.h>
#include <xxhash.h>
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

bool CreateBuffer(const Instance& instance, vk::DeviceSize size, bool host, vk::Buffer& buffer,
                  VmaAllocation& allocation, void** mapped, u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    if (mapped) {
        *mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address != 0;
}

// The still data of the vertex shader programs, by program hash. Entries are never removed.
std::mutex still_mutex;
std::unordered_map<u64, std::unique_ptr<Motion::StillProgram>> still_programs;

std::unique_ptr<Motion::StillProgram> AnalyzeStill(std::span<const u32> spv) {
    auto program = std::make_unique<Motion::StillProgram>();
    program->code = Motion::PositionCode::Parse(spv, "srt_flatbuf", Shader::PushData::UdRegsIndex,
                                                Shader::NUM_USER_DATA_REGS / 4);
    if (!program->code->Valid()) {
        program->why = program->code->Why();
        program->state.store(Motion::StillProgram::Unusable, std::memory_order_release);
    }
    return program;
}

} // namespace

void NoteVertexShaderCode(u64 pgm_hash, std::span<const u32> spv) {
    auto program = AnalyzeStill(spv);
    std::scoped_lock lk{still_mutex};
    still_programs.try_emplace(pgm_hash, std::move(program));
}

Motion::StillProgram& StillProgramOf(u64 pgm_hash) {
    {
        std::scoped_lock lk{still_mutex};
        if (const auto it = still_programs.find(pgm_hash); it != still_programs.end()) {
            return *it->second;
        }
    }
    // Not translated in this session: its stored binary (read outside the lock; another thread may do the same,
    // the first result stays).
    std::vector<u32> spv;
    std::unique_ptr<Motion::StillProgram> program;
    if (LoadStoredShaderBinary(pgm_hash, spv)) {
        program = AnalyzeStill(spv);
    } else {
        program = std::make_unique<Motion::StillProgram>();
        program->why = "no stored code";
        program->state.store(Motion::StillProgram::Unusable, std::memory_order_release);
    }
    std::scoped_lock lk{still_mutex};
    return *still_programs.try_emplace(pgm_hash, std::move(program)).first->second;
}

StillKeys MakeStillKeys(Motion::StillProgram& program, std::span<const u32> flat, u64 seed) {
    if (flat.size() < program.view_at + 16u || flat.size() < program.proj_at + 16u) {
        return {};
    }
    const auto* variant = program.VariantFor(flat);
    if (!variant) {
        return {};
    }
    u64 h = seed ^ (variant->config * 0x9e3779b97f4a7c15ULL);
    for (const auto& run : variant->key_runs) {
        if (size_t(run.first) + run.count > flat.size()) {
            return {};
        }
        h = XXH3_64bits_withSeed(flat.data() + run.first, size_t(run.count) * 4, h);
    }
    const u64 view = XXH3_64bits(flat.data() + program.view_at, 64);
    const u64 camera = XXH3_64bits_withSeed(flat.data() + program.proj_at, 64, view);
    return {h ? h : 1, camera ? camera : 1};
}

bool ObjectMotion::Still(const Motion::StillProgram& program, std::span<const u32> flat, u64 still_key,
                         u64 camera_key) {
    ++gate_stats.draws;
    if (!frame_camera_key) {
        // The frame's camera: the one its first draw with a key was drawn with (MakeStillKeys checked the size).
        frame_camera_key = camera_key;
        std::memcpy(frame_camera.data(), flat.data() + program.view_at, 64);
        std::memcpy(frame_camera.data() + 16, flat.data() + program.proj_at, 64);
        frame_camera_checked = false;
    }
    if (camera_key != frame_camera_key) {
        // Another camera than the frame's: the camera pass does not know it. Not noted either: the draw is not
        // still in the next frame, whatever camera it has then.
        ++gate_stats.other_camera;
        return false;
    }
    // Noted every frame, also while the gate may not act on it.
    const bool seen = still.Seen(still_key);
    if (camera_confirmed_frame + 1 != frame) {
        ++gate_stats.frame_unconfirmed; // the frame before had no velocity pass, or another camera than its draws
        return false;
    }
    if (!seen) {
        ++gate_stats.moved;
        return false;
    }
    ++gate_stats.still;
    return true;
}

void ObjectMotion::LearnCameraFrom(u64 pgm_hash, Motion::StillProgram& program, std::span<const u32> flat) {
    if (lessons.size() >= 4) {
        return;
    }
    for (const auto& lesson : lessons) {
        if (lesson.program == &program) {
            return;
        }
    }
    lessons.push_back({pgm_hash, &program, {flat.begin(), flat.end()}});
}

void ObjectMotion::OnVelocityClip(const float* clip) {
    if (!gate_enabled) {
        return;
    }
    if (frame_camera_key && !frame_camera_checked) {
        frame_camera_checked = true;
        float error = 0.0f;
        if (Motion::ProductIs(frame_camera.data(), frame_camera.data() + 16, clip, Motion::CameraTolerance,
                              &error)) {
            camera_confirmed_frame = frame;
            ++gate_stats.confirmed;
            gate_stats.worst_error = std::max(gate_stats.worst_error, error);
        } else {
            static u32 lines = 0;
            if (++gate_stats.refuted && lines < 8) {
                ++lines;
                std::printf("Object motion gate: the frame's draws have another camera than its velocity pass "
                            "(error %.3g of a row): no still draws in the next frame\n", error);
            }
        }
    }
    for (auto& lesson : lessons) {
        auto& program = *lesson.program;
        if (program.state.load(std::memory_order_relaxed) != Motion::StillProgram::NoCamera) {
            continue;
        }
        static u32 lines = 0;
        float error = 0.0f;
        size_t words = 0;
        if (program.LearnCamera(lesson.flat, clip, &error, &words)) {
            ++gate_stats.learned;
            if (lines < 16) {
                ++lines;
                std::string runs;
                if (const auto* variant = program.VariantFor(lesson.flat)) {
                    for (const auto& run : variant->key_runs) {
                        runs += " " + std::to_string(run.first) + "-" + std::to_string(run.first + run.count - 1);
                    }
                }
                std::printf("Object motion gate: vs %08x: this draw's position reads %zu of %zu flat words; view "
                            "matrix at %u, projection at %u (error %.2g of a row); still key words:%s\n",
                            u32(lesson.pgm_hash), words, lesson.flat.size(), u32(program.view_at),
                            u32(program.proj_at), error, runs.c_str());
            }
        } else if (++program.tries >= 8) {
            program.state.store(Motion::StillProgram::Unusable, std::memory_order_release);
            ++gate_stats.unusable;
            if (lines < 16) {
                ++lines;
                std::printf("Object motion gate: vs %08x: no camera found in its flat data (%zu position words, "
                            "closest pair off by %.3g of a row): its draws are never taken for still\n",
                            u32(lesson.pgm_hash), words, error);
            }
        }
    }
    lessons.clear();
}

ObjectMotion::ObjectMotion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    // Pipeline selection limits the extra attachment and vertex stores to likely
    // animated draws. A setting or environment override can still disable the path.
    if (!BbSettings::Get().object_motion ||
        BbSettings::Get().upscaler == BbSettings::UpscalerOff) {
        std::puts("Object motion: off (enable in menu or BB_OBJECT_MOTION=1)");
        return;
    }
    const auto& features = instance.GetPhysicalDevice().getFeatures();
    if (!features.vertexPipelineStoresAndAtomics) {
        std::printf("Object motion: vertexPipelineStoresAndAtomics unsupported, off\n");
        return;
    }
    // The motion pipelines push the buffers' addresses after the usual push constants (Shader::MotionPushData).
    const u32 push_limit = instance.GetPhysicalDevice().getProperties().limits.maxPushConstantsSize;
    if (push_limit < sizeof(Shader::PushData) + sizeof(Shader::MotionPushData)) {
        std::printf("Object motion: push constants limited to %u bytes, off\n", push_limit);
        return;
    }
    void* mapped = nullptr;
    u64 params_address = 0, positions_address = 0;
    if (!CreateBuffer(instance, vk::DeviceSize(1 + FrameSlots * ParamsPerFrame) * 32, true,
                      params_buffer, params_allocation, &mapped, params_address) ||
        !CreateBuffer(instance, vk::DeviceSize(1 + 2 * PositionsPerFrame) * 16, false,
                      positions_buffer, positions_allocation, nullptr, positions_address)) {
        std::printf("Object motion: buffer creation failed, off\n");
        return;
    }
    params_mapped = static_cast<u32*>(mapped);
    std::memset(params_mapped, 0, 32);
    deferred.reserve(ParamsPerFrame); // at most a frame's parameters wait
    Shader::MotionVectors::params_address = params_address;
    Shader::MotionVectors::positions_address = positions_address;
    enabled = true;
    {
        const char* check = std::getenv("BB_MOTION_GATE_CHECK");
        gate_enabled = StillGateOn();
        gate_check = gate_enabled && check && check[0] == '1';
    }
    if (const char* check = std::getenv("BB_MOTION_HISTORY_CHECK"); check && check[0] == '1') {
        history_check = std::make_unique<Motion::KeyedHistory>(PositionsPerFrame);
    }
    // (The motion shaders embed these addresses: a twin the driver keeps from an earlier start is found again only
    // when they are the same.)
    std::printf("Object motion: on (%u vertices per frame; buffers at %llx, %llx)\n", PositionsPerFrame,
                (unsigned long long)params_address, (unsigned long long)positions_address);
}

ObjectMotion::~ObjectMotion() {
    scheduler.Finish();
    const auto allocator = instance.GetAllocator();
    if (params_buffer) {
        vmaDestroyBuffer(allocator, params_buffer, params_allocation);
    }
    if (positions_buffer) {
        vmaDestroyBuffer(allocator, positions_buffer, positions_allocation);
    }
}

void ObjectMotion::OnFrameStart() {
    if (!enabled) {
        return;
    }
    FlushPairing();
    if (BbStats::enabled && (frame % 600) == 0 && history.stats.draws) {
        const auto& s = history.stats;
        std::printf("Object motion: %llu draws, %llu stored, %llu with history, %llu unmatched, "
                    "%llu capacity skips, %llu invalid, %llu still (600 frames); last frame %u/%u vertices; "
                    "index ranges %llu reused, %llu scanned (%llu found changed), %zu cached; paired %llu in order, "
                    "%llu searched (%llu indexes)\n",
                    (unsigned long long)s.draws, (unsigned long long)s.stored,
                    (unsigned long long)s.loaded, (unsigned long long)s.unmatched,
                    (unsigned long long)s.exhausted, (unsigned long long)s.invalid,
                    (unsigned long long)s.still,
                    history.Used(), PositionsPerFrame, (unsigned long long)index_ranges.stats.hits,
                    (unsigned long long)index_ranges.stats.scans,
                    (unsigned long long)index_ranges.stats.stale, index_ranges.Size(), (unsigned long long)s.in_order,
                    (unsigned long long)s.searched, (unsigned long long)s.indexes);
        index_ranges.stats = {};
        history.stats = {};
    }
    // bbport (object motion twins): every 600 frames while twins come in: how many draws went out without theirs and
    // what loading them cost (BuildMotionTwin); silent once nothing changes.
    if (Shader::MotionVectors::characters && (frame % 600) == 0) {
        const u64 gpu = g_twin_fallback_gpu.exchange(0, std::memory_order_relaxed);
        const u64 worker = g_twin_fallback_worker.exchange(0, std::memory_order_relaxed);
        const u64 built = g_twins_built_in_play.load(std::memory_order_relaxed);
        if (gpu || worker || built != twins_reported) {
            twins_reported = built;
            std::printf("Object motion twins: %llu draws without their twin yet (%llu selected on the GPU thread, %llu "
                        "by the workers; 600 frames); %llu twins loaded in play so far (%.1f ms in all, longest %.2f "
                        "ms; the driver compiled %llu, %llu with shaders translated in play)\n",
                        (unsigned long long)(gpu + worker), (unsigned long long)gpu, (unsigned long long)worker,
                        (unsigned long long)built, double(g_twin_load_ns.load()) / 1e6,
                        double(g_twin_load_max_ns.load()) / 1e6,
                        (unsigned long long)g_twins_compiled_in_play.load(),
                        (unsigned long long)g_twins_translated_in_play.load());
        }
    }
    if (gate_enabled) {
        gate_stats.no_pass += frame_camera_key && !frame_camera_checked;
        if ((BbStats::enabled || MotionCost::On()) && (frame % 600) == 0 && (gate_stats.draws || gate_stats.no_key)) {
            const auto& g = gate_stats;
            std::printf("Motion gate (per frame, 600 frames): %.1f motion draws, %.1f still (no store, history or "
                        "parameter%s), %.1f moved, %.1f another camera, %.1f in frames after an unconfirmed one, %.1f "
                        "without a key; frames: %llu camera confirmed by the velocity pass (worst error %.2g of a "
                        "row), %llu refuted, %llu without a pass; programs: %u cameras found, %u unusable; %zu keys "
                        "held\n",
                        double(g.draws + g.no_key) / 600.0, double(g.still) / 600.0,
                        gate_check ? "; CHECK: only these keep their vectors" : "", double(g.moved) / 600.0,
                        double(g.other_camera) / 600.0, double(g.frame_unconfirmed) / 600.0,
                        double(g.no_key) / 600.0, (unsigned long long)g.confirmed, double(g.worst_error),
                        (unsigned long long)g.refuted, (unsigned long long)g.no_pass, g.learned, g.unusable,
                        still.Size());
            gate_stats = {};
        }
        still.NextFrame();
        frame_camera_key = 0;
        frame_camera_checked = false;
        lessons.clear();
    }
    history.NextFrame();
    if (history_check) {
        history_check->NextFrame();
        if (frame % 600 == 0) {
            std::printf("Object motion history check: %llu draws compared with the keyed pairing, %llu differed; "
                        "%llu parameter indexes differed\n",
                        (unsigned long long)history_checked, (unsigned long long)history_differed,
                        (unsigned long long)index_differed);
        }
    }
    ++frame;
    if (frame % Motion::IndexRangeCache::Unused == 0) {
        index_ranges.Trim(frame);
    }
    const u32 slot = u32(frame % FrameSlots);
    // Do not overwrite host parameters still referenced by queued GPU work.
    if (params_ticks[slot]) {
        scheduler.Wait(params_ticks[slot]);
        params_ticks[slot] = 0;
    }
    params_used = 0;
    check_params_used = 0;
    written = false;
    scheduler.EndRendering();
    // Last frame's stores must be visible to this frame's loads. Also finish old reads
    // before the ping-pong half they reference is reused for writes.
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
}

u32 ObjectMotion::PrepareDraw(const DrawInfo& draw) {
    if (!enabled || BbToggle::Disabled(1u << 29) || params_used >= ParamsPerFrame) {
        return 0;
    }
    const u64 allocate_start = MotionCost::On() ? __rdtsc() : 0;
    const auto stored = history.Allocate(draw);
    if (allocate_start) {
        MotionCost::Add(MotionCost::BPair, __rdtsc() - allocate_start);
    }
    if (!stored.valid) {
        if (history_check) {
            deferred.push_back({draw, 0, 0}); // the keyed pairing counts it too, in order
        }
        return 0;
    }
    if (!stored.allocation.store) {
        // No room left for its positions this frame: it gets a parameter only with a load, which the pairing gives.
        FlushPairing();
        auto allocation = stored.allocation;
        history.Pair(draw, allocation);
        const u32 index = allocation.load ? NextParam() : 0;
        CheckAllocation(draw, allocation, index);
        if (index) {
            WriteParam(index, allocation);
        }
        return index;
    }
    // A store: the draw has a parameter whatever the pairing finds (FlushPairing).
    const u32 index = NextParam();
    deferred.push_back({draw, stored.allocation.store, index});
    return index;
}

void ObjectMotion::FlushPairing() {
    if (deferred.empty()) {
        return;
    }
    const u64 start = MotionCost::On() ? __rdtsc() : 0;
    for (const auto& d : deferred) {
        if (!d.index) {
            CheckAllocation(d.draw, {}, 0);
            continue;
        }
        Motion::History::Allocation allocation{d.store, 0, d.draw.vertices.count, d.draw.instances,
                                               d.draw.vertices.first, d.draw.first_instance};
        history.Pair(d.draw, allocation);
        CheckAllocation(d.draw, allocation, d.index);
        WriteParam(d.index, allocation);
    }
    if (start) {
        MotionCost::Add(MotionCost::BFlushDraws, deferred.size());
        MotionCost::Add(MotionCost::BFlushCycles, __rdtsc() - start);
    }
    deferred.clear();
}

u32 ObjectMotion::NextParam() {
    const u32 slot = u32(frame % FrameSlots);
    params_ticks[slot] = scheduler.CurrentTick();
    return 1 + slot * ParamsPerFrame + params_used++;
}

void ObjectMotion::WriteParam(u32 index, const Motion::History::Allocation& allocation) {
    const u32 flags = (allocation.store ? Shader::MotionVectors::FlagStore : 0) |
                      (allocation.load ? Shader::MotionVectors::FlagLoad : 0);
    u32* param = params_mapped + index * 8;
    param[0] = allocation.store;
    param[1] = allocation.load;
    param[2] = allocation.vertices;
    param[3] = flags;
    param[4] = allocation.first_vertex;
    param[5] = allocation.first_instance;
    param[6] = allocation.instances;
    param[7] = 0;
}

void ObjectMotion::CheckAllocation(const DrawInfo& draw, const Motion::History::Allocation& allocation, u32 index) {
    if (!history_check) {
        return;
    }
    const auto want = history_check->Prepare(draw);
    ++history_checked;
    history_differed += want.store != allocation.store || want.load != allocation.load ||
                        want.vertices != allocation.vertices || want.instances != allocation.instances ||
                        want.first_vertex != allocation.first_vertex ||
                        want.first_instance != allocation.first_instance;
    // The index before the pairing was deferred: the next of the frame's slot for a draw with a store or a load.
    const u32 expected = want.store || want.load
                             ? 1 + u32(frame % FrameSlots) * ParamsPerFrame + check_params_used++
                             : 0;
    index_differed += index != expected;
}

void ObjectMotion::EnsureImage(u32 width, u32 height) {
    if (image && width == image_width && height == image_height) {
        return;
    }
    const auto device = instance.GetDevice();
    if (image) {
        scheduler.Finish();
    }
    view.reset();
    image_layout = vk::ImageLayout::eUndefined;
    image = VideoCore::UniqueImage(device, instance.GetAllocator());
    image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    view = Check(device.createImageViewUnique({
        .image = vk::Image(image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    image_width = width;
    image_height = height;
    written = false;
}

void ObjectMotion::Attach(RenderState& state, u32 width, u32 height) {
    constexpr u32 slot = Shader::MotionVectors::Output;
    EnsureImage(width, height);
    auto& attachment = state.color_attachments[slot];
    attachment = {};
    attachment.image_view = *view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    if (!written || image_layout != vk::ImageLayout::eColorAttachmentOptimal) {
        // Clear once per frame; later reads must not erase the validity of this history.
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = image_layout,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = vk::Image(image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        // Recorded in order on the recording thread: CommandBuffer() here switched the rest of
        // the submission (most of the G-buffer) to direct recording on the GPU thread.
        scheduler.Record([barrier](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
        image_layout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.is_clear = !written;
        written = true;
    }
    state.num_color_attachments = std::max<u16>(state.num_color_attachments, slot + 1);
}

vk::ImageView ObjectMotion::PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height,
                                        bool& valid) {
    EnsureImage(std::max(image_width, 1u), std::max(image_height, 1u));
    // The scene fills the top left of targets that can be larger (Uncharted's aligned ones).
    valid = written && width <= image_width && height <= image_height &&
            (width == image_width || Shader::MotionVectors::characters) &&
            (height == image_height || Shader::MotionVectors::characters);
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = image_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    image_layout = vk::ImageLayout::eGeneral;
    return *view;
}

} // namespace Vulkan
