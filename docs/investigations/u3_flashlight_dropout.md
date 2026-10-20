# Uncharted 3: flashlight light drops out while the camera turns

Status (20 Oct 2026, 03:45): cause narrowed down, not fixed yet.

## Symptom
At a wall lit only by the flashlight (save `uncharted_saves/uc3/2026-10-20_flashlight-wall`), while the
camera turns (mouse or controller):
1. one frame without the flashlight's light on the wall (its beam in the air is still drawn);
2. one frame with the light back but large blocky dark squares over the wall;
3. normal again.
It repeats every 0.6 to 0.8 s of turning. Also in SuperMedo's first release v0.3 and in 0.3.6.2, so it is
not from this fork's changes, and BB_JOB_SLEEP=0, BB_TIGHT_WRITES=0 and BB_READBACK_SMALL=0 don't change it.

## Evidence (draw log of 120 frames + a 240 fps recording at the same time)
- Every frame runs the same passes: no pass appears or disappears on bad frames.
- The game picks the shadow map size dynamically: frames 0-45 render the shadows at 1024x1024
  (D32Sfloat), frames 46-113 at 1280x1280, frame 114 on at 1024 again.
- The two size switches are 68 frames (0.84 s) apart; the recording's two dropouts in that window are
  0.825 s apart. The dropouts line up with the switches.
- The tiled lighting (compute 12ed77bf, 8d3c1baa, 746e3080, 38d8bf05, 215x90 groups = 16-pixel tiles at
  3440x1440) binds the same buffers every frame; the shadow casters' draw counts also change in steps
  of ~40 draws as objects enter or leave the shadow.

## Working theory
When the shadow map changes size at the same memory, the emulated texture cache sees a different image
at that address. The first frame after the switch the lighting reads an empty (all-shadowed) or wrong
image: the light vanishes. The next frame it reads data laid out for the other size: blocky squares.

## Ruled out (20 Oct, morning)
- Occlusion queries: the emulator reports every query as fully visible, so a late result can't hide a light.
- COND_EXEC: read only one byte of its condition (fixed in 15eab75), but U3 issues no COND_EXEC at the wall.
- Intel red-zone corruption (upstream PR #403): doesn't happen on this Ryzen 9800X3D (the PR's own test).

## 20 Oct, afternoon
- Confirmed: the 1024 and 1280 shadow maps share the guest address 0x114bb88000 (draw log with image
  addresses); switches at frames 1, 55 and 118 of a capture, in step with the dropouts.
- The texture cache keeps two images there (one per size) and hands out the exact one every time
  (BB_IMAGE_MISMATCH_LOG: no size mismatches). Each switch re-uploads the other image from guest
  memory (BB_DEPTH_REFRESH_LOG: ~1 per switch), but skipping those uploads did not change the glitch.
- No pass in the draw log samples the shadow map at that address (it is only a depth target); after
  the shadow draws come target-less full-screen quads (ps 68d3a820), probably a depth resolve/copy
  that the draw log can't see into. Next: a RenderDoc capture of a frame at the wall to find the pass
  that applies the flashlight and what it reads.

## Next steps (earlier plan)
1. Capture again with the new draw log (image addresses now printed, commit 2f5ac63): confirm the
   1024 and 1280 shadow maps share a guest address.
2. In the texture cache, follow what happens when a depth render target is found at an address that
   already holds an image of another size (overlap resolution, image re-creation, initial contents).
3. Fix so the first frame after a size change renders and samples the new shadow map correctly.

Needs the player at the wall for captures, unless a pad recording (BB_PAD_REPLAY) of the walk from
the checkpoint is made once; then captures can run unattended.
