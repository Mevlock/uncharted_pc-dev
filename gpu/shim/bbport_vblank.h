// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the guest's vblank on the display's own refresh (GitHub #26). The present thread's vblank is a timer at
// BB_VBLANK_HZ; a display that refreshes at nearly that rate (a "60 Hz" TV at 59.94 Hz, or 60 Hz on a clock that
// drifts a little from the PC's) slowly slides past it. With FIFO the presents hold the game back to the display;
// with Mailbox nothing does, and while the two line up frames are dropped and others shown twice (steady 60 FPS,
// visible lag, gone again after a while). Here the vblank waits for the display's real vblank instead (DXGI
// WaitForVBlank on the monitor the window is on), when a measurement shows the display refreshes within 2% of the
// vblank rate; anything else (a faster display, VRR showing another rate, no answer) keeps the timer.
// BB_VBLANK_SYNC=0: always the timer.
#pragma once

#include <chrono>

namespace BbVblank {

/// Present thread, once: measures the display's refresh and decides. True: Wait() paces the vblank.
bool Start(std::chrono::nanoseconds vblank_period);
/// Present thread, each tick when Start returned true: until the display's next vblank. False when the display gave
/// no vblank in time (asleep, disconnected, moved to another monitor that does not match): the caller waits the
/// timer for this tick; the display is looked up again on a later tick.
bool Wait();

} // namespace BbVblank
