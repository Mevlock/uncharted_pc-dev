# FPS history

Frame rates measured between builds, so a release can be compared with the ones before it.

How the runs are made:
- **Machine:** the maintainer's PC (NVIDIA, 3440x1440). Numbers from different machines aren't comparable.
- **Settings:** native resolution, DLSS anti-aliasing, frame generation off, no frame cap, unless the row says otherwise.
- **Method:** the save's Continue, standing still for 50 s, with `BB_FRAME_STATS=1`. Mean, min and max are over the last 6 one-second samples.
- **Comparing builds:** each build gets its own install with the same saves, and the builds take turns (a warm-up run each, then 2 rounds). Compare rows from the same session and scene; separate sessions can drift by a few FPS.

Add a run with `tools/fps_history.py <user/last_run.log> --build <version> --commit <hash> --scene "<scene>"`.

| Date | Build | Commit | Scene | Settings | Mean FPS | Min–max | Note |
|---|---|---|---|---|---|---|---|
| 2026-10-21 | 0.3.6.9 | | U3 Cartagena square | native, DLSS, FG off, no cap | 89.7 | 87.8–91.9 | 2 rounds |
| 2026-10-21 | 0.3.6.10 | | U3 Cartagena square | native, DLSS, FG off, no cap | 90.2 | 87.7–91.5 | 2 rounds |
| 2026-10-21 | 0.3.6.11 | | U3 Cartagena square | native, DLSS, FG off, no cap | 82.2 | 81.5–83.2 | 2 rounds; regression from #449's copy-back, fixed in 0.3.6.12 |
| 2026-10-21 | 0.3.6.11, `BB_READBACK_SMALL_BUFFERS=0` | | U3 Cartagena square | native, DLSS, FG off, no cap | 88.9 | 88.5–89.2 | confirmed the cause |
| 2026-10-21 | 0.3.6.12 |  | U3 Cartagena square | native, DLSS, FG off, no cap | 88.4 | 83.3–93.3 | 4 rounds, alternating with the row below |
| 2026-10-21 | 0.3.6.13 candidate | 50c6f46 | U3 Cartagena square | native, DLSS, FG off, no cap | 87.2 | 82.3–92.6 | 4 rounds; rounds 87.9/86.0/90.2/89.5 (0.3.6.12) vs 88.3/84.3/88.0/88.1 |
