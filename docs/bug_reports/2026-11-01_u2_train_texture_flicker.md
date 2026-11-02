# U2 ch1 train wreck: a texture on the train flickers

- Found 2026-11-01 by the user at U2 checkpoint #2 (train-wreck-weather, ch1 "A Rock and a Hard Place"), EU install.
- Seen with: batch 3 build (b294ddeb), batch 3 with BB_MIP_CHAIN_CLAMP=0, the play-install build (3a7f7e8d), and the
  play-install build at the game's own settings (1920x1080 window, 60 fps, AA off).
- So: not from the 2026-11-01 survey picks, not DLSS / AA / render size.
- Next: PS4 footage of the same spot; if the PS4 is clean, a RenderDoc capture of a flicker frame (F10 slow motion helps)
  to find the draw and texture. Which texture exactly (train side / window / floor / roof) still to note.
- Log (PS4 settings run): uncharted_local_notes/logs/2026-11-01_u2_train_flicker_ps4settings.log
- User: the flickering texture is part of the train itself (the carriage), 2026-11-01.

## Plan before RenderDoc (2026-11-01 23:20)
Present in the play-install build (3a7f7e8d), so only changes up to it matter. Quick checks, cheapest first:
1. Live, no restart (BB_TOGGLE_FILE=<file>, re-read every 250 ms; "mask mask2"): switch the texture-path memos off
   while looking at the train: 32 texture descriptor cache, 128 image lookup cache, 1024 texture binding memo,
   1<<45 texture set memo, 1<<48 sampled scene proxies; mask2 1 (texture set exact), 2 (CPU write page check),
   64 (recycled images). Decimal (fscanf %llu): file content "316659348800672 67". Flicker stops -> halve the bits to find the one.
2. Restart switches from the last two weeks: BB_OVERLAP_KEEP_VIEW=0 (a6382a46, 10-24), BB_MIP_FIX=0 (ab15eaf0,
   10-22, on by default), BB_ANISO=0 (forced 16x anisotropy on G-buffer draws).
3. The 0.3.3 reference build (github/uncharted_033, mid-October): flicker there too -> older than two weeks (or the
   game); then PS4 footage.
4. Still unexplained: RenderDoc capture of a flicker frame (U2.bat, F12; retry after device lost), F10 slow motion.

## Found (2026-11-02 00:30): the runtime's region cache (BB_TOGGLE_FILE bit 0, BB_REGION_CACHE=0)
Live bisect at U2 #2 with checkpoint restarts (picks3 build): all toggles off -> gone; mask 0-31 / 0-15 / 0-7 / 0-3 /
0-1 -> gone; bit 0 alone (region cache) off -> gone; bit 1 alone off (cache on) -> back. Region cache = per-thread
cache of the guest mapping table (src/runtime_memory.c cached_region) for the GPU's per-draw queries
(runtime_memory_clamp / _region / _is_mapped: ClampRangeSize, CopySparseMemory uploads, write log).
Code reading: every table change takes write_lock (generation bump), so no stale entry found yet; either a wrong-answer
case not seen, or timing (the lock-free path hides/shows a race elsewhere). Check build BB_REGION_CACHE_CHECK=1
(perf_builds/2026-11-02_regioncheck): every hit re-looked up under the lock; logs WRONG / used-while-changed.

## Narrowed (2026-11-02 ~01:30)
- Check build: 218M region-cache hits, 0 wrong, 0 used while the table changed -> the cache is correct; it changes timing.
- Per-function: only runtime_memory_clamp locked -> gone (ring + draw recording sizes); region (uploads) locked -> still.
- Constant ring off (bit 36) -> the WHOLE train flashes white (much easier to see): same race, bigger.
  Ring off + stream copy memo off / deferred stream copies off / pipelined memory writes off / early labels off -> still.
  Ring off + draw pipeline off (37) -> fine; ring off + pipelined tasks off (38) -> fine.
- Original (ring on) with only pipelined tasks off (38) -> no flicker. BB_GPU_FENCES=1 (fences after GPU completion) -> still
  flickers: not fence timing vs GPU reads; ordering inside our pipeline (stage A runs on past a task handed to stage B).
- Next: build 2026-11-02_tasksites: per-site drain switches in mask2: 1024 EOS, 2048 EOP, 4096 RELEASE_MEM,
  8192 fault callbacks, 16384 in-order flush.

## Cause found (2026-11-02 ~02:00): end-of-pipe events pipelined (stage A runs on past them)
Per-task switches (build 2026-11-02_tasksites): mask2 2048 (EOP after a drain) -> whole-train flicker gone (ring off) and
the original flicker gone (ring on). Interim fix (uncommitted, build perf_builds/2026-11-02_eopdrain): EOP after a drain by
default, BB_EOP_PIPELINED=1 = before. User wants the proper fix next: find what in/after RunEventWriteEop stage A overtakes
(ReadBackSmallWrittenBuffers -> small GPU-written buffers read as constants? CheckConstants? the guest's interrupt
handler writing data stage A already prepared?) and order only that.

## EOP drain A/B (2026-11-02 01:30, stopped after 2 runs): too slow
picks3 (A) vs eopdrain (B), U3 market usual round: mean 123.9 -> 108.7 (-12%), market 108.9 -> 105.0; street windows
141-144 -> 120-138, crowd 95-111 -> 88-97; stage A waits RunInOrder 4.2% (EOPs are frequent in U3). Not shippable.
Working tree: default back to pipelined, BB_EOP_DRAIN=1 = the drain (diagnosis). Targeted fix next.
