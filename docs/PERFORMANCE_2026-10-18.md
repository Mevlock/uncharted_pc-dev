# Performance work 2026-10-18 (local, not published)

Goal: higher frame rates in Uncharted 3 **without frame generation**, with no visual glitches.

Test bench: Ryzen 7 9800X3D (8 cores / 16 threads), NVIDIA GPU, 3440×1440 output, DLSS "balanced"
(render 2016×832), frame generation off, frame rate cap 144. Benchmark scene: the Cartagena square
right after "Continue" (scripted, no input); glitch test: the pub fight in London (played and
recorded by hand).

## Result

All three games, native 3440×1440, frame generation off, no frame rate cap, 50 s in each scene
after "Continue" (measured 2026-10-19): every off switch set
(`BB_TIGHT_WRITES=0 BB_FLUSH_EVERY_DRAWS=0 BB_COMPUTE_RELEASE_DRAIN=1 BB_COMPUTE_DRAIN_ALL=1
BB_PREDICATION_DRAIN=1 BB_GPU_THREAD_PRIORITY=0`) against the defaults.

| Game (scene) | All changes off | All changes on | Gain |
|---|---|---|---|
| Uncharted 1 (the jungle with Sully) | ~112 FPS | ~151 FPS | +35% |
| Uncharted 2 (Chapter 2, the Istanbul museum balcony) | ~44 FPS | ~96 FPS | +118% |
| Uncharted 3 (Chapter 2, the Cartagena square) | ~43 FPS | ~97 FPS | +126% |

U3 at native (~97) runs about as fast as at "balanced" 2016×832 (~100): the GPU command thread on
the CPU sets the frame rate there, not the GPU.

The measurements during the day, U3 at "balanced":

| Uncharted 3, frame generation off | Start of day | Now |
|---|---|---|
| Cartagena square | ~42 FPS | **~100 FPS** |
| Pub fight | — | **110–144 FPS** (reaches the cap) |
| GPU busy in the square | 72% | 21% |
| Synchronous side read-back, average wait | ~950 µs | ~150 µs |

Glitch check of the final build: full pub fight recording (10 575 distinct frames) scanned with
`tools/glitch_scan`, every isolated candidate and the largest clustered ones reviewed by eye —
no spikes, no exploded frames.

Uncharted 2 (Chapter 2 "Breaking and Entering", the Istanbul museum balcony after "Continue"),
native 3440×1440 (b20ace7), frame generation off, no frame rate cap:

| Uncharted 2 | FPS (last 30 s of 50) |
|---|---|
| `BB_TIGHT_WRITES=0 BB_FLUSH_EVERY_DRAWS=0` | ~49 |
| Defaults | **~93** (+90%) |

Uncharted 1 (the jungle with Sully after "Continue"), native 3440×1440 (82f9373), same setup:

| Uncharted 1 | FPS (last 30 s of 50) |
|---|---|
| `BB_TIGHT_WRITES=0 BB_FLUSH_EVERY_DRAWS=0` | ~115 |
| Defaults | **~147** (+28%) |

The gain depends on the scene: U1's jungle has only two characters to skin. Measured there the same
way (FPS with the diagnostics on):

| U1 jungle | `BB_TIGHT_WRITES=0` | `BB_FLUSH_EVERY_DRAWS=0` | Defaults |
|---|---|---|---|
| Skinning dispatches/s (with a barrier first) | ~6 300 (80%) | ~4 800 (1.5%) | ~6 300 (1.4%) |
| GPU time on the compute queues | ~42 ms/s | ~4 ms/s | ~5 ms/s |
| GPU busy | 56% | 38% | 53% |
| Submit-to-done | 0.9 ms | 8 ms | 0.9 ms |
| FPS | ~151 | ~117 | ~152 |

U1's gain here is almost all early submits: the false barriers are fixed too, but with ~6 300
skinning dispatches/s (U2's museum ~89 000) they cost little. At ~152 FPS the GPU is half idle and
under 1 ms behind: the GPU command thread is the limit.

Why U2 gains the most, measured in the museum spot with `BB_QUEUE_SHADERS=1 BB_GPU_BUSY=1`
(FPS with the diagnostics on): U2 runs the same skinning shader (`cs 8f067540`, ~89k
single-workgroup dispatches/s), and it needs both changes.

| U2 museum | `BB_TIGHT_WRITES=0` | `BB_FLUSH_EVERY_DRAWS=0` | Defaults |
|---|---|---|---|
| Skinning dispatches with a barrier first | 97% | 0.1% | 0.1% |
| GPU time on the compute queues | ~490 ms/s | ~8 ms/s | ~10 ms/s |
| GPU busy | 83% | 31% | 43% |
| Submit-to-done | 11 ms | 15 ms | 3 ms |
| FPS | ~74 | ~69 | ~91 |

Without early submits the GPU idles most of the time yet runs 15 ms behind (work arrives in large,
late batches), so the CPU's waits for GPU data grow; without tight writes it is busy with skinning
barriers. U2's compute queues are guest queues 1–6 (U3's are 2–7).

