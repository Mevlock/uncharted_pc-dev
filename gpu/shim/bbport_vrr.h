// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FPS counter's live VRR indicator ("G-SYNC", "FreeSync" or "VRR"): lit while the
// display refreshes with the game's frames, dimmed ("... off") while it does not (the game not in
// front, composed by the desktop, a fixed refresh), as NVIDIA's own G-SYNC indicator switches.
//
// Sources, sampled four times a second while the FPS counter shows (none otherwise):
// - NVIDIA: NVAPI's VRR state of the display the game window is on (NvAPI_Disp_GetVRRInfo,
//   bIsVRREnabled "VRR mode currently active"). The display is found by the Windows adapter and
//   target of the window's monitor (NvAPI_Disp_GetDisplayIdInfo), never by its \\.\DISPLAYn name.
// - Others (or BB_VRR_TIMING=1 beside NVAPI, for comparison): the swapchain's own report through
//   VK_EXT_present_timing: the presentation engine's VRR/fixed-refresh mode
//   (VkSwapchainTimingPropertiesEXT) and whether the frames reach the screen on the refresh grid
//   (composed) or when they are ready (flipped directly).
// A change shows after it held for half a second; each change is logged ("VRR: on -> off (...)").
// BB_VRR=0: no indicator, nothing queried. BB_VRR_LOG=1: every raw sample in the log.
#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace BbVrr {

/// The game window (window thread, once it exists).
void SetWindow(void* hwnd);

/// Instance::CreateDevice, before the device is made: the device extensions and the feature
/// structures (a chain, nullptr: none) of the timing source. `reflex_present_id`: low latency
/// mode chains VkPresentIdKHR to the presents (the timing source then stays off).
void* PrepareDevice(vk::PhysicalDevice physical, const std::vector<std::string>& available,
                    std::vector<const char*>& extensions, bool reflex_present_id);
/// After the device is made.
void OnDevice(vk::Device device);

/// Swapchain::Create: the create flags to add for `surface`; OnSwapchain once it is made,
/// OnSwapchainDestroy before it goes.
vk::SwapchainCreateFlagsKHR SwapchainFlags(vk::PhysicalDevice physical, vk::SurfaceKHR surface);
void OnSwapchain(vk::SwapchainKHR swapchain);
void OnSwapchainDestroy();
/// Swapchain::Present (present thread): the structures to chain in front of `next` in the
/// present info (returns `next` when nothing is measured).
const void* PresentNext(const void* next);

/// The FPS counter (present thread): what to show. label nullptr: nothing.
struct Shown {
    const char* label = nullptr; ///< "G-SYNC", "FreeSync", "VRR"
    bool active = false;
};
Shown Get();

} // namespace BbVrr
