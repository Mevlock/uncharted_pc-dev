# The perf build: how it works, what it measured, where to pick it up

This document is for developers who want to understand, reuse or continue the CPU-side
performance work in this fork's `perf` branch. Players: see the release notes
(`docs/releases/*-perf*.md`) instead.

The work is GPL like the rest of the port; take whatever is useful.

## 1. The problem it attacks

The PS4 games build GPU command buffers (PM4 packets) with GCN shaders. The port translates them
into Vulkan every frame. Shaders are translated once and cached, so the per-frame cost is the
**command translation**: decoding packets, resolving each draw's resources (buffers, textures,
samplers, descriptor sets, barriers) and recording Vulkan commands. In a busy Uncharted 3 scene
that is ~2,000 draws and ~2,000 compute dispatches per frame. When the CPU is the limit (upscaled
render sizes, high frame rates, slower CPUs), this translation decides the frame rate.

Since 0.3.6.20 the translation runs as a three-stage production line on the same frame:

| Stage | Thread | Does | Main code |
|---|---|---|---|
| A, command processor | the guest GPU thread | decodes PM4, tracks register state, builds draw packets | `video_core/amdgpu/liverpool.cpp` |
| B, draw recorder | `bb:DrawRec` | per draw: binds buffers/textures/samplers, decides barriers, writes descriptors | `video_core/renderer_vulkan/vk_rasterizer.cpp`, `vk_draw_pipe.h` |
| C, Vulkan recorder | the scheduler's record thread | records and submits the Vulkan command buffers | `video_core/renderer_vulkan/vk_scheduler.*` |

Measured before the perf work at high frame rates: stages A and B were each busy for most of the
frame, and they often waited for each other (fence hand-offs, drains).

## 2. What the perf branch changes

Each item has a switch to turn it off, for A/B tests and bisecting.

### 2.1 Constant ring collector (`25d8ec4e`, `BB_RING_COLLECTOR=0` off)
Every draw reads small blocks of guest memory (constants: transforms, light parameters) that are
copied into a GPU-visible ring. Finding room in the ring, checking whether the GPU modified the
source, and the copies themselves used to run on stage A. They now run on a collector thread
(`vk_ring_collector.h`) in packet order while the packet travels through the stages; stage B
waits only for its own packet's copies.
Measured: U3 square ~118.9 -> ~124 FPS when introduced.

### 2.2 Constant ring livelock fix (`ac74e00c`)
After the first full lap of the 32 MiB ring, a stalled region was never published again, so every
allocation failed and fell back to a driver query (millions per second). The fix publishes the
stalled region and throttles the GPU queries (`BB_RING_STALL_FIX=0`, `BB_RING_RETIRE_THROTTLE=0`
restore the old behaviour). This bug is also in the normal release line.

### 2.3 Less work per draw on stage B
- `03a0a604`: neighbouring read-only buffers of a stage are bound as one range, and overlapping
  written views are merged (`BB_COALESCE_STREAMS=0` off).
