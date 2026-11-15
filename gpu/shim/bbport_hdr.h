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
    /// Where the extended highlights begin: the SDR luminance (0-1) of the game's tone mapped
    /// picture above which the scene signal takes over (0: the earlier ramp from mid grey).
    float highlight_start;
    /// Scene tone mapping: 0 RenoDX tone pass (as before), 1 the game's own curve extended past
    /// its inflection point (RenoDX's The Last of Us Part II method), 2 the game's SDR picture
    /// exact below a knee and the scene's own highlights above it, rolled off to the peak.
    float scene_mode;
    /// Mode 2: the knee, SDR luminance (0-1) of the game's picture.
    float knee;
    /// Compare: the left part of the screen (0-1) shows the game's SDR picture at paper white.
    float split;
    /// Mode 2: the share (0-1) of the scene's highlight range shown, in stops.
    float strength;
    float reserved[1];
    // The game's SDR picture in HDR10 (post process pass): decode gamma, Oklab chroma scale.
    float sdr_gamma;
    float sdr_saturation;
    /// Both outputs (the SDR picture and the scene HDR composite), the last step before PQ:
    /// 0 accurate (Rec. 709 exactly in Rec. 2020), 1 the colours read as Display P3, 2 as
    /// Rec. 2020 (as wide-gamut screens stretch SDR); luminance kept.
    float gamut_expansion;
    /// Scene HDR composite: the HUD's colours read as Display P3 (1) instead of Rec. 709 (0).
    float hud_vivid;
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

/// Live tuning from a file (the presenter, each frame, HDR10 output only): BB_HDR_TUNE_FILE, else
/// <user dir>/hdr_tune.txt (BB_GPU_USER_DIR). Its modification time is checked every 250 ms; a
/// change after start applies its `key=value` lines (bbport.ini's hdr_* keys) as if the menu's
/// sliders were moved, logs each changed value and writes bbport.ini.
void PollTuneFile();

} // namespace BbHdr
