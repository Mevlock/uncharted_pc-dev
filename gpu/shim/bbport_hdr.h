// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HDR10 output (the launcher's Display > HDR). Read at start; nothing of it runs with HDR
// off. Based on moistman7's PR #156 to BilalReloaded's port (HDR10 with Uncharted 2 scene HDR).
#pragma once

namespace BbHdr {

/// BB_HDR=1: HDR10 output wanted (the swapchain colour space extension is enabled then).
bool Requested();
/// BB_HDR_PEAK_NITS (400-10000, default 1000): the brightest highlights of the scene. Live: the
/// menu's HDR section changes it (BbSettings::hdr_peak_nits).
float PeakNits();
/// BB_HDR_PAPER_WHITE (80-500, default 203): the game's white (SDR 1.0), the HUD and the menus.
/// Live, like PeakNits.
float PaperWhite();

/// The live HDR picture settings (BbSettings, the menu's HDR section), read once per frame.
struct Tuning {
    // Scene HDR: the tone pass replacement's settings buffer, in this order (16 floats, the
    // offsets gpu/hdr/fs_3371ae8d_hdr.hlsl reads).
    float peak_nits;
    float paper_white;
    float exposure;
    float saturation;
    float highlight_saturation;
    float contrast;
    float chrominance; ///< RenoDX per-channel chrominance correction
    float blowout;     ///< RenoDX per-channel blowout restoration
    float hue_correction;
    float per_channel; ///< 1: per-channel tone mapping, 0: luminance
    float reserved[6];
    // The game's SDR picture in HDR10 (post process pass): decode gamma, Oklab chroma scale.
    float sdr_gamma;
    float sdr_saturation;
};
[[nodiscard]] Tuning Current();

/// Set once by the swapchain at start: HDR10 output is on (requested and the display offers it).
void SetOutput(bool on);
[[nodiscard]] bool Output();

/// Scene HDR (Uncharted 2's tone pass drawn again with the HDR replacement, gpu/hdr): with HDR10
/// output, in Uncharted 2 (BB_HDR_SCENE=1: every game, 0: none). The GPU checks are the
/// pipeline's (NVIDIA barycentric profile).
[[nodiscard]] bool Scene();

/// BB_HDR_FRAME_GEN=0: frame generation stays off while HDR is on.
[[nodiscard]] bool FrameGenAllowed();

} // namespace BbHdr
