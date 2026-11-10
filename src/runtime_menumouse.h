/* Mouse in the game menus (src/runtime_menumouse.c, by antikmewzik): hover, click, wheel, sliders, lists, back. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"

/* Which game and code version this is (every site and called function checked against the original bytes). */
void runtime_menumouse_prepare(unsigned char *image, uint64_t image_size);
/* The sites of the version found (probe.c installs them). */
const RuntimeGuestCallback *runtime_menumouse_callbacks(size_t *count);
/* 1 while a game menu is on screen (the cursor is free, the mouse buttons and wheel go to the menu). */
int runtime_menumouse_active(void);
/* The pad buttons the mouse presses in the menu now (keys: SDL keyboard state; mouse: 1 left 2 right 4 middle
 * 8 X1 16 X2; wheel: notches since the last read). scripted: BB_PAD_FILE gives the cursor (sx, sy: 0..1 of the
 * 16:9 menu picture). */
uint32_t runtime_menumouse_buttons(const bool *keys, uint32_t mouse, int wheel, int scripted, float sx, float sy);
