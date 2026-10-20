# Player report, 2026-10-20: present mode, U3 aiming blur, blue lines, shimmering after relaunch

Status: open. Reporter's build and settings not attached yet (the report says "here is my
settings", but they didn't come through).

## Report (verbatim)

> fifo relaxed (mailbox still had those even with 60) and 60fps lock fixed floating or clipping
> ground, full dof disable in uncharted 3 still blurry while aiming and some blue lines bug i found
> on one wall, also do i need to clear cache everytime i launch game? cuz shimmering/moving objects
> on screen came back with same settings as first launch today, here is my settings

## Items

1. **Floating or clipping ground, fixed by FIFO relaxed + a 60 fps lock.**
   With Mailbox it still happened, even locked to 60. FIFO relaxed plus the 60 fps lock fixed it.
   Points at frame pacing or present timing (animation/IK or physics stepping at uneven frame
   times). Need: which game, where, and the frame rate before the lock.

2. **U3 still blurry while aiming with depth of field fully off (`BB_DOF=0`).**
   Our DOF option skips U2's aiming DOF compute shaders (8b10b4cb, 1c3672c5, `bbgpu_note_aiming`
   from the U2 pad hook). U3's aiming blur probably uses other shaders that the option doesn't
   cover. Next: find U3's aim blur pass (BB_SKIP_SHADERS / draw log while aiming) and add it.

3. **Blue lines on one wall.**
   Need: game, chapter, a screenshot or the save.

4. **Shimmering / moving objects came back on a later launch with the same settings; does the cache
   need clearing every launch?**
   Clearing the cache should never be needed. The symptom fits what we found the same day: a
   session that compiles shaders live can render differently from a session that loads them from
   the cache (the U3 mine's flashlight glow went missing in some cold-cache sessions, intermittently;
   investigation in progress, see the compiler-thread runs of 2026-10-20). Here the shimmering
   came back with the cache warm, so it may be the opposite direction, or the upscaler/sharpening.
   Need: settings (upscaler, frame generation, render resolution), which game and where.

## To ask the reporter

- Build (0.3.6.x) and game.
- Their launcher settings (the screenshot/text that didn't come through).
- Where each item happens (chapter/spot), screenshots or a short video, and a save if possible.
- `user\last_run.log` from a run where it happens.
