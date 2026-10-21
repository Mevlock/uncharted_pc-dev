# Uncharted: The Nathan Drake Collection for Windows

> [!WARNING]
> **This is an experimental hobby fork, not the official project, and it is unlikely to be
> maintained.** It may break, lag behind, or stop being updated at any time.
> **The official source is SuperMedo's [uncharted_pc](https://github.com/Supermedo/uncharted_pc)**:
> get releases, updates and support there. Please don't report problems with this fork to him.

**Uncharted 1, 2 and 3 from the PS4 collection running natively on Windows 10 and 11, by [SuperMedo](https://github.com/Supermedo).** Mohammed Albarghouthi

Same idea as my [Bloodborne port](https://github.com/Supermedo/bloodborne_pc): the original PS4 game
code runs directly on your PC and the graphics are translated to Vulkan. Unpack the zip, start
`Uncharted.exe`, pick your game folder, choose a game and press **PLAY**.

**[Download the latest version](https://github.com/Supermedo/uncharted_pc/releases/latest)** · **[Join the Discord](https://discord.gg/s3RgH6cRfJ)**

> **No game files are included.** You need your own decrypted dump of the collection
> (CUSA02343, CUSA02320, ...: the folder with eboot.bin, big2-ps4_Shipping.elf,
> big3-ps4_Shipping.elf, sce_sys, u1data, u2data and u3data).
> This project is not affiliated with Sony Interactive Entertainment or Naughty Dog.

## This fork

A development fork of SuperMedo's port by [Mevlock](https://github.com/Mevlock), based on 0.3.6.
Everything below is optional or on by default only where it is safe; the launcher has a setting
for each.

### Performance

The GPU translation spent most of its time waiting. Fixing that roughly doubles the frame rate in
busy scenes, with no change to the picture. Frame generation off, native 3440×1440:

| Game (scene) | Before | Now |
|---|---|---|
| Uncharted 1 (the jungle) | ~112 FPS | ~151 FPS (+35%) |
| Uncharted 2 (Istanbul museum) | ~44 FPS | ~96 FPS (2.2×) |
| Uncharted 3 (Cartagena square) | ~43 FPS | ~97 FPS (2.3×) |

The biggest single fix: character skinning (about 100 000 tiny GPU jobs a second) made the GPU stop
and wait before nearly every job over a bookkeeping mistake. The games' idle job threads now sleep
instead of spinning (from [BilalReloaded's Uncharted port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port)),
which frees CPU cores: about 7% more in Uncharted 3. Details, measurements and everything
tried: [docs/PERFORMANCE_2026-10-18.md](docs/PERFORMANCE_2026-10-18.md).

### Picture

- **Render resolution** (Display page): all three games can render at your screen's resolution
  instead of enlarging 1920×1080, up to about 3440×1440 (the games' memory limit). They can also
  render below it and let DLSS or FSR rebuild the full size (Quality, Balanced, Performance).
- **Ultrawide** (21:9, 32:9): gameplay shows more at the sides and fills the screen; menus,
  loading screens and videos stay 16:9. **Field of view** setting.
- **DLSS and FSR in Uncharted 3 fixed:** the upscaler's history was reset every frame, so it never
  built up detail and the sub-pixel jitter showed as a constant shake (the title screen's rifle and
  dunes). It now keeps its history: shimmering is basically gone. Textures keep their detail at
  the upscaled render sizes (texture LOD bias), the history resets at camera cuts (no streaks), and
  **Anti-aliasing sharpness** now works with DLSS. Uncharted 1's sun shadows no longer shimmer
  under DLSS, FSR and TAA (from [kj2223's fork](https://github.com/kj2223/uncharted_pc-fixes)).
- **Bloom** on/off and **texture filtering** (anisotropic) settings.
- **Frame rate limit**: caps the frames shown, generated ones included (for example 120 with frame
  generation).

### Convenience

- **Skip intro logos**: starts at the title screen without the Sony, Naughty Dog and Bluepoint logos
  and their music, in all three games.
- **Crash-safe saves**: a save is written to a temporary file and swapped in, so a crash while
  saving cannot corrupt it.

### Fixes

- **Uncharted 3: Sully's shirt flashing** (Cartagena, chapter 2). For single frames, huge dark or
  coloured triangles shot out of Sully's shirt across the screen, and their shadows with them,
  mostly on NVIDIA and Intel graphics, only when the game ran above 30 FPS.

  The shirt is simulated as cloth on the GPU. One step of that simulation adds up pairs of
  values in the GPU's fast shared memory, and half of them come from past the end of the space
  it reserved (2 KB reserved, 4 KB read). On the PS4 a read outside that space simply returns 0, so the game never noticed.
  On a PC the translated shader read whatever lay beyond it instead (scratch data, or a value
  that means "not a number"), the cloth solve went wrong, and the shirt's vertices flew off for
  that frame. Above 30 FPS the game runs the solver with fewer steps per frame, which is why
  30 FPS looked clean. The fix makes every out-of-range shared-memory read in translated shaders
  return 0 and drops out-of-range writes, as the PS4 hardware does, so the cloth math gets the
  zeros it was written for. It cost hours of captures to find: the inputs were all correct and a
  replay of a bad frame looked clean, because the stray values only existed live.
- Uncharted 3: missing tutorial button text, treasure glints turning into big white shapes at
  reduced render sizes, a crash in the frame generation when the picture size changed between
  menu and gameplay.

### Known issues in this fork

- **Uncharted 1 and 2 with Quality, Balanced or Performance** (new) and **Uncharted 2 at native
  resolution** work in the opening areas but have not had a full playthrough (cutscene videos,
  HUD, menus, frame generation).
- **Ultrawide:** gameplay and menus are told apart automatically, and short scripted scenes can
  still switch to 16:9 by mistake. **Always fill** (Display page) never switches, at the cost of
  stretched menus and videos.

## Features

- All three games from one launcher: Drake's Fortune, Among Thieves and Drake's Deception.
- 60 FPS like on PS4, or 120 FPS (higher rates are experimental).
- Anti-aliasing with TAA, AMD FSR 3.1, AMD FSR 4, NVIDIA DLSS (DLAA) or Intel XeSS (any GPU).
- Frame generation: AMD FSR 3 (any GPU) or NVIDIA DLSS (RTX 40 2x, RTX 50 up to 4x).
- Keyboard, mouse and controller together, all remappable. Mouse look in U3.
- Button prompts show your keyboard and mouse keys while you play with them, Xbox buttons after
  input from an Xbox (or other) controller, and the PlayStation buttons after a PlayStation
  controller. Keys follow your keyboard layout (AZERTY...).
- Trophies: the games' own trophy lists, a notice in the game when one is earned, and a Trophies page in the launcher.
- Add to Steam and desktop shortcuts from the launcher, automatic updates.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core 4th generation (Haswell) or newer, an AMD Ryzen, or an AMD FX.
- A graphics card with Vulkan 1.3 and a current driver, with **6 GB of video memory or more**.
  4 GB cards (GTX 1650, RX 570) run out of video memory and crash after a while; on those, keep
  Anti-aliasing and Frame generation off.
- About 8 GB of free memory (RAM + page file).

## Known problems

- **Uncharted 3, Cartagena square (chapter 2):** at high frame rates (240 Hz setting, ~80 FPS)
  the game could freeze and close with "GPU crash: the graphics device was lost". It came from
  the speed fix for that area and is fixed in 0.3. If it still happens to you, set Frame rate to
  60 on the Display page and open an issue with your `user\last_run.log`.
- **Uncharted 3:** green or black shapes flashed on some characters (Sully's shirt in Cartagena),
  mostly on NVIDIA and Intel graphics. The cloth shader reads past its shared memory, which the PS4
  answers with zeros and the translated shader did not. Fixed in this fork (see Fixes above).
- **Crash with "ASSERTION: GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick"**
  (mostly U1 after the first cutscene): the game expected the GPU side to be done with a frame it
  wanted to reuse, and stopped when it wasn't yet (shaders building on a first run). Fixed in
  0.3.2 (0.3.4 for the 1.02 versions): the game now waits for it. If you still see it, open an
  issue with your log.
- **Intel HD / UHD graphics:** a black screen on the HD 630 is fixed in 0.3.3. Older Intel chips
  (HD 520) can still crash inside Intel's own driver.
- **Uncharted 3 London bar crash** (fault at `0xd9c59b`, or device lost in the bar fight): fixed in
  0.3.4.
- **Uncharted 2 loading crashes** (`ReadSync file (req/read: ...)`, white textures then
  `iSubMesh < m_numSubMeshInstances`): file reads could come back cut short. Fixed in 0.3.5.
- **Older CPUs:** the games use instructions of the PS4's processor (AVX, BMI1, MOVBE, LZCNT).
  AMD FX runs since 0.3.6 (its missing MOVBE is emulated, a bit slower). Intel Core 3rd generation
  and older can't run the games yet; the game says so instead of crashing with 0xc000001d.
- **Uncharted 2, train wreck:** `IsFinite(deltaTransform)` in character-leg-ik.cpp, fixed in 0.3.6
  for the EU 1.00 version.
- Frame rates above 120 are experimental.

## Building

MSYS2 CLANG64 on Windows: `bash build.sh` builds `out\bb-probe.exe`, and
`bash packaging/windows/package.sh` makes `dist\uncharted-windows` and the zip.

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of
Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4)
renderer. Video decoding (Videodec2) and the DLSS bridge come from
[IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).
Thanks to Mindro for the shadPS4 Uncharted build that showed which problems to look for.

Also used: [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (FSR),
FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the
[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[Intel XeSS SDK](https://github.com/intel/xess) (`libxess.dll` under Intel's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org),
[Pillow](https://python-pillow.org), the Cinzel font (SIL Open Font License) and the Xbox
button pictures from [Kenney](https://kenney.nl/assets/input-prompts)'s Input Prompts (CC0).

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. The default covers and the
icon are original artwork; no official art is included.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(see `licenses\` in the download).
