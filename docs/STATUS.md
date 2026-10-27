# Status of this fork (October 2026)

This fork may not be maintained for long. This page says what is where, what is finished and what
was left half-way, so anyone can pick it up. Everything here is GPL like the rest of the port.

## Branches
| Branch | What it is |
|---|---|
| `perf/recorder` | The release line: every `v0.3.6.x` tag is built from it by GitHub Actions (`.github/workflows/build.yml`). `dev` (the default branch, shown on the front page) follows it. |
| `perf/build` | The release line plus the CPU-side performance work (published as `-perf` test builds). Kept in sync by merging the release line into it. |
| `main`, others | Older lines, kept for history. |

Release notes for every build: `docs/releases/<tag>.md`. Bug investigations: `docs/bug_reports/`.

## Finished (in the release line)
- **Video memory limit and hand-back:** the texture cache and buffer arenas stay within the card's
  budget, unused arena memory is released (sparse binds), and the "Automatic" render resolution
  picks 1920x1080 on cards with 6 GB or less. Test switch `BB_VRAM_SIMULATE_MIB=<MiB>` simulates a
  smaller card with a ballast allocation. Small-card buffer placement: `BB_STREAM_MEMORY=auto`
  (off by default).
- **Older Intel CPUs:** idle job workers skip their yield when Windows' kernel VA shadow (Meltdown
  mitigation) is on (`src/probe.c`, `kernel_va_shadow_on`).
- Crash and rendering fixes listed in the release notes, for the EU (CUSA02343) and US / EU-Russian
  (CUSA02320 / CUSA02344) versions alike: every code-pattern hook is matched in both.

## The performance work (`perf/build` branch)
How it works is explained in `docs/PERF_BUILD.md` on that branch and its release notes. In short: the
translation of the game's GPU command stream runs as three threads (command processor, draw
recorder, Vulkan recorder, since 0.3.6.20); the perf work moves constant-data copies to a collector
thread, trims per-draw work and batches fence hand-offs between the stages. Measured: about +22%
where the CPU sets the pace, a few percent to +13% at native resolution on a fast GPU.

Half-done or not started, with where to begin:
- **Starting the GPU on a frame earlier** (the GPU still idles part of each frame at high frame
  rates, waiting for the CPU side): the next big step; not started.
- **Write-fault read-back on the faulting thread** (che2281337's idea): in the perf build but off by
  default (`BB_FAULT_HERE=1`), because it bypasses the ordered fault policy and crashed Uncharted 3's
  chapter 1 pub (Havok assert from a stale pointer).
- **NVIDIA Reflex** (lower latency with DLSS Frame Generation): not started. Plan: VK_NV_low_latency2
  markers around the game's frame, measure with PresentMon.

## Known open issues
- Uncharted 1: a cyan sheen on the pistol's slide while aiming is the original game's (it shows on the
  PS3 original too), not a port bug (`docs/bug_reports/2026-10-26_u1_pistol_blue_rectangle.md`).
- Uncharted 3: stepped sun shadow edges are the game's own shadow resolution, not a port bug
  (`docs/bug_reports/2026-10-26_u3_stepped_sun_shadows.md`); larger cascades would be an enhancement.
- Native 4K in Uncharted 1 (branch `feature/4k`, not merged): blocked by a 2 GiB pointer window.

## Testing
- Scripted runs load fixed saves and measure `BB_FRAME_STATS=1` output; A/B comparisons alternate
  the builds over two rounds. Pad record/replay (`BB_PAD_RECORD` / `BB_PAD_REPLAY`) replays a
  recorded route for repeatable heavy scenes.
- Before every release: a short run of each game on both the EU and US versions.
