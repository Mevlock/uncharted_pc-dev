// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: object motion vectors for temporal upscaling (docs/ROADMAP.md, step 1).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <span>
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/motion_still.h"

#include "common/types.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

/// bbport diagnosis (BB_MOTION_COST=1): CPU cycles of the object motion vectors on each thread (stage A's draw,
/// stage B's draw recording and its motion part, the draw-preparation workers), printed every 600 frames.
namespace MotionCost {
inline bool On() {
    static const bool on = [] {
        const char* env = std::getenv("BB_MOTION_COST");
        return env && env[0] == '1';
    }();
    return on;
}
enum Counter : u32 {
    ADraws, ACycles,           ///< stage A: Rasterizer::Draw
    BDraws, BCycles,           ///< stage B: DrawRecord (all draws)
    BMotionDraws, BMotionCycles, ///< stage B: DrawRecord of motion draws
    BIdentity, BOffsets, BHistory, ///< stage B: the motion block's parts (motion draws)
    BAttach,                   ///< stage B: ObjectMotion::Attach
    BPair,                     ///< stage B: History::Allocate (the store) within the history part
    BFlushDraws, BFlushCycles, ///< ObjectMotion::FlushPairing: draws paired after their recording, and the time
    CDraws, C0, C1, C2, C3, C4, ///< stage B: G-buffer candidates (twins, or the draws that would be): DrawRecord's parts
    ODraws, O0, O1, O2, O3, O4, ///< stage B: the other draws, the same parts
    RChunks, RCycles,          ///< the Vulkan recording thread: chunks executed and their time
    /// DrawRecord's last part split (G-buffer class, then others): to the motion block, the motion block, jitter,
    /// push/descriptors (Pipeline::BindResources), to the dynamic state, UpdateDynamicState, pass/barrier/BeginRendering,
    /// the draw's command, after it (to the function's end)
    CS0, CS1, CS2, CS3, CS4, CS5, CS6, CS7, CS8, OS0, OS1, OS2, OS3, OS4, OS5, OS6, OS7, OS8,
    /// Pipeline::BindResources by class (G-buffer, others): push constants, descriptors, the writes and descriptors
    /// pushed, draws taking the descriptor-set path (no push descriptors), the push data's bytes
    CB0, CB1, CB2, CB3, CB4, CB5, OB0, OB1, OB2, OB3, OB4, OB5,
    WDraws, WCycles,           ///< workers: prepared draws (PrepareGraphicsPipeline .. Ready)
    PrepUsed, PrepLate, PrepUnavailable, PrepChecksum, PrepRejected, ///< stage A: prepared draw outcome
    BGate, BGated,             ///< stage B: the still-draw gate's time (all motion draws) and the draws it took
    WStillDraws, WStill,       ///< workers: still keys made and their time
    BGateSeen,                 ///< stage B: the still table's part of the gate
    Count
};
/// Each thread's own counters (only it writes them, with plain loads and stores: one shared array of atomics put
/// lock-prefixed adds on cache lines the workers, stage A and stage B all wrote, and the timers measured that
/// contention more than the code they timed). Totals() sums them; the report takes the difference to the last one.
struct alignas(64) Block {
    std::array<std::atomic<u64>, Count> v{};
};
inline std::mutex blocks_mutex;
inline std::vector<Block*> blocks; ///< never freed: a thread's counters outlive it (a few hundred bytes a thread)
inline Block& Local() {
    thread_local Block* const block = [] {
        auto* b = new Block;
        std::scoped_lock lk{blocks_mutex};
        blocks.push_back(b);
        return b;
    }();
    return *block;
}
/// The class Pipeline::BindResources adds its parts to (CB0 / OB0; -1: not timed), set by DrawRecord around it.
inline thread_local int bind_base = -1;
inline void Add(Counter c, u64 v) {
    auto& x = Local().v[c];
    x.store(x.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline std::array<u64, Count> Totals() {
    std::array<u64, Count> t{};
    std::scoped_lock lk{blocks_mutex};
    for (const Block* b : blocks) {
        for (u32 i = 0; i < Count; ++i) {
            t[i] += b->v[i].load(std::memory_order_relaxed);
        }
    }
    return t;
}
} // namespace MotionCost

class Instance;
class Scheduler;

/// bbport (from BilalReloaded's Uncharted port, #84): a buffer-fetched G-buffer draw's identity across frames for
/// the object motion history: its vertex shader's buffers' sizes and strides, not their addresses (skinned vertices
/// can move between frames); `records`: its longest vertex stream (constant buffers, stride 16, and raw buffers,
/// stride 0, are not streams). The draw-preparation worker computes it with the draw (PreparedDraw); the recording
/// thread only for a draw that was not prepared.
/// `addresses` (still draws): a hash of the same buffers' addresses as well, the draw's mesh in guest memory.
inline u64 MotionDrawIdentity(const Shader::Info& vs, u32& records, u64* addresses = nullptr) {
    u64 hash = 0x6a09e667f3bcc909ULL, where = 0xbb67ae8584caa73bULL;
    records = 0;
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) continue;
        const auto buffer = resource.GetSharp(vs);
        if (!buffer.Valid()) continue;
        // (A multiply-mix, not XXH3: once per buffer of every G-buffer draw.)
        hash = (hash ^ (u64(buffer.num_records) << 32 | buffer.GetStride())) * 0x9e3779b97f4a7c15ULL;
        hash ^= hash >> 29;
        where = (where ^ buffer.base_address) * 0x9e3779b97f4a7c15ULL;
        where ^= where >> 29;
        if (buffer.GetStride() != 16 && buffer.GetStride() != 0) {
            records = std::max<u32>(records, buffer.num_records);
        }
    }
    if (addresses) {
        *addresses = where;
    }
    return hash;
}

