// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);
/// Every presented frame (FPS readout; BB_FPS_LOG=1 prints the rate every 5 seconds).
void NotePresent();
/// Every new frame of the game's own (not generated, not shown again): the game's frame rate,
/// lows and frame time graph of the FPS counter.
void NoteGameFrame();

/// Any thread: queues the notice of an earned trophy (grade 1 platinum .. 4 bronze, `png` the
/// trophy's icon file or empty). Notices are shown at the top of the screen one after the other.
void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char> png);

/// Any thread (bbport shader variant list): progress of the background shader preparation, shown in a corner
/// while it runs; `phase` 0 = none, 1 = shaders (`done` of `total`), 2 = pipelines.
void SetShaderPreparation(int phase, u32 done, u32 total);
/// The scan of the game files for shaders (a release without a shipped index), MiB read of MiB; total 0: not
/// scanning. Its own state: the pipeline warm-up may run beside it and sets the preparation phase meanwhile.
void SetScanProgress(u32 done_mib, u32 total_mib);

/// Any thread: the full-screen preparation page instead of the corner box. While it is on, the presenter shows blank
/// frames with the page and holds the game's flips (the game waits at its first frames until preparation is done).
void SetPreparationScreen(bool on);
/// The page shown on a later launch: the graphics driver compiles the shaders again (its own cache was reset, e.g.
/// after a driver update), so the page says why instead of "this happens once".
void SetPreparationRebuild(bool on);
/// The page also saves the prepared shaders before the game goes on (a full preparation in play): the bar keeps its
/// last 15 % for that step (phase 4), as without a window.
void SetPreparationSaves(bool on);
bool PreparationScreenActive();

/// Preparation without a window (BB_PREPARE_ONLY, the launcher's Prepare shaders button): the warm-up thread marks
/// its end, and one thread prints the progress for the launcher's bar as "PREP <percent> <done> <total> <step>|<eta>".
void SetPreparationFinished();
bool PreparationFinished();
void PrintPreparationProgress();

/// The menu is open or a text entry is shown: the game's input is held neutral.
bool CapturesInput();
/// ImGui draws its own cursor (the menu or the save list is open): the system cursor is hidden meanwhile (window.cpp).
bool DrawsCursor();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// The game's save list (sceSaveDataDialog list mode; ported from BilalReloaded's port, PR #349 by
/// the-best-is-best): `items` (lines split by a line break), each one's PNG picture (empty = none), the focused entry, and
/// the first entry that can be deleted (-1 none). The game's thread; false when the overlay is not ready. While it
/// is open the game gets no input. Keyboard, controller and mouse (hover highlights, click picks, wheel scrolls,
/// right button cancels).
/// `sections`: a heading above an entry (empty = none). An entry's first line is its name, drawn brighter.
bool BeginChoice(const std::string& title, std::vector<std::string> items,
                 std::vector<std::vector<unsigned char>> pngs, std::vector<std::string> sections, int focus,
                 int deletable_from);
/// -1 open, -2 cancelled, <= -3 delete entry -3 - n (the list closes), else the chosen entry (the list closes).
int PollChoice();
/// The game closed its dialog: the list goes.
void CancelChoice();

} // namespace BbOverlay
