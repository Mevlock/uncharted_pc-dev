# U2: game stops on "pAction != NULL" (moveactioncontroller.cpp:40)

## Symptom

Uncharted 2 stops on its own assertion `pAction != NULL` in `game\moveactioncontroller.cpp:40` (CreateAction),
then a fault at guest offset 0x2b3f57. Seen twice on the maintainer's PC, with identical backtraces:

- 2026-10-22, chapter 2 museum, on a tranquilizer takedown (0.3.6.15).
- 2026-10-27, Nepal war zone, jumping onto something during play (0.3.6.27 code, about 43 minutes in).

Both logs and saves are kept locally.

## Cause

CreateAction (EU 0x2b3ef0) takes the player's move action from a fixed pool: the object at character+0x2e8, pool at
+0xf0, 64 slots of 0x300 bytes, a slot bitmap. A full pool returns NULL and the assertion fires.

A diagnostic hook on CreateAction (a local test build) logged the pool every 2 s for about 30 minutes at 144 fps:

- Normal play, including heavy traversal (up to ~250 actions created a second): at most 6 slots taken at once.
- Bursts during fights, typically right after a jump: 16, 28 and 30 slots. In a burst two callers alternate,
  `CreateAction(3)` from 0xa565c8 and `CreateAction(0)` from 0xa1a808 (the crash's caller), and every pair keeps one
  more slot. Two move states take turns every frame and one action is not released each time.
- The swaps likely happen once per frame, so at high frame rates the same moment keeps several times more actions than
  at 30 fps. A longer burst fills all 64 slots.

## Fix (0.3.6.28)

The pool's slot count is set where the character is created (`mov ecx, 64` passed to the pool setup). It is patched
to 256 slots (+144 KB from the character's heap):

- EU CUSA02343: 0x33872c (character in r13, setup 0x2b4560).
- US CUSA02320 (= CUSA02344): 0x721347 (character in r15, setup 0x5e00f0, the same pool at +0xf0 with 0x300 slots).

Tested: EU about 30 minutes of play at 144 fps (bursts of 16-30 absorbed, Drake spawns without a heap complaint);
EU and US scripted runs on the release build (patch applied, gameplay, no assertions). The game's leak itself is
unchanged; the pool just has 4x the room.