- `684b9c6c`: buffers that hold only this frame's stream copies skip write-hazard tracking (nothing
  the GPU wrote can be in them; `BB_STREAM_NO_TRACK=0` off); the compute metadata check reads each
  buffer descriptor (V#) once (`BB_META_ONE_PASS=0` off). +1.3%.
- `3704052d`: each V# is read once in BindBuffers; mid-frame flushes stay in order on stage B
  instead of draining the pipeline (`BB_FLUSH_IN_ORDER=0` off).
- `c7cc5edb`: stage B's per-packet/per-draw/per-binding stats timers run only with
  `BB_STAGEB_STATS=1` (they cost ~1%).

### 2.4 Batched fence hand-offs (`863ac9e1`)
The games set a fence after almost every compute job (~1,900 per frame in U3). Each forced stage B
to hand its recorded work to stage C at once. Hand-offs now go in batches (up to 16 packets, or
when stage B runs out of work); a deferred fence's forced kick waits for the queued packets
(`BB_LAZY_FENCE_KICK=0` restores the immediate kick).

### 2.5 che2281337's changes (credited in the commits)
- `bd4760f3`: end-of-shader labels are chained behind the deferred fences, and an already-met
  display-buffer label wait no longer drains the draw queue (`BB_EOS_CHAIN=0` off). Measured with
  markers on vs off (perf build, 1080p): U3 +1%, U1 +1%, U2 the same -> on in all games.
- `d3f50ca2`, `06a6b687`, `d2b1a3b4`: images remade with another format reuse the Vulkan images
  freed that way (image recycling), counted in the VRAM totals and paused while an allocation is
  over budget.
- `48579bc4`, `523aa50a`, `92e285bc`, `d21a085d`: write faults on submitted GPU data read back on
  the faulting thread. **Off by default** (`BB_FAULT_HERE=1` on): it bypasses the ordered fault
  policy and crashed U3's chapter 1 pub (Havok assert from a stale pointer). See section 5.

### 2.6 Perf build identity (`738550c3`, `f7ccaacf`)
Window title, settings menu header and a line under the FPS counter say PERF BUILD; it keeps its
own pipeline cache (`<serial>-perf`), because sharing one with the normal build made each wipe the
other's at start.

## 3. Measurements

All on one PC: RTX 5080, 16-thread CPU, Windows 11, EU versions unless noted, DLSS, frame generation
off, uncapped, the same saved spot per game, 2 minutes per run, the two builds alternating over two
rounds (rounds agreed within ~2%, U1 native within 7%). "Normal" is the same source without the perf
work.

| Setting | U3 | U1 | U2 |
|---|---|---|---|
| DLSS Performance (1712x704 -> 3440x1440) | 114.7 -> 140.1 (+22%) | 159.4 -> 194.3 (+22%) | 178.1 -> 218.3 (+23%) |
| Native 3440x1440 | 115.9 -> 130.8 (+13%) | 132.8 -> 138.4 (+4%) | 124.6 -> 137.5 (+10%) |

Older-CPU simulation (8 threads, CPU max state 60%, job workers sleeping), U3:
square at DLSS Performance 62.3 -> 77.2 (+24%); crowd-heavy rooftop chase 81.4 -> 89.5 (+10%).
8 threads at full clock: 117.8 -> 143.1 (+21%).

Frame times, U3 square at 1920x1080 uncapped: slowest 1% 12.6 ms -> 9.3 ms; frames over 1.5x the
median in 2 minutes: 12 -> 0.

### How the runs were made
- Fixed save slots per game (U1 jungle, U2 an early chapter, U3 Cartagena street), loaded with
  Continue; 120 s per run; FPS = mean of the last 12 `Frame stats:` windows (`BB_FRAME_STATS=1`).
- Builds alternate (A, B, then B, A) so warm-up and heat favour neither.
- Heavy scenes: a recorded controller route replayed from a checkpoint (`BB_PAD_RECORD=<file>`,
  `BB_PAD_REPLAY=<file>`), so the camera and crowd do the same each run.
- Older CPUs: the game started with an affinity mask (8 logical CPUs) and the Windows power plan's
  maximum processor state lowered.

## 4. Where the limit is now

A causal-profiling run (artificially slowing stage B per draw: `BB_SLOW_B_NS`) on U3's heavy route
showed +300 ns/draw costing 3.6% and +600 ns 10.5%, with stage B busy only ~73% of the time at
normal speed. So a faster stage B gains 1-3% at most in U3 today. The limit has moved to the guest's
own threads and its frame waits: the games wait for the GPU to go idle each frame
(`sceGnmSubmitDone` and related calls), the PS4's one-frame-in-flight model.

## 5. What is next (not done)

1. **Let the GPU start on a frame earlier.** The biggest remaining win: today the GPU idles part of
   every frame at high frame rates waiting for the CPU side. Needs care with the games' idle waits
   (removing them only moves the wait to the next call, measured 5-7 ms/frame).
2. **Fault read-back on the faulting thread, made safe.** drakolordx7's `792620c7` (in his community
   branch) writes each page back in the same locked step that settles it, only while it is still
   GPU data, and re-checks neighbour pages for CPU writes. That matches how our crash happened. Port
   it onto this branch's side-queue slots, turn `BB_FAULT_HERE` on (~+2.5% in our A/B), and play-test
   U3's chapter 1 pub.
3. **NVIDIA Reflex** for DLSS frame generation (`VK_NV_low_latency2` markers around the game's frame;
   measure with PresentMon).

## 6. Tried, and why it was dropped

