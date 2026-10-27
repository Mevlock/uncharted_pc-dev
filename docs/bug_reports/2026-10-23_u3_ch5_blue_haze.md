# U3 chapter 5: blue haze over the screen after ~30 minutes (player report, 2026-10-23)

**Build:** 0.3.6.22-pre. **GPU:** GTX 1660 Super (6 GB). 1920x1080, TAA, ~55 FPS (1% low 7).

**What:** after about 30 minutes of continuous play, reaching chapter 5, a soft blue glow covered
the top and right of the screen (stone vault with a chain, Sully kneeling), with a pale blob on
the wall left of centre. Turning Bloom off did not change it. Restarting the game fixed it.

**Not yet known:** whether it comes back at the same spot after a fresh start (if it does, it is
the scene, not the session), and memory use at the time (no log).

**Suspects:**
- An area or chapter change leaving a stale texture where the fog or lighting expects the new
  area's (a restart reloads everything). 30 minutes is short for the memory growth below alone.
- Video memory running short on a 6 GB card over a long session (buffer arenas never give video
  memory back), so the texture cache evicts and reuses images, and a stale image lands on a fog or
  lighting texture. The texture collector fixes in 0.3.6.23 (community picks) may help.
- A fog or light-shaft pass reading a wrong input in this scene (soft, low-resolution look).

**Asked for:** `user\last_run.log` before restarting, with `BB_FRAME_STATS=1` in Environment;
the save near the spot.

## Follow-up log (same player, likely): GPU crash after a long session
0.3.6.22, U3 CUSA02344, GTX card (DLSS unavailable), 12 threads, 1080p TAA. Played from London through
the chateau (forest, burning manor, spider chase). The texture cache stayed far over its budget
("memory pressure, 3400-3660 of 1893 MiB (critical 3786)") with long runs of "0 images evicted", then
"Device lost" / "GPU crash: write to an invalid address at 0x680c6000". Video memory exhaustion over a
long session; the blue haze was likely an earlier symptom. 0.3.6.23's texture collector fix (images it
cannot evict no longer block it, 20 s idle images freed) targets this; the buffer arenas that never
shrink (VRAM growth TODO) remain.
