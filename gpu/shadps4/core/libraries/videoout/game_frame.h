// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's own frame number in reports (crash, device lost, hitch, stall, frame stats).
//
// Each game counts its frames in g_frameState.m_frameCount (+1 per pass of its main loop, never reset) and passes
// that number as the flip argument of the frame it draws, so one number follows a frame through every stage:
// - started: m_frameCount, read in the game's memory (probe.c finds it; the low latency mode's frame start reads the
//   same field): the frame that runs (or, between the loop's increment and the next pass, the one about to start);
// - submitted: the flip argument of the last flip the GPU command thread reached (that frame's commands were all
//   handed to Vulkan; g_gpu_flip_arg, the projection jitter's game side);
// - presented: the flip argument of the last game flip whose present finished.
// Nothing runs per frame for this beyond one relaxed store at the present; it is read only when a line is printed.
#pragma once

#include <cstddef>

#include "common/types.h"

namespace Libraries::VideoOut {

/// -1: none yet, or not known (an unknown program: no frame number).
struct GameFrames {
    s64 started;
    s64 submitted;
    s64 presented;
};
GameFrames CurrentGameFrames();

/// "game frame N" (full: "game frame N started, S submitted, P presented"); "none" for a stage that has no frame yet,
/// "game frame unknown" without the number. Writes into `out` and returns it.
const char* GameFrameText(char* out, std::size_t size, bool full = false);

} // namespace Libraries::VideoOut
