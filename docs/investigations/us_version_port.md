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
2. [x] (done 2026-10-20: US image+0xd9ce70, found by the heap marker 0x4ababeef) **U3 heap free guard** (crash fix, runtime_pad.c `0x494a70`).
3. [x] (done 2026-10-20: US image+0x72778f, same target slot [rbp-0x60]) **U2 camera focus NaN reset** (crash fix, `0x33ba13`).
4. [x] (done 2026-10-20: US image+0x4713b9) **U2 leg IK NaN delta** (crash fix, `0x8ec618`).
5. [x] (done 2026-10-20: US waits in microseconds, mov edx, 10000; now 1000 for 1 ms, log "wait 1 ms") **Job sleep** (speed; pattern search, EU and CUSA02344 forms only, probe.c).
6. [x] (found in all three games) **EOP tick wait** (frame pacing; pattern search that should accept US 1.02).
7. [x] (done 2026-10-20: US is the same code moved: follow camera 0xaede0, pad update 0x117350, joypad sticks 0x11799e; mouse look 45.5/180.6/716.6 deg/s vs EU 45.3/180.4/720.2. Fast aiming stops at stick speed on US, ~45 deg/s vs EU 176/778: the same US stick limit as U2/U3, parked with 10/11) **U1 mouse camera**: follow camera `0xaecf0`, pad update `0x117200`, joypad sticks
   `0x11784e`.
8. [x] (done 2026-10-20: US follow camera image+0x42c690) **U2 mouse camera**: follow camera `0x171fe0`.
9. [x] (done 2026-10-20: US follow camera image+0x4e7970; scripted sweep 45.8/184.5/720.5 deg/s vs EU 45.6/184.7/722.6) **U3 mouse camera**: follow camera `0x121b90`.
10. [ ] (parked 2026-10-20: pad update hooked at US 0x565d30/0x56668e, but aiming stays at stick speed, ~45 deg/s: US inlines its own stick pipeline into UpdateCameraDirection (player-aiming-machine3.cpp, 0x71a581) with several caps; removing the magnitude cap at 0x71a5f1 changed nothing. Next idea: turn the aiming camera directly, as the follow camera does) **U2 raw aim**: camera input `0x16d920`, pad sticks `0x251be8`.
11. [ ] (parked 2026-10-20 with 10: US U3 also has the extra length-limited stick pipeline EU lacks (0x9291a7), so the pad-stick approach will stop at stick speed there too; camera input applied via CUSA02344 0x4e42f0) **U3 raw aim**: camera input `0x11d4e0`, pad update `0x741a30`, pad sticks `0x74231e`.
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
14. [x] (confirmed 2026-10-20: the same feature lines as EU in all three games) **Movie scaling and GPU memory sizes** (byte patterns).
15. [x] (confirmed 2026-10-20: same lines as EU, incl. 16x filtering and camera motion) **Shader-hash fixes** (bloom clamp, U2 aiming DOF, HUD squeeze, movie shader, camera
    motion, light shaft fog): expected identical; confirm with the shader cache hashes.
16. [x] (works) **Intro logo skip** (`sce-ndi-logos.bsf`): compare the file.
17. [x] (done 2026-10-20: u2_us/u3_us_rdtsc_sites.h, 235 and 295 reads) **Slow motion clock lists** (U2/U3 rdtsc sites; diagnostic only).
