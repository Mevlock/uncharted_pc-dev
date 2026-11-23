// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (object motion, still draws): the still key's per-program data and the camera's place in the flat data.
//
// A draw is still when everything its positions follow from, but the camera, is what it was in the frame before
// (StillDraws, motion_history.h). For a vertex shader that takes its constants from the flattened user data
// (Uncharted) that is: the flat words its position reads (PositionInputs) without the view and projection
// matrices' words, the vertex streams and the draw's own numbers. Where the camera sits in the flat data is not
// known from the code: it is found by value, as the two 16-word matrices (view, then projection: clip = P V world)
// whose product is the world-to-clip matrix of the game's own velocity pass of the same frame, which is the camera
// the camera motion pass reprojects with. Nothing is taken for the camera without that match.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "motion_position_inputs.h" // (relative: the regression test includes this header alone)

namespace Vulkan::Motion {

/// Runs of consecutive words (hashed run by run).
struct WordRun {
    uint16_t first{}, count{};
    bool operator==(const WordRun&) const = default;
};

inline std::vector<WordRun> Runs(const std::vector<uint16_t>& words) {
    std::vector<WordRun> runs;
    for (const uint16_t word : words) {
        if (!runs.empty() && runs.back().first + runs.back().count == word) {
            ++runs.back().count;
        } else {
            runs.push_back({word, 1});
        }
    }
    return runs;
}

/// A row-major 4x4 product C = P V of two matrices given as float bits, against `clip` (16 floats): every
/// element within `tolerance` of its row's largest magnitude.
inline bool ProductIs(const uint32_t* view, const uint32_t* proj, const float* clip, float tolerance,
                      float* error = nullptr) {
    float v[16], p[16];
    std::memcpy(v, view, sizeof(v));
    std::memcpy(p, proj, sizeof(p));
    float worst = 0.0f;
    for (int r = 0; r < 4; ++r) {
        float row[4], scale = 0.0f;
        for (int c = 0; c < 4; ++c) {
            row[c] = p[r * 4] * v[c] + p[r * 4 + 1] * v[4 + c] + p[r * 4 + 2] * v[8 + c] + p[r * 4 + 3] * v[12 + c];
            scale = std::max(scale, std::fabs(clip[r * 4 + c]));
        }
        if (!(scale > 0.0f) || !std::isfinite(scale)) {
            return false; // an empty or broken row: no camera
        }
        for (int c = 0; c < 4; ++c) {
            const float e = std::fabs(row[c] - clip[r * 4 + c]) / scale;
            if (!(e <= 1.0f)) { // also NaN
                worst = 2.0f;
            }
            worst = std::max(worst, e);
        }
    }
    if (error) {
        *error = worst;
    }
    return worst <= tolerance;
}

/// How close the flat data's matrices must be to the velocity pass's world-to-clip matrix (relative to each row's
/// largest element): the game multiplies the same two matrices itself, in its own order of operations.
inline constexpr float CameraTolerance = 2e-4f;

/// Finds the view and projection matrices in `flat` among the position's words: both runs of 16 words of `words`,
/// the view's last row (0, 0, 0, 1). True with their first words when exactly one pair matches `clip`.
inline bool FindCamera(std::span<const uint32_t> flat, const std::vector<uint16_t>& words, const float* clip,
                       uint16_t& view_at, uint16_t& proj_at, float* error = nullptr) {
    std::vector<uint16_t> starts; // first words of 16 consecutive position words inside the flat data
    for (const auto& run : Runs(words)) {
        for (uint32_t s = run.first; run.count >= 16 && s + 16 <= uint32_t(run.first) + run.count; ++s) {
            if (s + 16 <= flat.size()) {
                starts.push_back(uint16_t(s));
            }
        }
    }
    static constexpr uint32_t one = 0x3f800000u;
    uint32_t found = 0;
    float best = 1e9f;
    for (const uint16_t v : starts) {
        const uint32_t* row = &flat[v + 12];
        if ((row[0] << 1) != 0 || (row[1] << 1) != 0 || (row[2] << 1) != 0 || row[3] != one) {
            continue; // not an affine matrix
        }
        for (const uint16_t p : starts) {
            if (p < v + 16u && v < p + 16u) {
                continue; // overlapping
            }
            float e = 0.0f;
            const bool match = ProductIs(&flat[v], &flat[p], clip, CameraTolerance, &e);
            best = std::min(best, e);
            if (match) {
                view_at = v;
                proj_at = p;
                ++found;
            }
        }
    }
    if (error) {
        *error = best;
    }
    return found == 1;
}

/// The words of `words` outside the two camera matrices, as runs; false when a matrix is not wholly among them.
inline bool WithoutCamera(const std::vector<uint16_t>& words, uint16_t view_at, uint16_t proj_at,
                          std::vector<WordRun>& runs) {
    std::vector<uint16_t> rest;
    uint32_t camera = 0;
    for (const uint16_t word : words) {
        if ((word >= view_at && word < view_at + 16u) || (word >= proj_at && word < proj_at + 16u)) {
            ++camera;
            continue;
        }
        rest.push_back(word);
    }
    runs = Runs(rest);
    return camera == 32;
}

/// A vertex shader program's part of the still key. Created once per program (StillProgramOf) and never freed.
/// `code` never changes; the camera's place is written once, before `state` leaves NoCamera; the variants (the
/// position's words for each way the flat data lets the code run) are added as draws bring them, under the mutex,
/// and read without it.
struct StillProgram {
    enum State : uint8_t {
        NoCamera, ///< the code is understood, the camera's place in its flat data is not known yet
        Ready,
        Unusable, ///< the code was not understood, or no camera was found in its flat data
    };
    struct Variant {
        uint64_t config{};
        bool valid{};                  ///< false: the position's words are not known for it (never still)
        uint32_t words{};              ///< how many flat words the position reads
        std::vector<WordRun> key_runs; ///< those words without the camera's
    };
    static constexpr uint32_t MaxVariants = 8;