These two tables toggle only the two big changes. A first run with every off switch set ended
right after boot, but it did not repeat: each switch alone works in U3 and all of them together
work in U2 (the "Result" table above). That run was most likely closed during U2's slow boot.

## The changes, in the order they landed

| Commit | Change | Square FPS after |
|---|---|---|
| c9d4cd0 | Compute queues: RELEASE_MEM handled in order with the draws instead of draining the draw pipe; the compute queue drains only before packets that use the caches | 42 → 54 |
| 8880a36 | Recorded GPU work is submitted every 250 draws (`BB_FLUSH_EVERY_DRAWS`, 0 = off), not only at the guest's submit | 54 → 72 |
| 9bfc964 | **Tight write ranges for typed buffer writes** + barrier tracker cap (below) | 72 → ~100 |

Smaller ones from the same day, kept because they are harmless or useful:

- 65b5545 — from the God of War III port: no drain for ignored SET_PREDICATION
  (`BB_PREDICATION_DRAIN=1` restores it), `BB_STREAM_MEMORY=device|cached|host`, `BB_PERF_DIAG`.
- 7695006 — GPU command, draw recording and Vulkan recording threads above normal priority
  (`BB_GPU_THREAD_PRIORITY=0` turns it off). No measurable gain in U3.

### 9bfc964: tight write ranges (the big one)

GPU timestamps per guest queue (`BB_GPU_BUSY=1`) showed the GPU busy 72% of the time, ~520 ms of
every second on the game's compute queues 2–7, and only ~170 ms/s on the graphics queue. Nearly
all of it was one shader, U3's character skinning (`cs 8f067540`): ~108 000 dispatches a second,
each **a single workgroup**, and 86% of them preceded by a pipeline barrier that drained the GPU.

The barriers were false. Skinning writes interleaved vertices (28-byte stride) through two typed
views: buffer 6 (3 floats at offset 0) and buffer 7 (2 floats at offset 12). The barrier tracker
used each view's nominal range, `stride × records`, so buffer 7's range reached 12 bytes past the
object's end, into the next object's first vertex, and every following dispatch "overlapped" it.
The bytes actually written stop 8 bytes before the object ends.

