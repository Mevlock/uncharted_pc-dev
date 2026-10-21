# U1 boat deck: black fan flickering on a hatch

Status (2026-10-21): **fixed** (see "Resolution" at the end). It was z-fighting between a soot
decal and the deck hatch, caused by our anti-aliasing jitter missing the decal pass.

## The symptom

Uncharted 1, opening boat deck. For single frames, a black, soft-edged burst with spokes appears
on the circular deck hatch (bottom right from the checkpoint). It shows in about 7–13% of frames,
standing still, with nothing shooting. It was already in SuperMedo's 0.3.0. The user checked PS4
footage: **it isn't there on PS4**, so it's a port bug.

Reproduce: save snapshot `uncharted_saves/snapshots/2026-10-21_u1-boat-flicker`, Continue, stand
still. The scratchpad scripts `flk_seq.txt` and `flkcount.py` take 60 screenshots and count frames
with the fan (fewer than 25 luma in the hatch box, more than 500 pixels).

## What draws it (RenderDoc)

Captured with `user\RenderDoc U1.bat`. Frame 7333 has the fan, 7332 is clean; both are in `user\rdc`.

- Pixel history at 2968,1246: the lit-scene target goes from floor colour (0.19) to black (0.0007)
  at one draw, `vkCmdDrawIndexed` with 24 indices. That's a 3×3 vertex patch lying flat on the hatch.
- Game shaders: **pixel `b2a42acf`**, vertex `0239b258` (matched by md5 against the shader cache).
- Its main texture (512×512 BC1) is a radial burst, an impact or soot mark. The scene has 10 such
  decals.
- The draw is alpha-blended, with depth test LessEqual and depth write on. The game sets no
  polygon offset for it (`NeedsBias()` false), and Vulkan uses none.
- Its colour is dark by design: albedo times a sum of lights plus fog. The black comes out at full
  opacity because the fade term is constant (P[62..65] = 0 0 0 1, so fade = 1 every frame).

## It's z-fighting

- In the clean frame, the same decal draw **fails the depth test** at that pixel (`depthfail`). In
  the fan frame it passes.
- Diagnostic `BB_DECAL_BIAS=b2a42acf:<constant>` (stash "u1 hatch diag 2") forces a depth bias on
  that pixel shader's draws:
  - −200000 (toward the camera): **fan in 60 of 60 frames**, a permanent black splat.
  - +200000 (away from the camera): **fan in 0 of 60 frames**, the flicker is gone.
  - ±16: no effect (far too small for a 32-bit float depth buffer).
- So the decal and the hatch are coplanar. On PS4 the decal consistently loses (it never shows); in
  the port it wins by rounding in about 1 frame in 12.

## Ruled out along the way

- **Compute shaders:** none of the 7 screen-space compute shaders matter (skipped one at a time).
- **NaN handling:** NaN-safe min/max/clamp (NMin/NMax, like GCN) changed nothing.
- **Denormals:** flushing denormals in the decal's constant reads (GCN flushes; NVIDIA can't be told
  to) changed nothing.
- **Torn constants:** the decal's light/cascade window (`0x100c6220f0+0x400`, double-buffered with
  `0x10150026e0`) and its header blocks are identical across every read within a frame, both workers
  and GPU thread. The GPU never writes them. The "shifted record" seen once was the window changing
  between frames, which is normal.
- **Shadow maps:** the 4-cascade sun shadow array (2048², D16) is practically identical in the fan
  and clean frames.
- **Recording timing:** `BB_DRAW_PIPE=0` makes it rarer (2 of 60) but not gone; that's consistent with
  rounding that shifts with timing.
- **Multiply-add:** our translation keeps GCN's `v_mad_f32` unfused (mul + add, marked precise), so
  that isn't it.

## Options for a fix (need a decision)

1. **Targeted bias (easy, a hack):** give pixel shader `b2a42acf` a positive depth bias so it always
   loses, which matches what the PS4 shows here. First find the smallest bias that removes the
   flicker (between 16 and 200000), and check other places this decal shader is used (bullet or
   explosion marks elsewhere might be meant to show).
2. **Find why the PS4 is stable (proper fix, harder):** compare how the decal's vertex shader and
   the floor's vertex shader compute depth on GCN against our SPIR-V (rounding of the transform,
   viewport/depth conversion, `emulate_depth_negative_one_to_one`, depth clamp). The decal's
   vertices are GPU-written (its guest vertex buffers read as zeros), so its compute shader is part
   of the path too.
3. **Check whether the decal should be visible at all:** if its fade inputs (P[62..65]) are meant to
   come from GPU-written data that we never read back, on PS4 they'd hide it. Our values are a
   default-looking `0 0 0 1`.

Diagnostic code is in git stashes "u1 hatch diag" and "u1 hatch diag 2". The tools are scratchpad
scripts (rdtrigger, rdthumb, rdfan pixel history, rdevent, rdshadow, rdbufs, rddebug).

## Resolution

Only with the port's anti-aliasing (DLSS, FSR, TAA, XeSS): with Anti-aliasing **Off** the hatch
is clean (0 of 60 frames, twice). Frame rate (30/60 caps, 60 Hz vblank) and render resolution
(1080) made no difference.

Everything the game feeds the two draws is static and identical: the hatch floor (vs 5211da72)
and the decal (vs 0239b258) share the same object matrix (`3.6938 0.617 4.5923`) and the same
camera matrices every frame, and the decal's vertices (cs 8f067540, identity palette, constant
inputs) never change. The decal's PS4 depth state is what we set (Z32 float, LessEqual, no
polygon offset).

The difference was the jitter (`BB_JITTER_LOG`, diag stash "u1 hatch diag 3"): the floor draws got
the upscaler's sub-pixel shift (e.g. -0.375, -0.056 px), the decal draws none. U1 copies the
scene depth to a second buffer of the same size (0x11448f0000 next to the scene depth at
0x113e608000) and draws the decals against the copy. `SceneJitterFor` only recognised the scene
depth image itself (and half-size targets), so the decals were drawn unjittered against jittered
depth, up to ~0.4 px off: the depth gap swung by hundreds of ulps from frame to frame and the
decal won the test in some frames.

Fix: depth buffers of the scene's size get the full jitter too. Hatch count with DLSS: 0 of 60
(twice; 5 to 13 of 60 before), FPS unchanged.
