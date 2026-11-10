/* Mouse look: the mouse turns the game's cameras directly, like a PC game (no virtual stick, no stick
 * acceleration or turn-speed cap). Technique and most handlers from BilalReloaded's port
 * (https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port, gow3_pc/src/
 * runtime_mouselook.c, commit 06b77753, GPL-2.0-or-later), ported to this loader's callbacks and to
 * both game codes: 1.02 (US CUSA02320 1.02, EU CUSA02343 1.02, CUSA02344: his addresses) and 1.00
 * (EU CUSA02343 1.00, US CUSA02320 1.00: located here).
 *
 * The loader (probe.c) runs a handler at the entry of each game's camera input functions (follow,
 * aim and cover cameras); it takes all mouse counts since the last camera update and turns the
 * direction the function is about to use, so the game's own collision, limits and swing logic run on
 * the result. Every hook site and every global it reads is checked against the original code first;
 * a game whose code does not match keeps the mouse as the right stick (logged). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"

/* All sites, every game and code version (probe.c installs those whose bytes match). */
const RuntimeGuestCallback *runtime_mouselook_callbacks(size_t *count);
/* Before the callbacks go in: which game and code version this is, and the globals its handlers read
 * (each from a checked instruction of the game's code). */
void runtime_mouselook_prepare(unsigned char *image, uint64_t image_size);
/* After the callbacks: the steering patches (the games' "the player steers" results also see mouse
 * turns), in stubs from `alloc` (executable memory within rel32 range of the image). */
void runtime_mouselook_install_patches(unsigned char *(*alloc)(size_t size));
/* BB_MOUSELOOK=0: no direct turns, the mouse is the right stick everywhere. */
int runtime_mouselook_enabled(void);
/* For the pad read (runtime_pad.c): camera hook takes so far (the mouse turns cameras directly only
 * while a hook took it at every recent pad read), and whether the hooks get the mouse (0: the pad
 * gives it to the right stick, the hooks drop it). */
unsigned runtime_mouselook_calls(void);
void runtime_mouselook_pause(int paused);
/* BB_MOUSE_TRACE=<file>: one line per pad read (direct, the stick values it gave the mouse). */
void runtime_mouselook_trace_pad(int direct, float stick_x, float stick_y);

/* runtime_pad.c */
int runtime_pad_aim_held(void);
void runtime_pad_mouse_settings(float *sensitivity, float *aim, float *vertical, int *invert_y);
/* Scripted runs (BB_PAD_FILE): 1 and the scripted mouse rate (mx= my=, counts per second). */
int runtime_pad_scripted_mouse(float *rate_x, float *rate_y);
