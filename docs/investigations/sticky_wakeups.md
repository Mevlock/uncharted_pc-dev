# Sticky wake-ups for the job manager (groundwork, 2026-10-23)

**Goal:** replace the 50000-spin / 1 ms workaround (5d519ad, U3 one-frame lighting flashes) with a fix
for the missed wake-up, so idle job workers can sleep the game's way (1000 spins, 10 ms) on every CPU.

## The code (EU 1.02, boot-linked.bin offsets; same shape in all three games)
| | U3 (big3) | U2 (big2) | U1 (eboot) |
|---|---|---|---|
| worker: flag re-check (`cmp byte [rax+0x59],0`) | 0x70b10c | 0x67d8dc | 0x4215c4 |
| worker: timed wait `call` (rdi = mgr+0x80 cond, rsi = mgr+0x60 mutex, edx = 10 ms) | 0x70b125 -> 0x7070e8 | 0x67d8f5 -> 0x678618 | 0x4215dd -> 0x41c210 |
| submission: lock mgr+0x60 (`call`), then signal (rdi = mgr+0x80, rsi = mgr+0x60), unlock | 0x709d65: signal `call` at 0x709d87 -> 0x707278 | (pattern site 3) | (pattern site 3) |

The submission signals with the manager's mutex held, and the worker waits with the same mutex held,
so both hooks run under one lock. The worker's queue check happens before it takes the mutex: a job
pushed and signalled in between wakes nobody.

## Plan
1. Hook the submission's signal call: set `pending[cond] = 1`, then call the original.
2. Hook the worker's timed-wait call: if `pending[cond]`, clear it and return 0 (a spurious wake-up,
   allowed; the worker loops and scans the queue again); else call the original wait.
3. Defaults back to the game's 1000 spins and 10 ms; `BB_JOB_STICKY=0` turns the hooks off.
4. Sites found by pattern next to patch_job_sleep's (EU encodings above; US 1.02 / CUSA02344 use
   another submission encoding and microseconds, `mov edx, 10000`).

## Tests
U3 mine flashlight spot (no flashes with 1000/10), FPS 0.3.6.23 vs sticky on 12 threads (affinity
0xfff) and 16, U1/U2/U3 EU+US soaks, 4-core U2 death loop.
