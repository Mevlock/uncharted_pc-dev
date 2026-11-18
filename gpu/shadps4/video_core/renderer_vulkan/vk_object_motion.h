// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: object motion vectors for temporal upscaling (docs/ROADMAP.md, step 1).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <chrono>

#include <algorithm>
#include <array>
#include "video_core/renderer_vulkan/motion_history.h"

#include "common/types.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// bbport (from BilalReloaded's Uncharted port, #84): a buffer-fetched G-buffer draw's identity across frames for
/// the object motion history: its vertex shader's buffers' sizes and strides, not their addresses (skinned vertices
/// can move between frames); `records`: its longest vertex stream (constant buffers, stride 16, and raw buffers,
/// stride 0, are not streams). The rasterizer computes it, or takes the draw-preparation worker's (PreparedDraw).
inline u64 MotionDrawIdentity(const Shader::Info& vs, u32& records) {
    u64 hash = 0x6a09e667f3bcc909ULL;
    records = 0;
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) continue;
        const auto buffer = resource.GetSharp(vs);
        if (!buffer.Valid()) continue;
        // (A multiply-mix, not XXH3: once per buffer of every G-buffer draw.)
        hash = (hash ^ (u64(buffer.num_records) << 32 | buffer.GetStride())) * 0x9e3779b97f4a7c15ULL;
        hash ^= hash >> 29;
        if (buffer.GetStride() != 16 && buffer.GetStride() != 0) {
            records = std::max<u32>(records, buffer.num_records);
        }
    }
    return hash;
}
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    using DrawInfo = Motion::Draw;
    u32 PrepareDraw(const DrawInfo& draw);
    /// Small skeletons: whether the bone palette changed (Motion::History::Moving).
    bool Moving(const Motion::History::GateKey& key, u64 palette) {
        return history.Moving(key, palette);
    }
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan) {
        return index_ranges.Get(key, frame, scan);
    }

    /// Rendering of a motion pipeline: the motion attachment (render-target size).
    void Attach(RenderState& state, u32 width, u32 height);

    /// Start of a frame (display pass).
    void OnFrameStart();
    /// BB_FRAME_STATS: cycles the GPU thread spent on a motion draw's history (and its parts), for the stats line.
    void AddCycles(u64 total, u64 identity) {
        cycles_total += total;
        cycles_identity += identity;
        ++cycles_draws;
    }
    void InvalidateHistory() { history.NextFrame(); history.NextFrame(); }

    /// The motion image for the compose pass (layout General after this call) and whether it
    /// holds this frame's vectors at `width` x `height`.
    vk::ImageView PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height, bool& valid);
    /// The image of the last PrepareRead (layout General), or null.
    [[nodiscard]] vk::ImageView View() const noexcept {
        return view ? *view : vk::ImageView{};
    }
    [[nodiscard]] vk::Image Image(u32 width, u32 height) const noexcept {
        return written && width <= image_width && height <= image_height
            ? vk::Image(image) : vk::Image{};
    }

private:
    void EnsureImage(u32 width, u32 height);
    u64 cycles_total = 0, cycles_identity = 0, cycles_draws = 0;
    u64 cycles_tsc0 = 0;
    std::chrono::steady_clock::time_point cycles_t0{};

    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false;

    // Parameter ring (host visible): FrameSlots frames of ParamsPerFrame entries (two u32x4),
    // element 0 all zero (motion off).
    static constexpr u32 FrameSlots = 4;
    static constexpr u32 ParamsPerFrame = 8192;
    // Positions (device local): two halves (current/previous frame) of vec4; element 0 is reserved.
    static constexpr u32 PositionsPerFrame = 4u << 20;
    Motion::History history{PositionsPerFrame};
    Motion::IndexRangeCache index_ranges;
    std::array<u64, FrameSlots> params_ticks{};
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params_mapped{};
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};

    u64 frame = 0;
    u32 params_used = 0;
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    u32 image_width = 0, image_height = 0;
    bool written = false; ///< this frame's vectors are in the image
    vk::ImageLayout image_layout = vk::ImageLayout::eUndefined;
};

} // namespace Vulkan
