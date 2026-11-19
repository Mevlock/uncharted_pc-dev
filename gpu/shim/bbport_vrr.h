// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the FPS counter's live VRR indicator ("G-SYNC", "FreeSync" or "VRR"): lit while the
// display refreshes with the game's frames, dimmed ("... off") while it does not (the game not in
// front, composed by the desktop, a fixed refresh), as NVIDIA's own G-SYNC indicator switches.
//
// Sources, sampled four times a second while the FPS counter shows (none otherwise):
// - NVIDIA: NVAPI's VRR state of the display the game window is on (NvAPI_Disp_GetVRRInfo,
//   bIsVRREnabled "VRR mode currently active"). The display is found by the Windows adapter and
//   target of the window's monitor (NvAPI_Disp_GetDisplayIdInfo), never by its \\.\DISPLAYn name.
// - AMD without present timing: ADLX (amdadlx64.dll of Adrenalin), which has no live state: lit
//   only while FreeSync is on for the window's display (IADLXDisplayFreeSync; the display is the
//   single-display desktop at the monitor's place and size, on its Windows source), the window is
//   in front and covers that display, and the frames on screen are inside the display's FreeSync
//   range (ADL2_Display_FreeSync_Cap, capped by the current refresh rate; ADL's ability flags are
//   only logged); dimmed otherwise; hidden when the display is not certain.
// - Others (or BB_VRR_TIMING=1 beside NVAPI, for comparison): the swapchain's own report through
//   VK_EXT_present_timing: the presentation engine's VRR/fixed-refresh mode
//   (VkSwapchainTimingPropertiesEXT) and whether the frames reach the screen on the refresh grid
//   (composed) or when they are ready (flipped directly).
// A change shows after it held for half a second; each change is logged ("VRR: on -> off (...)").
// An option, off by default (bbport.ini show_vrr, the launcher and the in-game menu): off, no
// source is started or queried (BB_VRR_LOG=1 runs it for the log, the indicator stays hidden;
// BB_VRR_INDICATOR=1 starts it on whatever bbport.ini says; the in-game menu still switches it).
// BB_VRR=0: no indicator, nothing queried. BB_VRR_LOG=1: the raw sample in the log at each change and every 5 s.
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
void OnSwapchain(vk::SwapchainKHR swapchain, vk::PresentModeKHR present_mode);
void OnSwapchainDestroy();
/// Swapchain::Present (present thread): the structures to chain in front of `next` in the
/// present info (returns `next` when nothing is measured).
const void* PresentNext(const void* next);

/// The FPS counter (present thread): what to show, with the frames on screen per second (`fps`,
/// the AMD source's range check). label nullptr: nothing.
struct Shown {
    const char* label = nullptr; ///< "G-SYNC", "FreeSync", "VRR"
    bool active = false;
    float lit[3] = {0.45f, 0.85f, 0.25f}; ///< the colour while lit: G-SYNC's green, FreeSync AMD's red
};
Shown Get(float fps);

} // namespace BbVrr