| Idea | Result |
|---|---|
| Parallel recording per render pass / bindless | Ceiling 1-3% for U3 now (section 4) |
| A texture-binding helper thread | Flicker and -1.9% |
| Binding memos | No gain |
| Asynchronous side read-backs | +14 FPS average but frame spikes (the GPU runs while the guest stalls) |
| Prefetched read-backs (`BB_PREFETCH_READBACK=1`) | No gain on 16 threads |
| Guest thread priorities on host threads | U3 tie, U2 -4%, 4-core -4% |
| Texture eviction age tuning on small cards | No difference |
| Native batching (6.1) | +2% in calm scenes, -3.6% in the heaviest; left in the tree, off |
| Object motion vectors off (`BB_OBJECT_MOTION=0`) | +0.4%: they cost practically nothing in U3 |
| Chained WriteData on the draw recorder (spacepepperoni's port) | No gain here, and a load freeze in a slow-CPU test; not merged |

### 6.1 Native batching (experiment, in the tree, off)
U3 runs thousands of small compute dispatches per frame for character skinning, one per object
(`cs 8f067540`). Native batching turns a run of them into one batched dispatch: each object gets a
workgroup range, its read-only inputs are taken from the constant ring (`BB_NB_RING`, on with the
experiment), outputs are allocated per object in one arena, and a hazard check by address ranges
decides where a batch must end. A later prototype does the same for draws (`BB_DRAW_BATCH=1`, needs
`BB_INLINE_FETCH=1`: the fetch shader translated to read vertex buffers through buffers, a DrawIndex
table and indexed indirect multi-draws).

Switches: `BB_NATIVE_BATCH=1` (compute batching), `BB_DRAW_BATCH=1` + `BB_INLINE_FETCH=1` (draws),
`BB_DRAW_BATCH_DIAG=1`. It works (about 34 objects per batch, a play-test without errors), but the
measurements say it is not worth turning on:

| U3, DLSS Performance unless noted | Off | On |
|---|---|---|
| Cartagena street (2 rounds) | 140.2 | 143.0 (+2.0%) |
| Rooftop chase, ~2,200 draws/frame | 174.3 | 167.9 (-3.6%) |
| 8 threads at 60% CPU | 75.3 | 77.6 (+3%) |
| Native 3440x1440 (earlier) | | +1.3% |

The bookkeeping (hazard checks, building the batch tables) costs about as much as it saves, and in
crowded scenes more; section 4 explains why a faster draw recorder has little left to gain in U3.
The idea came from axylpup's shadPS4 work on inFAMOUS Second Son, where batching pays off for that
engine.

## 7. Switches

Experiments and diagnostics in this branch, all off unless stated:

| Switch | What it does |
|---|---|
| `BB_RING_COLLECTOR=0` | constant ring work back on stage A |
| `BB_COALESCE_STREAMS=0`, `BB_STREAM_NO_TRACK=0`, `BB_META_ONE_PASS=0`, `BB_FLUSH_IN_ORDER=0` | per-draw savings off |
| `BB_LAZY_FENCE_KICK=0` | fence hand-offs one by one again |
| `BB_EOS_CHAIN=0` | che's label changes off |
| `BB_FAULT_HERE=1` | fault read-back on the faulting thread (see 5.2) |
| `BB_PREFETCH_READBACK=1` | prefetched read-backs |
| `BB_ASYNC_SUBMIT=1/2`, `BB_SUBMIT_MARKERS=1`, `BB_SUBMIT_NO_IDLE_WAIT=1/2`, `BB_VO_NODRAIN=1`, `BB_WAITREGMEM_NODRAIN=1`, `BB_COMPUTE_WRM_PENDING=1` | experiments on submission and waits (measurement only) |
| `BB_STAGEB_STATS=1`, `BB_MATERIAL_STATS=1`, `BB_DISPATCH_STATS=1` | stage B and material statistics in the log |
| `BB_FRAME_TRACE=<file>` | per-frame timeline of the stages |
| `BB_SLOW_A_NS=<ns>`, `BB_SLOW_B_NS=<ns>` | causal profiling: slow a stage per packet/draw |
| `BB_SAMPLE_THREADS=1` | host thread sampler (where each thread spends its time) |
| `BB_LIST_PS=1` | pixel and compute shaders used every 10 s (for `BB_SKIP_SHADERS` bisects) |
| `BB_FRAME_STATS=1` | FPS, worst frame, frame pacing and frames over 25/33/50 ms in the log |
