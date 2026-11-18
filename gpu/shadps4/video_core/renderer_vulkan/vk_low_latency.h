// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: low latency mode. NVIDIA Reflex (VK_NV_low_latency2) or AMD Anti-Lag 2
// (VK_AMD_anti_lag), both straight through Vulkan (no vendor SDK).
//
// Every marker carries the game's own frame number (g_frameState.m_frameCount, +1 per frame, the
// number the games also pass as their flip argument), as id N + 1 (0 stays "no id"):
// - the game's frame start (the entry of its main loop's frame function, probe.c): the driver's
//   sleep (NVIDIA: vkLatencySleepNV; AMD: inside its INPUT update), then SIMULATION_START and
//   INPUT_SAMPLE, so the game starts its frame as late as the GPU allows;
// - the game's submit and flip of that frame (by its flip argument): SIMULATION_END and
//   RENDERSUBMIT_START/END;
// - our present of that flip (by the flip argument the flip request carries): PRESENT_START/END
//   and VkPresentIdKHR.
// A stage marks a frame only after the stage before it did, ids only going up.
//
// BB_LOW_LATENCY: 0 off (default), 1 on, 2 on with NVIDIA's boost (GPU clocks stay up),
// m: markers only (the driver's timings for A/B, no sleep).
// BB_REFLEX_LOG=1: about once a second, the id sequence of each stage, its gaps and repeats.
#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::LowLatency {

/// Instance::CreateDevice, before the device is made: adds the extensions this needs and returns
/// the feature structures to chain at the end of the device's features (nullptr: none).
void* PrepareDevice(vk::PhysicalDevice physical, const std::vector<std::string>& available,
                    std::vector<const char*>& extensions);
/// After the device is made (the dispatcher holds its functions).
void OnDevice(vk::Device device);

/// Swapchain::Create: chained into the create info (nullptr: none); OnSwapchain once made,
/// OnSwapchainDestroy before it goes.
const void* SwapchainNext();
void OnSwapchain(vk::SwapchainKHR swapchain);
void OnSwapchainDestroy();

/// The runtime, once after its code patches (before the game runs): whether the game's frame
/// start was found. Not found: the low latency mode switches itself off (no correct place for the
/// sleep, no id for the start).
void SetFrameSource(bool found);
/// The game's frame `frame` starts (its own number; `kicked`: KickJob's last number then, for the
/// log; ~0: not known). May sleep.
void GameFrameStart(u64 frame, u64 kicked);
/// The guest reads the controller (any thread): counted for the log only.
void InputSample();
/// The guest hands the frame with this flip argument to the GPU (submit and flip).
void RenderSubmit(s64 flip_arg);

/// Presenter: the id for the present of the game's flip with this argument (0: no id; -1: not a
/// game flip).
u64 PresentId(s64 flip_arg);
/// Around the swapchain present of that frame (id from PresentId; 0: nothing to mark).
void PresentStart(u64 id);
void PresentEnd(u64 id);
/// VkPresentIdKHR is chained to the presents.
bool UsesPresentId();

/// BB_LATENCY_TRACE=1 (diagnosis): the video driver's stages of the game's flip with this argument:
/// the GPU reached it (its commands are done in order before it) and the present thread took it.
enum class FlipStage { GpuReached, Taken };
void TraceFlip(FlipStage stage, s64 flip_arg);

/// Frame pacing asked for (BB_LATENCY_PACING not 0, the default): the video driver then keeps the
/// Frame rate as a cap at the game's frame start and shows frames as they come (without the
/// game's frame start hook the present thread's slot hold keeps the cap).
bool StartPacingRequested();

/// Even presents (vk_low_latency.cpp Pace; BB_EVEN_PRESENT=0: off): the present target of the
/// game's flip with this argument (its frame start's slot + the offset; 0: none).
/// With VK_EXT_present_timing (SetTimedPresents(true), bbport_vrr.cpp): the present is made at
/// once and carries its target; PresentQueued notes the ready time, PresentFlipped the flip the
/// presentation engine reported (late ones raise the offset). Without it (build-1 path):
/// PresentReadyAndWait before the present notes the ready time and holds it until the target.
s64 PresentTarget(s64 flip_arg);
void PresentReadyAndWait(s64 flip_arg);
void SetTimedPresents(bool on);
/// The GPU gate (vk_low_latency.cpp Pace::gate): the GPU thread after a game flip it queued
/// (waits, with a deadline, until that frame's post work was submitted); the presenter once it
/// has submitted it.
void GateAfterFlip(s64 flip_arg);
/// Even presents are on (the frame start paces and BB_EVEN_PRESENT is not 0).
bool EvenPresents();
/// How the presenter holds paced frames (vk_present_release.h): 0 on its thread, 1 on the GPU in the
/// game's queue, 2 on the release queue.
void SetGpuHolds(int mode);
/// Reflex: the release queue presents beside the game's queue (out of band, not its frames' work).
void MarkOutOfBandPresentQueue(vk::Queue queue);
bool GpuRelease();
/// A GPU hold of the game's flip with this argument has run: the frame was ready on the GPU at
/// `ready_ns`, its present work released at `release_ns` (steady clock).
void PresentGpuTimes(s64 flip_arg, s64 ready_ns, s64 release_ns);
void GatePostSubmitted(s64 flip_arg);
void PresentQueued(s64 flip_arg);
void PresentFlipped(s64 flip_arg, s64 flip_ns);
/// The frame thread, right after GameFrameStart: the time from now to that frame's present target
/// (0: none), for the frame clock (the world steps = the intervals between the targets).
s64 PresentLeadNs();
/// GPU thread, while it records a game frame (before its flip): that frame's world step, the time
/// from the last frame's start to its own as the game's frame clock has them (paced: the interval
/// between their present targets, i.e. between their pictures on screen; else between their
/// starts). 0: not known (no frame start hook, the first frame). For the upscalers' frame time.
s64 FrameStepNs();

/// "Reflex", "Anti-Lag 2" while active, else nullptr (FPS counter).
const char* Name();

} // namespace Vulkan::LowLatency