    std::unique_ptr<PositionCode> code;
    const char* why = ""; ///< Unusable: the reason
    uint16_t view_at = 0, proj_at = 0;
    uint32_t tries = 0;   ///< draws looked at for the camera (the recording thread's)
    std::atomic<uint8_t> state{NoCamera};
    std::array<Variant, MaxVariants> variants;
    std::atomic<uint32_t> num_variants{0};
    std::mutex variants_mutex;

    /// State Ready: the variant of a draw's flat data; null when its position's words are not known.
    const Variant* VariantFor(std::span<const uint32_t> flat) {
        const uint64_t config = code->Config(flat);
        if (config == PositionCode::NoConfig) {
            return nullptr;
        }
        const auto find = [&](uint32_t count) -> const Variant* {
            for (uint32_t i = 0; i < count; ++i) {
                if (variants[i].config == config) {
                    return &variants[i];
                }
            }
            return nullptr;
        };
        const Variant* variant = find(num_variants.load(std::memory_order_acquire));
        if (!variant) {
            std::scoped_lock lk{variants_mutex};
            const uint32_t count = num_variants.load(std::memory_order_relaxed);
            variant = find(count);
            if (!variant) {
                if (count == MaxVariants) {
                    return nullptr;
                }
                Variant& made = variants[count];
                made.config = config;
                const auto inputs = code->Inputs(flat);
                made.words = uint32_t(inputs.words.size());
                made.valid = inputs.valid && WithoutCamera(inputs.words, view_at, proj_at, made.key_runs);
                num_variants.store(count + 1, std::memory_order_release);
                variant = &made;
            }
        }
        return variant->valid ? variant : nullptr;
    }

    /// The recording thread, with a draw's flat data and the frame's velocity clip matrix: finds the camera's
    /// place (state Ready). `words`: how many flat words the position reads for this draw.
    bool LearnCamera(std::span<const uint32_t> flat, const float* clip, float* error = nullptr,
                     size_t* words = nullptr) {
        const auto inputs = code->Inputs(flat);
        if (words) {
            *words = inputs.words.size();
        }
        uint16_t v = 0, p = 0;
        if (!inputs.valid || !FindCamera(flat, inputs.words, clip, v, p, error)) {
            return false;
        }
        view_at = v;
        proj_at = p;
        state.store(Ready, std::memory_order_release);
        return true;
    }
};

} // namespace Vulkan::Motion
