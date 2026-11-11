// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox button prompts drawn as BilalReloaded's port draws them (bbport_xbox_bilal.cpp).
#pragma once

#include <cstdint>

namespace BbXboxBilal {

/// Bilal's drawing for the Xbox set (the default); BB_XBOX_STYLE=antik: antikmewzik's pictures.
bool Active();
/// The atlas (512x512 straight RGBA, the game's original): the Xbox cells redrawn.
bool PaintAtlas(uint8_t* rgba);
/// A face button picture (straight RGBA, opaque; upside_down: stored with its rows turned over): the symbol becomes `label`
/// ("#A" "#B" "#X" "#Y") and the glow the button's colour. False when the button is not found (picture unchanged).
bool RedrawFacePicture(uint8_t* rgba, int w, int h, bool upside_down, const char* label);
/// A shoulder button picture (U3's dark ones): "L1" etc. becomes `label` ("#LB" "#RB" "#LT" "#RT").
bool RedrawShoulderPicture(uint8_t* rgba, int w, int h, bool upside_down, const char* label);

} // namespace BbXboxBilal
