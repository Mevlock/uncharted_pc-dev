# Uncharted: The Nathan Drake Collection for Windows — Mevlock's fork

> [!WARNING]
> **Unofficial fork.** This is a hobby fork of [SuperMedo's uncharted_pc](https://github.com/Supermedo/uncharted_pc),
> the official port. Please don't report problems with this fork to him; open an issue here instead.

Uncharted 1, 2 and 3 from the PS4 collection, running natively on Windows 10 and 11. The original PS4 game
code runs directly on your PC and the graphics are translated to Vulkan.

**[⬇ Download 0.3.6.34](https://github.com/Mevlock/uncharted_pc-dev/releases/latest)** ·
[What's new](docs/releases/v0.3.6.30.md) · [How much faster](docs/PERFORMANCE.md) · [Known issues](#known-issues) · [How the shader preparation works](docs/SHADER_PREPARATION.md)

> **No game files are included.** You need your own decrypted dump of the collection, version 1.02:
> EU/UK (CUSA02343), US (CUSA02320) or EU/Russian (CUSA02344). The folder holds eboot.bin,
> big2-ps4_Shipping.elf, big3-ps4_Shipping.elf, sce_sys, u1data, u2data and u3data.
> Not affiliated with Sony Interactive Entertainment or Naughty Dog.

## Getting started

1. Unpack the zip into a new folder and start **`Uncharted.exe`**.
2. Pick your game folder, choose a game, press **PLAY**.
3. **First start of each game:** a *Preparing shaders* page builds the game's shaders once, so you don't get
   stutter from them while you play. On a fast PC that takes about half a minute for Uncharted 3, on a slower
   one a minute or two (less for Uncharted 1 and 2). The game waits behind the page; later starts skip it.

**Updates:** the launcher checks this page for a new version and offers to install it. Your saves, settings,
shader cache and art are never replaced. You can turn the check off in the launcher.

**Coming from an older version or another port:** copy your `user\savedata` folder over, or point
**Saves folder** in the launcher at it.

## What this fork adds

### Smoother and faster
- **Shader preparation:** each game's shaders are built before you play (from lists recorded by playing the
  whole games), instead of the first time something appears on screen.
- **Faster GPU translation:** the port's CPU-side work runs on three threads and skips most of its old waiting.
  Busy scenes run up to about twice as fast as the original 0.3.6 at native resolution, more where the processor
  is the limit (DLSS/FSR upscaling, high frame rates, older processors). *Measured on one PC (RTX 5080); your
  numbers will differ, and little changes where the graphics card is the limit.* Step by step: [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
- **Idle game threads sleep** on processors with few cores, so the game's own work gets the CPU.

### Picture
- **Render resolution:** your screen's resolution (up to 3440×1440; native 4K experimental), or below it with
  DLSS, FSR or XeSS rebuilding the full size.
- **Ultrawide** (21:9, 32:9) with a field of view setting; menus and videos stay 16:9.
- **DLSS, FSR 3.1, FSR 4, XeSS, TAA**; frame generation with DLSS (RTX 40/50) or FSR 3 (any GPU).
- **Depth of field** and **bloom** options, texture filtering, a frame rate limit.

### Game fixes
- **Uncharted 2, chapter 21 (convoy):** Elena's truck no longer sinks into the snow after the enemy truck rams it.
- **Uncharted 1, jet ski:** floating barrels no longer freeze in mid-air above 60 FPS.
- **Uncharted 1 and 2:** no more stops on the game's move-list check at high frame rates.
- **Uncharted 3:** Sully's shirt no longer throws dark triangles across the screen (Cartagena); the
  chapter 8 fort crash is gone.
- **NVIDIA:** no more graphics-device-lost crashes in Uncharted 1 and 2 above about 55 FPS.

### Convenience
- **Skip the intro logos** and **skip the friends' stats notice** after Continue / New Game.
- Keyboard, mouse and controller together, all remappable; button prompts follow what you play with.
- Trophies (in-game notice and a launcher page), Add to Steam, desktop shortcuts, crash-safe saves.

## Requirements

- Windows 10 (1903 or later) or 11, 64-bit.
- A processor with AVX: Intel Core 4th generation or newer (older Intel Core runs with a slower fallback),
  AMD Ryzen or FX.
- A graphics card with Vulkan 1.3, a current driver and **6 GB of video memory or more** (4 GB cards: keep
  anti-aliasing and frame generation off).
- About 8 GB of free memory (RAM + page file).

## Known issues

- The shader preparation has been tested on NVIDIA graphics; AMD and Intel reports are welcome.
- The first start after updating rebuilds the shader cache once (its format changed in 0.3.6.30).
- Frame rates above 120 are experimental.

**Reporting a problem:** open an issue with the game, chapter, your graphics card, and `user\last_run.log`
from the game folder (copy it right after the problem; the next start overwrites it).

## More

- [Release notes](docs/releases/) · [Performance, step by step](docs/PERFORMANCE.md) · [What is finished and what is not](docs/STATUS.md) ·
  [How the shader preparation was built](docs/SHADER_PREPARATION.md)
- Building: MSYS2 CLANG64, `bash build.sh`, then `bash packaging/windows/package.sh`.

## Credits

Built on SuperMedo's [uncharted_pc](https://github.com/Supermedo/uncharted_pc), on
[bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and on the
[shadPS4](https://github.com/shadps4-emu/shadPS4) renderer.

Fixes and features from: [Umar](https://github.com/Umar-Qayyum-dev) (button prompts, keyboard layouts,
the NVIDIA large-workgroup fix), [drakolordx7](https://github.com/drakolordx7/uncharted_pc) (first-sight
stutter, Uncharted 2 crash fixes, Xbox prompts, trophies, XeSS, the convoy speed fix),
[BilalReloaded](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port) (idle thread
sleep, the render target heap move, chapter unlocks), [G3545](https://github.com/G3545),
[d4rksp4rt4n](https://github.com/d4rksp4rt4n), [eduardoghi](https://github.com/eduardoghi),
[iannitram](https://github.com/iannitram), [antikmewzik](https://github.com/antikmewzik/uncharted_pc_controls_qol),
[spacepepperoni](https://github.com/spacepepperoni/uncharted_Linux), [zorde23](https://github.com/zorde23/Uncharted_Collection_PC),
[kj2223](https://github.com/kj2223/uncharted_pc-fixes), [che2281337](https://github.com/che2281337/uncharted_pc).

Also used: FSR-Vulkan and the AMD FidelityFX SDK, NVIDIA DLSS, Intel XeSS, Videodec2 and the DLSS bridge from
IFreemz's shadPS4 Bloodborne build.

The launcher's pictures (the game scenes, the map banner, the compass emblem and the icon) are original artwork drawn by code (packaging/windows/make_uncharted_art.py); no official art or game images are included.