/// bbport (still draws): the still data of the vertex shader program `pgm_hash` (motion_still.h), made at its first
/// use from the program's SPIR-V: the one just translated in this session (NoteVertexShaderCode), else its stored
/// binary. Never null and never freed; any thread.
Motion::StillProgram& StillProgramOf(u64 pgm_hash);
/// A vertex shader's SPIR-V as translated in this session (before the program's first draw).
void NoteVertexShaderCode(u64 pgm_hash, std::span<const u32> spv);
/// Whether the still-draw gate is on (BB_MOTION_GATE=0: off, every motion draw keeps its history).
inline bool StillGateOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_MOTION_GATE");
        return !(env && env[0] == '0');
    }();
    return on;
}
/// A motion draw's keys from its program's still data (state Ready) and its vertex shader's flat data: `still`, a
/// hash of `seed` (the mesh and the draw's numbers) and the position's flat words without the camera's; `camera`,
/// a hash of the view and projection matrices (never 0). still = 0: no key (the position's words are not known for
/// this draw's flat data).
struct StillKeys {
    u64 still{}, camera{};
};
StillKeys MakeStillKeys(Motion::StillProgram& program, std::span<const u32> flat, u64 seed);
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    using DrawInfo = Motion::Draw;
    /// The draw's parameter index (0: none) for its push constants. The parameter itself (the pairing with the
    /// previous frame's draw) is written by FlushPairing.
    u32 PrepareDraw(const DrawInfo& draw);
    /// Pairs the draws PrepareDraw left for later and writes their parameters: before every submission of the draw
    /// scheduler (the GPU reads them) and before the history goes on to the next frame.
    void FlushPairing();
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

    /// bbport (still draws): whether the still-draw gate is on (the character vectors on; BB_MOTION_GATE=0: off).
    [[nodiscard]] bool GateEnabled() const noexcept {
        return gate_enabled;
    }
    /// BB_MOTION_GATE_CHECK=1 (diagnosis, the vectors are NOT usable with it): only the draws the gate takes for
    /// still keep their object vectors, and the camera pass writes their difference to the camera's vectors.
    [[nodiscard]] bool GateCheck() const noexcept {
        return gate_check;
    }
    /// A prepared motion draw with its keys (PreparedDraw::still_key with the draw's index offset, camera_key), its
    /// program's still data (Ready) and its vertex shader's flat data: true when the draw stands still, i.e. its
    /// still key was drawn in the frame before, under the camera the frame's first such draw has, in a frame whose
    /// predecessor's camera was confirmed by the velocity pass. Such a draw needs no store, history or parameter.
    bool Still(const Motion::StillProgram& program, std::span<const u32> flat, u64 still_key, u64 camera_key);
    /// A motion draw of a program whose camera place is not known yet: its flat data is kept for the frame's
    /// velocity pass (OnVelocityClip).
    void LearnCameraFrom(u64 pgm_hash, Motion::StillProgram& program, std::span<const u32> flat);
    /// The game's velocity pass of this frame: its world-to-clip rows (16 floats). Confirms the frame's camera
    /// for the gate and finds the camera's place for the programs waiting.
    void OnVelocityClip(const float* clip);
    /// A motion draw the gate did not look at (not prepared, or its program has no still data).
    void CountNoKey() noexcept {
        ++gate_stats.no_key;
    }

    /// Start of a frame (display pass).
    void OnFrameStart();
    void InvalidateHistory() {
        FlushPairing();
        still.Forget();
        camera_confirmed_frame = ~0ull;
        history.NextFrame();
        history.NextFrame();
        if (history_check) {
            history_check->NextFrame();
            history_check->NextFrame();
        }
    }

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
    /// The next parameter of this frame's slot.
    u32 NextParam();
    void WriteParam(u32 index, const Motion::History::Allocation& allocation);
    /// BB_MOTION_HISTORY_CHECK: the keyed pairing for the same draw, compared, and the parameter index the draw got
    /// (`index`, its push constant) with the one the pairing at the draw gave (an index only for a store or a load).
    void CheckAllocation(const DrawInfo& draw, const Motion::History::Allocation& allocation, u32 index);

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
    /// BB_MOTION_HISTORY_CHECK=1 (diagnosis): 0.3.6.68's keyed pairing next to `history`, every allocation compared.
    std::unique_ptr<Motion::KeyedHistory> history_check;
    u64 history_checked = 0, history_differed = 0, index_differed = 0;
    u32 check_params_used = 0; ///< the history check's parameter count of this frame
    Motion::IndexRangeCache index_ranges;
    /// Draws whose store is allocated and whose pairing and parameter wait for FlushPairing, in draw order (index 0:
    /// an invalid draw, kept only for the history check's order). The pairing was a third of a motion draw's cost on
    /// the draw recording thread (~40 ns between the rest of each draw's work; in one pass the previous frame's walk
    /// stays in cache).
    struct Deferred {
        DrawInfo draw;
        u32 store;
        u32 index;
    };
    std::vector<Deferred> deferred;
    // The still-draw gate (motion_history.h StillDraws, motion_still.h).
    bool gate_enabled = false, gate_check = false;
    Motion::StillDraws still;
    u64 frame_camera_key = 0;             ///< the camera key of the frame's first draw with one (0: none yet)
    std::array<u32, 32> frame_camera{};   ///< that draw's view and projection matrices
    bool frame_camera_checked = false;    ///< the velocity pass of this frame was compared with them
    u64 camera_confirmed_frame = ~0ull;   ///< the last frame whose camera the velocity pass confirmed
    struct CameraLesson {
        u64 pgm_hash;
        Motion::StillProgram* program;
        std::vector<u32> flat;
    };
    std::vector<CameraLesson> lessons;    ///< this frame's draws of programs without a camera place
    struct GateStats {
        u64 draws{}, still{}, moved{}, other_camera{}, frame_unconfirmed{}, no_key{};
        u64 confirmed{}, refuted{}, no_pass{};
        u32 learned{}, unusable{};
        float worst_error{};
    } gate_stats;
    std::array<u64, FrameSlots> params_ticks{};
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params_mapped{};
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};

    u64 frame = 0;
    u64 twins_reported = 0; ///< twins loaded in play at the last object motion twins line
    u32 params_used = 0;
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    u32 image_width = 0, image_height = 0;
    bool written = false; ///< this frame's vectors are in the image
    vk::ImageLayout image_layout = vk::ImageLayout::eUndefined;
};

} // namespace Vulkan
