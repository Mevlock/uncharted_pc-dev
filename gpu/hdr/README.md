# Scene HDR for Uncharted 2 (tone pass replacement)

From moistman7's PR #156 to BilalReloaded's port (HDR10 with Uncharted 2 scene HDR), GPL; the
`renodx/` folder is RenoDX's colour library, MIT (its `LICENSE` and `SOURCE.txt` are kept).

`fs_3371ae8d_hdr.spv` replaces the game's tone pass pixel shader (guest hash `3371ae8d`, the same
program in all three games and in every release: the port's translations from the EU 1.00, US 1.02
and EU-Russian caches are byte-identical) in a second draw into an RGBA16F shadow. The pass after
it (`484cdbf5`, BB_HUDLESS_PS) is drawn again with that shadow as its input into a second RGBA16F
shadow, which the upscaler (DLSS, FSR 4, XeSS, TAA) reads instead of the game's 8-bit frame. The
build embeds the `.spv` in bb-probe.exe (`spv_to_header.cmake`); nothing is shipped beside it.

The replacement keeps the translated shader's interface (bindings 0-9, the 128-byte push
constants, the per-vertex input of the NVIDIA barycentric profile) and adds a read-only buffer
with the live settings (`BbHdr::Tuning`, gpu/shim/bbport_hdr.h: peak and paper-white nits, then
RenoDX's exposure, saturation, highlight saturation, contrast, per-channel chrominance correction,
per-channel blowout restoration, hue correction, per-channel tone mapping and the highlights
start, read by the
`RENODX_*` macros at the top of the `.hlsl`; the menu's HDR section changes them, a new slot of a
small ring per change); its binding (12 in the file) is renumbered at load to the
pipeline's first free binding. Only built with HDR on at start (BB_HDR=1, an HDR10 display), on
NVIDIA with VK_KHR_fragment_shader_barycentric; elsewhere the picture is HDR10 output of the SDR
frame.

Highlights start (`hdr_highlight_start`, default 0.60): below that SDR luminance of the game's
own filmic output the replacement keeps that output unchanged (sunlit surfaces stay at paper
white); above it the gain-matched scene signal takes over with a smoothstep from `start` to
`start + 0.6 * (1 - start)` (0.60 -> full at 0.84). 0 (bbport.ini, BB_HDR_HIGHLIGHT_START or the
tune file only; the menu's range is 0.10-0.95) is the earlier ramp, `(luminance / 0.18)^2`, full
from mid grey.

Live tuning from a file (HDR10 output only): `BB_HDR_TUNE_FILE`, else `hdr_tune.txt` in the user
folder (`BB_GPU_USER_DIR`). Its modification time is checked every 250 ms; when it changes after
the start, its `key=value` lines (the bbport.ini keys `hdr_paper_white`, `hdr_peak_nits`,
`hdr_exposure`, `hdr_saturation`, `hdr_highlight_saturation`, `hdr_contrast`, `hdr_chrominance`,
`hdr_blowout`, `hdr_hue_correction`, `hdr_per_channel`, `hdr_sdr_gamma`, `hdr_sdr_saturation`,
`hdr_highlight_start`; `#` comments, other keys ignored) are applied as if the menu's sliders were
moved (same ranges), each changed value is logged (`HDR tune: hdr_saturation=1.20 (file)`) and
bbport.ini is written.

Rebuild from PowerShell with the DirectX Shader Compiler:

```powershell
./gpu/hdr/build.ps1 -Dxc C:/path/to/dxc.exe
```

The checked-in `.spv` is built with the Vulkan SDK 1.4.341.1 DXC
(`C:/VulkanSDK/1.4.341.1/Bin/dxc.exe`, dxcompiler 1.9.0.5180), which reproduces it byte for byte.
