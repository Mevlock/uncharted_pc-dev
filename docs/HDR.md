# HDR (branch feature/hdr)

Work in progress, started 2026-11-08 on top of release 0.3.6.48. Off by default; nothing here changes the game
with HDR off.

The aim: the picture looks like the game's own SDR picture, with real HDR highlights on top. Faces, walls,
foliage and the HUD keep the colours and brightness Naughty Dog graded; only what SDR has to squash (the sun,
sky, fire, glints, bright reflections) goes brighter than paper white.

Based on moistman7's HDR pull request #156 to BilalReloaded's port (HDR10 output and Uncharted 2 scene HDR with
a RenoDX tone pass replacement). RenoDX (MIT) is in `gpu/hdr/renodx` with its licence and `SOURCE.txt`.

## How it works

1. **HDR10 output** (`BB_HDR=1`, launcher Display > HDR). The swapchain takes an HDR10 (ST 2084) format at start
   (A2B10G10R10, then A2R10G10B10, then RGBA16F). If the display offers none, the game stays SDR and the log
   lists what the display offers. Frames become A2B10G10R10; the post-process pass converts the picture to PQ:
   decode with gamma 2.2 (as an SDR screen shows it), white at paper white, Rec. 709 to Rec. 2020, clamped at the
   peak. Our overlay is drawn in 8 bits and blended onto the HDR image at paper white.
2. **Scene HDR.** The games' tone pass (pixel shader `3371ae8d`, the same shader in all three games and in both
   game code versions) is drawn a second time with the replacement shader `gpu/hdr/fs_3371ae8d_hdr.hlsl` into a
   float "tone shadow"; the pass after it (`484cdbf5`) is drawn again into a float "scene shadow". DLSS, FSR 4,
   XeSS and TAA read the scene shadow instead of the 8-bit frame. At the flip the HDR scene is shown wherever the
   HUD did not draw; under HUD pixels it is kept in proportion to how much the HUD covers. Frame generation gets
   the HDR scene as its picture.
   - On by default in Uncharted 2; `BB_HDR_SCENE=1` turns it on in every game (U1 and U3 use the same tone pass),
     `0` turns it off.
   - Needs NVIDIA with fragment shader barycentrics and the render-resolution path (a render resolution set; a
     1080p screen at Native does not use it). Elsewhere: HDR10 output of the SDR picture.
3. **Highlights start** (`2bfffa51`). Below this SDR brightness (luminance of the game's own filmic output, 0-1)
   the game's SDR value is used unchanged; above it the picture blends smoothly (smoothstep over 60% of the
   remaining range) into the extended HDR signal. Without it, everything brighter than mid-grey was lifted, so
   whole sunlit streets went above paper white and lost their colour (measured: 22% of the Uncharted 3 Cartagena
   street at 300-700 nits). `0` restores the old ramp for comparison.

## Settings

Launcher Display > HDR: On/Off, peak brightness (nits), paper white (nits). In game: Insert > Settings > HDR
(only shown with HDR10 output), applied live, saved to `bbport.ini` (`hdr_*` keys) when the menu closes; env
vars `BB_HDR_*` override the file. Paper white and peak come from the launcher on every start.

