// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the guest's vblank steered by the display's own refresh (GitHub #26). The present thread's vblank is a
// timer at BB_VBLANK_HZ; a display that refreshes at nearly that rate (a "60 Hz" TV at 59.94 Hz, or 60 Hz on a clock
// that drifts a little from the PC's) slowly slides past it. With FIFO the presents hold the game back to the
// display; with Mailbox nothing does, and while the two line up frames are dropped and others shown twice (steady
// 60 FPS, visible judder, gone again after a while, back later).
// A thread of its own here waits for the display's real vblanks (DXGI WaitForVBlank on the monitor the window is
// on) and measures them. When the display refreshes within 2% of the vblank rate, each tick of the timer is
// lengthened or shortened a little (Correction) so it runs at the display's measured rate and stays just after its
// vblank: the drift cannot build up, and the timer keeps its slack (a late tick is made up on the next one; waiting
// for the real vblank itself instead showed a frame twice whenever a tick ran late). Anything else (a faster
// display, VRR at another rate, no vblanks) leaves the timer as it is. BB_VBLANK_SYNC=0: off.
#pragma once

#include <chrono>

namespace BbVblank {

/// Present thread, once: starts watching the display. False when off (BB_VBLANK_SYNC=0, not Windows).
bool Start(std::chrono::nanoseconds vblank_period);
/// Present thread, at the start of each tick: how much longer (positive) or shorter the next interval should be to
/// follow the display; zero while the display does not match.
std::chrono::nanoseconds Correction();

} // namespace BbVblank