- **Fix:** for typed (formatted) writes through a strided view, the tracked range is
  `stride × (records − 1) + element size` (element size from the buffer's data format), which
  covers every byte a typed store can write. Raw and unstrided writes keep their full range.
  `BB_TIGHT_WRITES=0` turns it off.
- **Barrier tracker cap:** the tracker's ranges are cleared only by a barrier. With barriers now
  rare, its tree grew without bound and every buffer binding's lookup slowed the recording thread
  (92 FPS sinking to 61 within half a minute). Past `BB_TRACKER_CAP` ranges (default 4096) one
  ordinary barrier is recorded and the tracker cleared.
- **Risk:** if a shader ever writes more through a typed view than its format describes, a real
  barrier would be skipped (it would show as flickering or spiking geometry). Test with
  `BB_TIGHT_WRITES=0` to confirm or rule it out.

With the GPU no longer a frame behind, the synchronous side read-backs (see below) wait ~150 µs
instead of ~950 µs, which is where most of the frame rate came from.

## Tried and rejected

All of these are measured; none is enabled by default. The async diagnostics stay in the code
behind their switches.

### Asynchronous side read-backs (`BB_ASYNC_SIDE_DOWNLOAD=1`, off)

When a guest thread writes a page holding GPU data, that data is read back first ("side
read-back", on a second Vulkan queue). Synchronously, the GPU command thread waits for each one;
in the square that was ~40% of its time. The async variant lets the guest thread wait for its own
read-back while the GPU thread carries on: +14 FPS at the time (72 → 86), but one-frame geometry
spikes (pool table, characters) and whole exploded frames at the pub's "escape a grab" prompt.

Root cause, found after many experiments: **not stale data** (`BB_ASYNC_VERIFY=1` compares every
read-back with a fresh copy — always identical). The game hands over GPU work that reads what the
stalled guest thread writes next; with synchronous read-backs the GPU thread was frozen during the
stall too, which hid this. Mitigations tried:

| Switch | What | Result |
|---|---|---|
| `BB_ASYNC_STRICT=1` | redo a read-back synchronously if a draw touched its pages in flight | fewer spikes, not none |
| `BB_ASYNC_PAUSE=1` | no draw or dispatch while any guest thread waits | clean, but no gain (73 FPS) |
| `BB_ASYNC_PAUSE=2` | only work overlapping the read-back window waits | 87 FPS, rare spikes remain (the thread then writes other buffers) |
| `BB_SIDE_WINDOW_KB=4` | read back the faulting page only | much worse with async; fine without |
| `BB_READBACKS=2` | precise mode (read protection too) | hangs at boot |

Now that read-backs take ~150 µs, there is far less left to gain here.

### Making synchronous read-backs cheaper

`BB_PERF_DIAG=1` prints a breakdown. Before 9bfc964: ~78% of read-backs found the writing GPU work
done (~50 µs); the rest waited ~4 ms for the GPU, nearly all on two regions:

- `0x1036160000`: GDS counters copied to memory by DMA on the graphics queue, right after records
  the game writes on the CPU — and the game reads those counters right afterwards.
- `0x11101d0000`: a ring where GPU-written and guest-written blocks share pages.

Rejected: eager background read-backs of learned pages (no gain: the guest writes before the GPU
has run the work), and byte-level sharing of the counter page (the game crashed at gameplay start —
it needs the fresh counters). Both patches were kept out of the tree.

### Others

- Pipe spin length (`BB_PIPE_SPIN_US` 200/20/0): no difference.
- Pinning the GPU threads to cores: abandoned (not worth it).
- LTO, stream memory "cached", occlusion queries, image churn: no gain (earlier in the day).
- A separate Vulkan compute queue for the game's compute queues: not needed — the compute queues'
  GPU time was the false barriers above, and the counter waits come from the graphics queue.

## Diagnostics added (all off unless set)

| Variable | Prints |
|---|---|
| `BB_FRAME_STATS=1` | FPS, write faults and side read-backs, draw pipe drains by reason (existing) |
| `BB_PERF_DIAG=1` | side read-backs per region, image creates, **synchronous read-back time breakdown** |
| `BB_GPU_BUSY=1` | GPU busy %, idle gaps, submit-to-done time, **GPU time per guest queue** (timestamps per command buffer and where the queue changes; cheap) |
| `BB_QUEUE_WATCH=<hex>` | which guest queue's work (or DMA) writes the 64 KiB from that address; work per queue |
| `BB_QUEUE_SHADERS=1` | compute-queue dispatches by shader (count, groups, buffers written) and how many start with a barrier |
| `BB_SAMPLE_THREADS=1` | Windows sampler of the GPU threads (busiest addresses, callers) |
| `BB_GPU_WRITE_WATCH=<hex>` | GPU writes and guest write faults on a 64 KiB range |
| `BB_ASYNC_TRACE=1`, `BB_ASYNC_VERIFY=1` | async read-back accesses / stale-data check |

Avoid `BB_GPU_PROFILE=1` in U3 gameplay: it writes a timestamp per dispatch and hangs once the
skinning starts (~108k dispatches/s). `BB_GPU_BUSY` replaces it for this purpose.

## How it was measured

- **FPS:** scripted runs with `"Play Uncharted.exe" u3` plus `BB_PAD_FILE` (pressing ✕ through
  the title, menu and notice into the Continue save), `BB_FRAME_STATS=1`, 40–60 s in the square.
  Remove `BB_PAD_FILE` from the launcher's extra variables afterwards — it disables the controller.
- **Glitches:** NVIDIA recording at 240 FPS of the pub fight, then
  `python tools/glitch_scan/oneframe_scan.py <video> <dir>` and
  `python tools/glitch_scan/mask_sheet.py <dir> <prefix>`. Review every contact sheet and the
  largest candidates (including ones in runs of consecutive frames) by eye: fast motion produces
  many harmless candidates, and a scanner verdict alone has missed real spikes.

## Other fixes the same day

- Bloom option (`BB_BLOOM=0`, launcher toggle), bloom bright-pass clamp.
- Anisotropic filtering only in G-buffer draws, off by default (softened the picture with DLSS).
- Crash-safe save writes (from bloodborne_pc).
- Tutorial button-prompt text missing; frame generation crash when the picture size changed;
  DLSS history reset on camera cuts (streaks).