| Setting | Key | Range | Default (code) | Tuned 2026-11-08 |
|---|---|---|---|---|
| Paper white | `hdr_paper_white` | 80-500 | 203 (launcher) | 300 (user's room/monitor) |
| Peak brightness | `hdr_peak_nits` | 400-10000 | 1000 (launcher) | 1000 (monitor peak) |
| Exposure | `hdr_exposure` | 0.25-4 | 1.00 | **1.05** |
| Highlights start | `hdr_highlight_start` | 0.10-0.95 | 0.60 | **0.70** |
| Saturation | `hdr_saturation` | 0-2 | 1.10 | **1.05** |
| Highlight saturation | `hdr_highlight_saturation` | 0-2 | 1.00 | **0.90** |
| Contrast | `hdr_contrast` | 0.5-2 | 1.00 | **1.10** |
| Colour restoration (per-channel chrominance correction) | `hdr_chrominance` | 0-1 | 1.00 | 1.00 |
| Blowout restoration | `hdr_blowout` | 0-1 | 0.00 | **0.25** |
| Hue correction | `hdr_hue_correction` | 0-1 | 1.00 | 1.00 |
| Per-channel tone mapping | `hdr_per_channel` | 0/1 | 0 | 0 |
| SDR picture gamma (HDR10 of the SDR image) | `hdr_sdr_gamma` | 1.8-2.8 | 2.20 | 2.20 (not tuned) |
| SDR picture saturation | `hdr_sdr_saturation` | 0-2 | 1.10 | 1.10 (not tuned) |

The tuned column is the next default set (not in the code yet), pending the user's final look.

## Live tuning file

`BB_HDR_TUNE_FILE=<path>` (default `<user dir>/hdr_tune.txt`): `key=value` lines with the keys above, `#`
comments. Checked every 250 ms while HDR10 output is on (modification time and size); the state at the first
check is the baseline, so an old file does not override the launcher at start. A changed value is applied as if
its slider moved, logged (`HDR tune: hdr_saturation=1.20 (file)`) and saved to `bbport.ini`.

## How the defaults were found (2026-11-08)

1. **By eye**, Uncharted 2 Nepal (#89 warzone-highrise), 1000-nit monitor: paper white 300, saturation 1.10,
   per-channel off (it only added an orange tint where the sun shines), contrast 1.00, highlight defaults.
   Uncharted 3 still looked washed out in the sun.
2. **Measured** from NVIDIA HDR screenshots (Alt+F1 `.jxr`, decoded with WPF as scRGB): luminance in nits and
   Oklab chroma per brightness band showed the sunlit diffuse surfaces lifted above paper white. That led to
   Highlights start.
3. **Matched automatically** (`uncharted_autotest/session_tools/hdrmatch2.py` and `hdrmatch2_run.sh`, local tools):
   - Reference run with HDR off at the same spot, Nate standing, no input: the frame dump's SDR output, decoded
     with gamma 2.2 at paper white, averaged per 80x80-pixel cell over three frames.
   - HDR run: the matcher writes the tune file, dumps the HDR output (PQ, Rec. 2020), decodes it to nits and
     compares per cell. Cost: midtone brightness and Oklab colourfulness equal to SDR, highlight colourfulness
     equal to SDR, highlights clearly brighter (target 2.5x SDR, capped by peak/paper), and no more cells above
     paper white than SDR has near-white cells. Coordinate descent over the seven scene settings, two rounds.
   - Uncharted 3 Cartagena street (sunny, young Nate): cost 8.97 -> 2.91. Result: midtone brightness +3.7%,
     midtone colour 0.999, highlight colour 1.001, highlights 1.9x SDR, top 1% about 620 nits.
   - The same values on Uncharted 2 Nepal (a dim scene with no highlights): midtone brightness -5.6% (contrast 1.20
     deepens dark scenes a little), colour +1.6%. One default set fits both.
4. **Uncharted 1** jungle (#1 save, dark, no highlights): with the U3 values the midtones came out 15% darker than
   SDR (contrast 1.20 deepens dark scenes); the matcher raised exposure to 1.10. **Compromise for all three**
   (contrast 1.10, exposure 1.05, the rest as matched in U3), one measurement each: U1 midtones -0.8% / colour
   +2.2%, U2 +4.5% / +3.7%, U3 +5.7% / +0.6% with highlights 1.78x SDR, colour 0.97, top 1% about 605 nits.
   This is the tuned set in the table.
5. **User's look** with the U3-only values (contrast 1.20, exposure 1.00): "not bad, might have to tweak a bit". Final values pending.

## Open

- Final defaults after the user's tweaks; then put them in the code (overlay defaults and Reset).
- Scene HDR on by default for Uncharted 3 and 1 (same tone pass; U3 matched, U1 not looked at yet).
- The SDR-picture path (gamma, saturation) is not tuned; it applies to menus, movies and non-NVIDIA cards.
- Frame generation with HDR: on; DLSS-G cannot be told the frames are HDR (`colorBuffersHDR` is not reachable from
  our DLSS module), so it interpolates the PQ signal as it is. Check by eye.
- AMD and Intel: HDR10 output of the SDR picture only; untested.
- A simple/advanced split of the HDR menu for players (simple: peak, paper white, colour intensity).
- FPS A/B with HDR off (must be level) and the lean check on EU and US before a release.
