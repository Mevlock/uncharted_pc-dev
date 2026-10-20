# US version (CUSA02320 1.02): what needs its own addresses

CUSA02344 (EU/Russian) runs the same 1.02 code as US, so everything here covers it too.

The user's US dump: `E:\Emulation\ps4\Games\CUSA02320` (UP9000-CUSA02320_00-UNCHARTEDTRILOGY,
APP_VER 01.02). Its executables differ from the EU ones (big3 25.9 MB vs 22.9 MB, big2 18.4 MB vs
16.4 MB, eboot 10.37 MB vs 10.62 MB), so fixed addresses don't carry over. Hooks check the original
bytes first: on US they stay off rather than crash.

Status: [ ] to do, [x] done, [-] not needed.

1. [x] **Scan** (2026-10-20). Finding: US 1.02 runs the same code as CUSA02344 (the hooks' CUSA02344
   alternatives matched). **Scan.** One launch of each game from the US folder; the log says which patches found
   their sites. Confirms the rest of this list.
2. [ ] (not applied) **U3 heap free guard** (crash fix, runtime_pad.c `0x494a70`).
3. [x] (done 2026-10-20: US image+0x72778f, same target slot [rbp-0x60]) **U2 camera focus NaN reset** (crash fix, `0x33ba13`).
4. [x] (done 2026-10-20: US image+0x4713b9) **U2 leg IK NaN delta** (crash fix, `0x8ec618`).
5. [ ] (sleep applied at all 3 sites; the 1 ms wait tweak not: "wait -1 ms") **Job sleep** (speed; pattern search, EU and CUSA02344 forms only, probe.c).
6. [x] (found in all three games) **EOP tick wait** (frame pacing; pattern search that should accept US 1.02).
7. [ ] (not applied) **U1 mouse camera**: follow camera `0xaecf0`, pad update `0x117200`, joypad sticks
   `0x11784e`.
8. [ ] (not applied) **U2 mouse camera**: follow camera `0x171fe0`.
9. [ ] (not applied) **U3 mouse camera**: follow camera `0x121b90`.
10. [ ] (camera input applied via the CUSA02344 address 0x428f40; pad sticks not) **U2 raw aim**: camera input `0x16d920`, pad sticks `0x251be8`.
11. [ ] (camera input applied via CUSA02344 0x4e42f0; pad update and pad sticks not) **U3 raw aim**: camera input `0x11d4e0`, pad update `0x741a30`, pad sticks `0x74231e`.
12. [x] (done 2026-10-20: the callback is found by its code, EU 0x65d540, US 0x6734a0) **U1 straight from the launcher** (skip the trilogy screen; probe.c `0x65d540`, `0x14ae00`).
13. [x] **Render resolution and ultrawide** (done 2026-10-20). Resolution was off because none of the
    three movie patterns matched: US has the same code with other registers (first luma
    `mov ecx, ebx; mov r8d, r13d`, chroma at +0x5f/+0xe3, second luma `mov rcx, [rbp-0x2e0]` with a
    4th stub, chroma offset `imul esi, ecx; and rsi, rdx`), now recognised. US has one 1080p config
    block per game (EU three: its constructor and mode setter are merged into the init in US) and
    no 1920.0 float constants (the view setup converts the config), so "1 display mode, 0 view
    setups" is complete; every 1920/1080 pair and 2560x1216 limit in the US games is patched.
    Ultrawide: US U2 has 6360 bytes of padding, under the 8 KiB once required: the check now asks
    for the room actually used. U1/U3 widen every paired camera read; the lower counts are inlining
    (the unwidened ones are CreateOrthographicProjection and lone zoom reads, as in EU). Checked in
    game on US U3 (EU chapter 5 save copied to the US save folder).
14. [ ] **Movie scaling and GPU memory sizes** (byte patterns).
15. [ ] **Shader-hash fixes** (bloom clamp, U2 aiming DOF, HUD squeeze, movie shader, camera
    motion, light shaft fog): expected identical; confirm with the shader cache hashes.
16. [x] (works) **Intro logo skip** (`sce-ndi-logos.bsf`): compare the file.
17. [ ] **Slow motion clock lists** (U2/U3 rdtsc sites; diagnostic only).
