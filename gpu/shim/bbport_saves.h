// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay's save slots, story save and current checkpoint (bbport_saves.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace BbSaves {

constexpr int SlotCount = 5;
// Slot indexes after the five manual ones: the copy taken before each Load, and the story save.
constexpr int UndoSlot = SlotCount;
constexpr int StorySlot = SlotCount + 1;

struct Checkpoint {
    bool known = false;
    uint32_t task = 0;
    int row = -1;    // place in the game's list of tasks (game order)
    int number = 0;  // the game's playable checkpoint number, 0 for a cutscene or setup step
    int chapter = 0;
    std::string chapter_title, name;
};

struct Slot {
    bool used = false;
    std::string progress;  // the game's own text, "Campaign 37% Complete - Run to Ground"
    Checkpoint checkpoint;
    std::string date;  // when it was saved into the slot
};

// 1, 2 or 3 (Uncharted 1/2/3), 0 when the running game is unknown.
int Game();
// Where the running game's campaign save is now (its last autosave), re-read when the file changes.
Checkpoint Current();
// The slots, re-read from disk.
void Refresh();
const Slot& GetSlot(int index);
// True when the campaign save has left the story save: a jump back or ahead (Chapter Select, a new game)
// rather than playing on from it. The story save then stays where it was until it is loaded again or
// MakeStory moves it.
bool AwayFromStory();
// Copies the campaign save into a slot (replacing what it held). Fails while the game is writing a save.
bool Store(int index, std::string& message);
// The current campaign save becomes the story save (the player keeps a skip ahead on purpose).
bool MakeStory(std::string& message);
// Puts a slot back: copies the current save to the undo slot, then restarts the game, which restores the slot
// before it opens its saves. Continue then starts there.
bool Load(int index, std::string& message);
// "Checkpoint 57 · valve-room-entrance · Chapter 4 Run to Ground"
std::string Describe(const Checkpoint& c);

} // namespace BbSaves
