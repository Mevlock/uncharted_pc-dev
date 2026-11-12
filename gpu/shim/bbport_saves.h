// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay's Saves and Checkpoints pages: the game's campaign saves (your games), the Undo copy, the current
// checkpoint, practice mode (bbport_saves.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BbSaves {

// The older save slots' folders (slot1..5, before-load, story; no longer on a page): a Load requested by an older
// build still puts one back at start-up, and the story save follows when BB_STORY_SAVES=1.
constexpr int SlotCount = 5;
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

// One of your games (a campaign save, SAVEDATA0<game><digit>), or the Undo copy of one.
struct GameSave {
    int digit = -1;        // 0-9: "Game <digit + 1>"
    bool playing = false;  // the one the game plays on
    Checkpoint checkpoint;
    std::string date;  // when it was last saved (the Undo copy: when it was kept)
};

// 1, 2 or 3 (Uncharted 1/2/3), 0 when the running game is unknown.
int Game();
// Where the running game's campaign save is now (its last autosave), re-read when the file changes.
Checkpoint Current();
// Your games, as in the game's own lists (re-read from disk), and the Undo copy: a game as it was before it was saved
// over (false when there is none).
std::vector<GameSave> Games();
bool UndoSave(GameSave& out);
// Restarts the game into one of your games: before the game opens its saves it is made the newest, so Continue
// starts there.
bool LoadGame(int digit, std::string& message);
// Moves one of your games to savedata-deleted (not the one in play; never erased).
bool DeleteGame(int digit, std::string& message);
// Restarts the game with the Undo copy put back (the save it replaces becomes the next Undo); Continue starts there.
bool LoadUndo(std::string& message);
// Moves the Undo copy to the deleted folder (never erased).
bool DeleteUndo(std::string& message);
// The running game's playable checkpoints in game order.
std::vector<Checkpoint> Checkpoints();
// The running game's task jump is hooked (Uncharted 3, EU and 1.02 code).
bool JumpsAvailable();
// Practice mode is on: the game saves to a practice copy until it restarts.
bool Practising();
// Practice mode on: the campaign save copied to the practice folder, where the game saves until it restarts.
bool StartPractice(std::string& message);
// Jumps the running game to a checkpoint at its next frame. The first jump turns practice mode on.
bool Jump(const Checkpoint& target, std::string& message);
// Ends practice mode keeping its progress: the practice save becomes a new game of its own (the next free campaign
// save) and the game restarts with Continue there; the other games stay as they are.
bool KeepPractice(std::string& message);
// The outcome of the last jump once the game ran it: 1 done, -1 no such task, -2 not supported, 0 nothing new.
int JumpResult();
// "Checkpoint 57 · valve-room-entrance · Chapter 4 Run to Ground", in the menu's language (the task name and the
// chapter title stay as the game names them); `translated` false: English, for the log.
std::string Describe(const Checkpoint& c, bool translated = true);

} // namespace BbSaves
