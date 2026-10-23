# Giving buffer video memory back (design, 2026-10-23)

**Why:** the buffer arenas only ever grow. U3's square standing still: +13 MiB a minute; a player with a
6 GB GTX 1660 Super ran U3 from London to the chateau with the texture cache at 3.4-3.7 GiB of a
1.9 GiB budget, then the device was lost (docs/bug_reports/2026-10-23_u3_ch5_blue_haze.md). On cards
with 4-6 GB the driver starts paging, then fails.

## How the arenas work today
- Guest addresses map 1:1 onto sparse arena buffers (`arenas`, buffer_cache.h). A 64 KiB block becomes
  resident on its first use: `ObtainBuffer` -> `EnsureResident` (buffer_cache.cpp ~2406).
- `EnsureResident` allocates **one new `vk::DeviceMemory` per call**, sized for the gaps it binds,
  queues the sparse binds (`pending_binds`, committed by `SubmitPendingArenaBinds` before the next
  submit) and writes the blocks' addresses into the BDA page table (`bda_pagetable_buffer`) that
  shaders use for pointer access.
- `resident_ranges` (IntervalList<Backing>) records block range -> (memory, offset). Nothing ever
  removes an entry or frees a `DeviceMemory`; `g_resident_buffer_bytes` only grows.
- Side effect: the number of driver allocations grows too (one per first-use burst). Some drivers
  cap it at 4096 (`maxMemoryAllocationCount`); worth logging next to the bytes.

## Proposal
1. **Pooled backing.** Allocate arena memory in fixed chunks (e.g. 32 MiB) and hand out 64 KiB blocks
   from a free list. Released blocks go back to the list and are reused first; a chunk whose blocks
   are all free is freed once memory is high. Fixes the allocation count as well.
2. **Use stamps.** Per resident range (or per 2 MiB of blocks): the scheduler tick of the last
   `ObtainBuffer` that touched it. Cheap: one store per binding.
3. **Release pass** (GPU thread, every ~5 s, only while resident bytes exceed a threshold: a quarter
   of VRAM like BilalReloaded's port, or better the `VK_EXT_memory_budget` heap budget). A range is
   released when all of these hold:
   - unused for >= 60 s and its last tick is complete (`scheduler.IsFree(tick)`);
   - no GPU-only data: not in `gpu_modified_ranges`, `shared_gpu_ranges` or a pending read-back
     (`late_read_backs`, side queue) — the guest copy must be the truth;
   - not a ring/stream binding and not under an image the texture cache holds as buffer-backed.
   Release = sparse unbind (`SparseMemoryBind` with null memory) through `pending_binds`, BDA entries
   pointed at a shared zero block (never left dangling: a stray shader read must not fault),
   `resident_ranges` entry removed, blocks returned to the pool, and
   `memory_tracker->MarkRegionAsCpuModified(range)` so the next use uploads from guest memory.
4. **Diagnosis:** `BB_ARENA_RELEASE=0` off; frame stats line: resident MiB, allocations, released
   MiB per 5 s, re-uploads caused by releases (a release that comes back within seconds is a bad
   pick and should stop the pass from taking that range for a while).

## Risks
- A range released while GPU work still reads it: guarded by the completed tick; unbinds go in the
  sparse queue before the next submit, after earlier submits, like the binds.
- Data only the GPU has (compute results never read back): guarded by `gpu_modified_ranges`; when in
  doubt, keep the range.
- Re-upload cost if the threshold is too low: measure in U2 museum and U3 square standing still and
  walking (fps must not move), then a 60-minute U3 run on a 6 GB budget (`BB_VRAM_LIMIT`-style cap)
  showing resident bytes level off.

## Order of work
Pooled backing first (no behaviour change, allocation count flat), then stamps + release pass
behind the switch, then the long-run test.
