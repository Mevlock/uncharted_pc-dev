# U1: blue-grey speckled rectangle on the pistol's slide (user, 2026-10-26)

**Status:** open. Probably the same bug as the "blue lines on one wall" in U3 chapter 6
(2026-10-20_fifo_dof_blue_lines_shimmer.md, item 3): blue marks around/on some textures.

**What:** aiming the pistol in Uncharted 1 (jungle temple, early game), a pale blue-grey speckled
rectangle shows on the side of the slide, where the metal should be. The rest of the gun (grip,
hammer, trigger) looks right. Screenshots: `Videos\NVIDIA\out\out Screenshot 2026.10.26 - 13.04.*`.

**Tests (US CUSA02320, user's PC, RTX 5080):**
| Build / settings | Rectangle |
|---|---|
| test/antik-perf, DLSS Performance (1712x704) + DLSS FG 2x, game rate 60 | yes |
| same, BB_KEY_ICONS=0 (antikmewzik's texture repaint off; log confirms) | yes |
| release 0.3.6.26, same settings | yes |
| release 0.3.6.26, anti-aliasing off, render 1920x1080, FG off | faint, still there |

**So:** not the perf/VRAM/antik changes, not DLSS itself (it only sharpens it): in the base port.
Looks like part of a texture decoded or uploaded wrongly (a compressed block, a channel swizzle,
or a wrong mip), so only some surfaces show it.

**Next:** RenderDoc capture of the aiming frame (F12; retry after device lost): find the slide's
texture, its format and contents, compare with the raw guest data. Also check the EU version and
PS4 footage of the pistol.

## RenderDoc, 2026-10-26 13:14 (release 0.3.6.26, US, AA off, 1080p; user\rdc\u2_frame2638.rdc)
- The gun is drawn at event 1314 (2976 indices; depth pre-pass at 925) into the 1920x1080 RGBA16F
  normal G-buffer (target 7246). Its pixel shader reads one texture: 15052, 512x512 BC5_UNORM, 10 mips
  (the gun's normal map). Mip 0 and mip 2 saved: both look correct.
- The normal buffer over the gun (PickPixel grid, rdgunpick.py): clean, no rectangle.
- The single-frame capture holds only geometry passes (113 draws, all into 7246) and the final blit
  to the 3440x1440 image: the lighting/material passes fall outside this frame's capture window
  (presents and the port's GPU threads). So the rectangle comes from the lighting/material stage.
- Next: a 2-3 frame capture (BB_RDOC_CUT=N, F11) so the material passes are in the same file;
  find the gun's colour/specular texture there and check it.

## Evening 2026-10-26: user describes it as a blue/green glow / corrupt reflection on the gun, worse in motion
Ruled out (user checking each launch, U1 US jungle save, DLSS Performance 3440x1440):
- Our draw threads: BB_DRAW_PIPE=0 BB_VK_RECORD_THREAD=0 (log: no draw pipe) -> still there.
- This fork's changes: not the cause (it is in the original game, see the resolution below). US game files: the EU version (CUSA02343) shows it too. Shader cache: fresh installs.
- Driver: 617.14 installed 2026-09-29 (unchanged during the project). NVIDIA filters off; Auto HDR does
  not apply to Vulkan. NVCP negative LOD bias Clamp -> same; the port's own scene LOD bias off
  (BB_TOGGLE_FILE bit 57, log confirmed) -> same. Bloom was already off in the user's settings.
- Pixel shader bisect (BB_LIST_PS=1, BB_SKIP_SHADERS, 117 shaders drawn): all ruled out except
  6ea82d46 (empty depth-only pass: skipping it removes the gun) and 3371ae8d (final picture: black).
  Blur/DOF passes (5e0b720c 784b6f74 701c3d08 f4b18264) off -> still there.
- Compute shaders (23 listed): every half/third tried blacks or greys the picture (chained lighting),
  so skipping cannot isolate it.
Left: the compute lighting/reflection inputs (RenderDoc capture that includes compute, multi-frame),
or the game's own look (check PS4 footage of the U1 pistol). A Discord report of "blue lines in the
chateau" (U3 ch8) may be related; not found yet.
Diagnostics added on vram-main (uncommitted): BB_LIST_PS=1 lists pixel + compute shaders every 10 s.

## Same bug in U3 chapter 8 (chateau), 2026-10-26 21:37 (save uncharted_saves/uc3/2026-10-26_chateau-blue-lines)
Bright blue dots, evenly spaced (~16-18 render pixels at 1712x704) along the depth edges of one wall
panel. One wrong pixel per ~16 px along an edge suggests a tiled compute pass (16x16 or 8x8 groups)
where one pixel per tile goes wrong at a depth discontinuity.
U3 compute shaders seen there (BB_LIST_PS=1, chateau_list.log; dumps in user/shader/dumps):
- 12ed77bf off: ambient/indirect lighting gone (scene dark except sun), DOTS STILL THERE -> not it.
- 8b10b4cb + 1c3672c5 off: picture normal, dots still there -> not them.
- 38d8bf05 47e0edef f8fa9165 8d3c1baa 746e3080 c936dd6 off: launched, not checked yet.
Shared by U1 and U3 (Bluepoint's common remaster code; the bug is in both games, so likely here):
19367542 1e8e11d0 2a2e99be 2d215c3b 308607aa 43225bd9 4f81a8f 67921bad 6a6422a9 6d71bb 7095d6c2
7d7e7206 8f067540 a57ac2f aec539f0 c92150ba e8746089. Next singles to skip: 6d71bb (5 image reads,
abs/select: edge-aware filter?), 19367542, 43225bd9, 2a2e99be, a57ac2f; then a U3 pixel-shader bisect.
Method: BB_LIST_PS=1 for the shader list, BB_SKIP_SHADERS=<hashes> to switch them off.

## Resolution 2026-10-26 23:20
- U3 chateau blue dots: a PORT bug, fixed on vram-main: draws of up to 6 indices were never jittered
  (meant for full-screen quads), but single-quad wall panels are scene geometry; with an upscaler's
  jitter they sat a fraction of a pixel off their jittered neighbours and the seams opened into
  pinholes showing the scene colour's clear (0, 0, 1, 0). Now small draws that write depth or test it
  for equality (not ALWAYS) get the jitter (BB_JITTER_SMALL=0: old behaviour). Found by checking PS4-like
  settings (gone at native 1080 without AA, back with DLSS at 1080) + RenderDoc pixel history.
- U1 pistol cyan sheen: NOT a port bug. The user sees the same in RPCS3 running the original PS3
  game. It is the game's own material lighting (gun G-buffer shader 4ff5cb04 packs material bits into
  the normal buffer's alpha; material shader cd9fb6a2 lights it cyan), stronger when panning right.
  Ruled out on the way: NaN guard, GCN-accurate float->int conversion and saturate (BB_GCN_CVT test),
  jitter, LOD bias, threads, cache, game files, driver.
- Side finding (separate small bug): V_RCP_LEGACY_F32 reads the result's sign with ConvertFToU (a
  numeric conversion) instead of a bit cast, so 1/0 gives the wrong-signed zero. Not used here.
