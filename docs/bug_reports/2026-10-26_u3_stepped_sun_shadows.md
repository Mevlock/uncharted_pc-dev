# U3: stepped / combed sun shadow edges (user, 2026-10-26)

**Status:** investigated, **not a port bug** (the port draws the game's shadows as the game asks).
Smoother shadows would be a PC enhancement (see Options); nothing changed.

**What:** U3, chapter 2 market street (square save), sun shadows on the ground and walls have stepped
edges with a fine comb/dither along them, most visible at DLSS Performance and with AA off.

## Tests (EU CUSA02343, square save spawn, full-size presenter frames, RTX 5080, 3440x1440)
| Setting | Edges |
|---|---|
| Native 3440x1440, DLSS | stepped |
| Game's own 1920x1080 render (scaled up) | stepped, same step size in the world |
| DLSS Performance (1712x704) | stepped |
| Anti-aliasing off | stepped, comb clearest |
| 16:9 window 2560x1440 (ultrawide patch off) | stepped |
| Release 0.3.6.26 | stepped |
| BB_PCF_LINEAR=1 (diagnostic: compare samplers forced bilinear) | no change |

Shots kept locally (native, r1080, perfmode, aaoff, w169, rel, pcflog, pcflin).

## What the shadow reads are (a temporary diagnostic, not kept)
- Compare samplers the game makes: mag/min **bilinear**, compare Less, clamp. The port maps them as
  is (hardware 2x2 PCF on).
- Images read with them: sun cascades **2048x2048 x4 layers D16Unorm**, a 4096x4096 D32Sfloat map,
  the scene depth (D32S8, read through a D32 view). All are depth images with depth views.

## Conclusion
Ruled out: render size / upscaling (same steps at 1080), the ultrawide camera widening (same at 16:9),
DLSS (same with AA off), a recent regression (same in 0.3.6.26), lost PCF filtering (game asks for
bilinear and gets it; forcing it changes nothing), colour/depth view mismatch (all depth).
Left: the game's own shadow resolution. Four 2048 cascades near the camera cover several screen
pixels per shadow texel at 1440p; the PS4 shows the same map over fewer pixels at 1080p. The comb is
the game's soft-edge dither. To confirm against the PS4: footage of this street (ch2, Cartagena).

## Options (enhancements, not started; user to decide)
- A: larger sun cascades (2048 -> 4096): patch the game's shadow allocation, viewports and the
  shadow-space constants; costs ~4x shadow memory (D16: 32 -> 128 MiB) and some GPU time.
- B: more PCF taps / a wider filter in the shadow lookup (shader patch on the lighting pass): softer
  edges, cheap, but changes the look.
