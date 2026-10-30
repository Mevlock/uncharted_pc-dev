# Performance: how this fork got faster

Frame rate gains over SuperMedo's 0.3.6, the version this fork started from, step by step. Every number below was
measured on **one PC** (RTX 5080, Ryzen 7 9800X3D with 16 threads, Windows 11, EU versions, frame generation off, no
frame rate cap) by running the same saved spot or recorded route with the builds taking turns. **Your numbers will
differ**: the gains are largest where the processor sets the pace (DLSS or FSR upscaling, high frame rates, older
processors) and small where the graphics card is the limit. With a frame rate cap your PC already holds there is
nothing to gain except steadier frame times.

## The short version

| Step | What changed | Where it shows | Gain |
|---|---|---|---|
| 1. October 2026 fixes | The GPU translation stopped waiting on itself | Busy scenes, native 3440 × 1440 | +35% (U1) to about 2.2× (U2, U3) |
| 2. Perf round 1 | Work moved off the translation threads, fewer hand-offs | Processor-limited (DLSS Performance) | about +22% |
| 3. Perf round 2 | Asynchronous submission, faster write tracking, fault read-back | Processor-limited | +6% (U2, U3) to +13% (older processor) |
| 4. 0.3.6.30 | A faster lock in the shader pipeline cache | Uncharted 3's crowds | about +7% average, +14% in the worst moments |

Steps 2-4 multiply on top of step 1. Before 0.3.6.30, steps 2 and 3 were only in separate perf test builds; from
0.3.6.30 everything is in the normal release.

## Step 1: the October 2026 fixes

The port translates the PS4 games' GPU commands into Vulkan. In busy scenes that translation spent most of its time
waiting: character skinning (about 100,000 small GPU jobs a second) made the GPU stop before nearly every job over a
bookkeeping mistake, and other drains and waits added to it. Idle game threads also spun instead of sleeping.

| Native 3440 × 1440 | Before | After |
|---|---|---|
| Uncharted 1, the jungle | ~112 FPS | ~151 FPS (+35%) |
| Uncharted 2, Istanbul museum | ~44 FPS | ~96 FPS (2.2×) |
| Uncharted 3, Cartagena square | ~43 FPS | ~97 FPS (2.3×) |

Details: [PERFORMANCE_2026-10-18.md](PERFORMANCE_2026-10-18.md).

## Steps 2 and 3: the perf rounds

The translation runs as three threads working on the same frame: a command processor, a draw recorder and a Vulkan
recorder. After step 1 the first two were busy for nearly the whole frame at high frame rates.

**Round 1** gave the game's constant data a thread of its own, bound neighbouring buffers together, handed fences on
in batches and cut waits between the threads:

| Same build without → with | Uncharted 1 | Uncharted 2 | Uncharted 3 |
|---|---|---|---|
| DLSS Performance (1712 × 704 → 3440 × 1440) | 159 → 194 FPS (+22%) | 178 → 218 FPS (+23%) | 115 → 140 FPS (+22%) |
| Native 3440 × 1440 | 133 → 138 FPS (+4%) | 125 → 138 FPS (+10%) | 116 → 131 FPS (+13%) |
| Older 4-core processor, simulated | | | 62 → 77 FPS (+24%) |

Uncharted 3 at 1920 × 1080: the slowest 1% of frames took 9.3 ms instead of 12.6 ms.

**Round 2** let the draw recorder go on without waiting for each submission, tracked memory the GPU is about to
write with a fast ordered list, and read game writes back on the thread that wrote them:

| Round 1 → round 2, DLSS Performance | FPS |
|---|---|
| Uncharted 3, Cartagena square | 146 → 156 (+7%) |
| Uncharted 3, older 4-core processor simulated | 77 → 87 (+13%) |
| Uncharted 2, museum | 150 → 159 (+6%) |
| Uncharted 1, jungle | 202 → 206 (about +2%, within the noise there) |

The "older processor" rows run the game on 8 threads of a fast processor slowed to 60% of its clock. Real older
processors also pay more for the operating system's security fixes, so their results can be lower.

## Step 4: 0.3.6.30

Shader preparation (see [SHADER_PREPARATION.md](SHADER_PREPARATION.md)) puts many more shader variants in the cache.
The cache's old lock made the GPU thread and the preparation workers queue behind each other on every shader lookup,
which cost about 4% on average and 10% in the worst moments in Uncharted 3's crowds. A Windows slim reader/writer lock
replaced it:

| Uncharted 3, market route, DLSS Performance | Old lock | New lock |
|---|---|---|
| Average | 173.6 FPS | 185.2 FPS (+7%) |
| Worst 5-second stretch | 100.5 FPS | 114.6 FPS (+14%) |
| Crowded market | 117.0 FPS | 120.8 FPS (+3%) |

No measurable change on the simulated older processor, where fewer threads wait on the lock.

## Where the limit is now

The games wait for the GPU side to finish each frame (the PS4's one-frame-in-flight design), so the translation's
latency, not only its speed, sets the pace in busy scenes. The graphics card still sits idle for part of every frame
at high frame rates. Starting it on a frame earlier is the largest remaining step and is not started.

## For developers

How each change works, every switch to turn it off, the measurement method and what was tried and dropped:
[PERF_BUILD.md](PERF_BUILD.md).
