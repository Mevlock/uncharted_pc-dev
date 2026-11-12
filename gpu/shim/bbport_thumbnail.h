// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a small picture of the game for the save list, ported from BilalReloaded's port (gow3_thumbnail.h,
// PR #349 by the-best-is-best). The presenter scales a game frame down about once a second while the game plays
// (a follow / cover camera ran: not in a menu or a cutscene), before the overlay is drawn; a save the game writes
// then gets the last one as thumb.png beside its param (runtime_savedata.c, on unmount). BB_SAVE_PICTURE=0: off.

#pragma once

#include <cstdint>
#include <string>

namespace BbThumbnail {

constexpr uint32_t Width = 456, Height = 256; // twice the save list's 228x128 picture

/// Present thread: whether to capture this frame (a second since the last request, and the game in play).
bool Wanted();
/// Present thread: the captured picture, Width x Height RGBA8 rows top to bottom.
void Store(const unsigned char* rgba);
/// Upscaler (vk_temporal_upscaler.cpp): whether to capture the scene before the UI now (about once a second).
bool WantedClean();
/// Upscaler: the scene before the UI, Width x Height RGBA8 (sRGB-encoded) rows top to bottom.
void StoreClean(const unsigned char* rgba);
/// The game's save list ("Save game") was shown: the saves written before play goes on (at most two minutes) get
/// the picture from before the pause menu (bbgpu_thumbnail_pause).
void NoteSaveList();
/// A save's folders are replaced (a slot put back, bbport_saves.cpp): its picture still waiting for play is
/// dropped, so it does not land on the save put in its place. `meta_name`: "SAVEDATA020.sce_sys".
void ForgetSave(const std::string& meta_name);
/// The game loads one of its campaign saves: the pictures so far are dropped (the play before it, the title screen).
/// `meta_name`: "SAVEDATA031.sce_sys".
void Loading(const std::string& meta_name);
/// The game opens one of its campaign saves (read or write): opening another game drops the pictures so far, unless
/// it is saved by hand into it (the play goes into it).
void CampaignOpened(const std::string& meta_name);
/// The game opens one of its campaign saves to write it: its first write after the start or a load (the save it
/// writes as play begins) drops the pictures so far once more (the title screen, menus, the loading).
void Writing(const std::string& meta_name);

} // namespace BbThumbnail
