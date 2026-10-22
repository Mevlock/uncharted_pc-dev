# Uncharted 2: weapons carried into a chapter are invisible

Status: open, investigating. Found 2026-10-22 on our own playthrough (EU CUSA02343, from a
complete save via chapter select).

## Symptom

Drake aims and fires normally and the HUD shows the weapon (1911, 40 rounds), but no gun is drawn
in his hands. It stays that way after reloading the checkpoint. A weapon picked up in the level is
drawn normally.

- Repro save: `uncharted_saves/uc2/2026-10-22_gun-gone` (35%, "The City's Secret", chapter 8).
  It was reached from the complete save `uc2/2026-10-22_complete-eu`.
- It is not a regression: 0.3.6.16 and the 0.3.3 reference build show it too. In 0.3.3, a
  picked-up gun is drawn black, the old upstream black-weapon bug, which we have fixed.

## Findings

### RenderDoc

Captures are in `dist/.../user/rdc`:
- `u2_frame3822`: the carried gun, while aiming;
- `u2_frame7678`: a picked-up Beretta, drawn.

Both are full frames, taken with `BB_RDOC_CUT=2,600`, F11, and a fast camera flick as the cut. A
checkpoint restart also works as the cut, but it drops a picked-up gun.

1. **Nothing is drawn at the hand pixels in the carried-gun frame.** Pixel history on the scene
   depth shows only background, and nothing even fails the depth test there.
2. **Drake's carried weapons are skinned every frame and never drawn.** The skinning compute
   shader (`8f067540`) writes their deformed vertices, but no draw reads them, at any binding:
   - a 1028-vertex pistol, the same mesh and size as the Beretta (dispatch 1073);
   - a 1863-vertex, 1-joint long gun on his back (1081).

   Chloe has the same split: a 707-vertex pistol is skinned and not drawn (1037), while her
   1081-vertex weapon is drawn (1029). Bodies, eyes and other attachments are drawn normally.
3. **In the picked-up frame, the Beretta draws and the long gun still doesn't.** The Beretta
   (dispatch 1074, event 1310) is skinned with its own 9-joint palette. The long gun (1070) is
   still not drawn.
4. **The 450-vertex mesh at events 3654 and 3620 is the eyes, not a gun.** It was briefly taken
   for the pistol.

### Port diagnostics

These are automated runs (scratchpad `autotools/gunrun.sh`: U2, Continue, hold aim, take
screenshots). They reproduce the empty hands.

- **The game never issues the gun's draw.** `BB_DRAW_WATCH_INDICES=2991` sees no draw of the
  pistol mesh at all. The controls are seen: the body (13248), the eyes (2400) and Chloe's weapon
  (2838).
- **No draw is dropped for a missing pipeline** (`BB_NULL_PIPELINE_LOG=1`). `BindResources` and
  `FilterDraw` don't drop it either.
- **Not COND_EXEC:** U2 issues none (`BB_COND_EXEC_LOG=1`).
- **Not occlusion queries:** the port always answers "visible".
- **Not shared writes** (`BB_SHARED_WRITES=0`, manual test), so not the 0.3.6.17 speed-up.
- **Not small GPU results:** `BB_READBACK_SMALL_BUFFERS=all` changes nothing. U2's only small
  compute results are the two exposure values.
- **Not the skeleton:** the gun's palette copy is identical to Drake's body palette.
- **The read probe and write watch were inconclusive.** With `BB_READBACKS=2 BB_READ_PROBE=1`
  plus `BB_GPU_WRITE_WATCH`, the game memcpy-reads pages next to the skinning output each frame,
  but those pages also hold the game's own data. `BB_READBACKS=2` alone freezes U2 at launch.
- **Not Umar's fixes:** Umar-Qayyum-dev's full build (0.3.6 plus his patches, including the
  read-back of the scalar copy destinations into guest memory before each fence, his U2
  floating-NPC fix) shows the same empty hands on the same save. Tested by the user,
  2026-10-22, from the test install `github/uncharted_test_umar`.
- **The "not found" weapon effect packs are irrelevant.** `effect1/*-pistol.bin` is not found for
  every weapon, including the drawn Beretta.

- **Not port code at all, most likely:** BilalReloaded's port, a completely separate port with
  no shared code, also shows the empty hands on the same save (US copy CUSA02320, the save
  copied into its slot; seen by the user, 2026-10-22).
- **Port switches change nothing** (automated, aim screenshot each): `BB_DRAW_PIPE=0`; draw
  preparation off; `BB_SCALAR_COPY64_DEFER=0`; `BB_TIGHT_WRITES=0`; ordered faults, pipelined copy
  and shared writes off.

- **Exact GPU reads can't be tested yet:** `BB_READBACKS=2` freezes U2 at launch and also when
  switched on in gameplay (`BB_READBACKS_DELAY=60`, new), most likely a read waiting for GPU
  work that waits for the reading thread. Both ports share the shadPS4 video core, so a stale
  GPU result read by the game is still possible on both; it needs exact reads that don't
  deadlock (a stack dump at the freeze would show which read waits on what).

## Remaining theory

Two unrelated ports show the same thing, so the cause is most likely in the save, or in the game
itself on this path, not in either port. The save is a completed game from a Spanish-language
console, continued through chapter select. Next: check whether a weapon carried over a chapter
boundary also vanishes in a save made from a fresh EU game, and look for the same thing in PS4
footage of a chapter-select replay.

Earlier theory, now unlikely:

The game itself decides not to draw weapons that were carried into the chapter. Possible reasons:
- a streaming or residency state, or a visibility state, set at chapter start;
- data the port gives the game wrong at that point (async file I/O completion, a GPU result read
  without a fault).

## Next steps

- Automated batch, aim screenshot each, one at a time: `BB_DRAW_PIPE=0`; draw preparation off;
  `BB_SCALAR_COPY64_DEFER=0`; `BB_TIGHT_WRITES=0`; ordered faults and pipelined copy off.
- Other saves: the museum gun save `uc2/2026-10-19_gun`, and a fresh EU new game. Does a weapon
  carried over a chapter boundary vanish there too?
- PS4 footage of chapter 8: is the rifle on Drake's back?
- Then, if needed, reverse engineering of the game's weapon draw/visibility code.

## Diagnostics added (local commits, all off by default)

- `BB_GPU_WRITE_WATCH` also names the shaders writing the watched address.
- `BB_NULL_PIPELINE_LOG=1`
- `BB_DRAW_WATCH_INDICES=n,...`
- `BB_STALE_CHECK=<lo>:<hi>`, which now also lists compute outputs no draw read
  ("Unread outputs").
