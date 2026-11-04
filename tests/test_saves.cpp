// SPDX-License-Identifier: GPL-2.0-or-later
// tests/test_saves.cpp: the overlay's save code (gpu/shim/bbport_saves.cpp) against real Uncharted 3 saves set
// to known checkpoints, in a throwaway save folder (its path has a space and an accented letter). The game's
// side is stubbed: where the saves are, whether a save is open, the restart (recorded; the test then runs the
// next start's restore itself) and the checkpoint jump flags. After every step the player's profile save is
// compared byte for byte with the original: nothing here may ever change it.
//
//   test_saves <fixtures folder> <scratch folder>
// The fixtures folder holds u3_<checkpoint>/SAVEDATA030(.sce_sys) for 12 16 18 20 22 26 27 28 45 and the profile
// SAVEDATA03P(.sce_sys) (made with savegoto.py from a recorded save).
#include "bbport_saves.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

// ---- the game's side -------------------------------------------------------------------------------------------
static std::string saves_dir;
static int mounted = 0, restarts = 0, practice = 0;
extern "C" {
int runtime_savedata_dir(char* out, size_t size) {
    std::snprintf(out, size, "%s", saves_dir.c_str());
    return 0;
}
int runtime_savedata_mounted(void) { return mounted; }
void runtime_restart(void) { ++restarts; }
void runtime_checkpoint_request(uint32_t) {}
int runtime_checkpoint_practice(void) { return practice; }
void runtime_checkpoint_set_practice(int on) { practice = on; }
int runtime_checkpoint_result(void) { return 0; }
int runtime_checkpoint_available(void) { return 1; }
void bbport_saves_apply_pending(void);
void bbport_saves_closed(const char* host_dir);
}

// ---- helpers ---------------------------------------------------------------------------------------------------
static int failures = 0, checks = 0;
static fs::path fixtures, root, saves, backups;

static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

static std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// Two folders hold the same files with the same bytes (recursively).
static bool same_tree(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::is_directory(a, ec) || !fs::is_directory(b, ec)) {
        return false;
    }
    size_t na = 0, nb = 0;
    for (const auto& e : fs::recursive_directory_iterator(a)) {
        if (!e.is_regular_file()) {
            continue;
        }
        ++na;
        const fs::path other = b / fs::relative(e.path(), a);
        if (!fs::is_regular_file(other, ec) || read_file(e.path()) != read_file(other)) {
            return false;
        }
    }
    for (const auto& e : fs::recursive_directory_iterator(b)) {
        nb += e.is_regular_file();
    }
    return na == nb;
}

// A campaign save (both folders) as the game holds it: SAVEDATA030 and its .sce_sys.
static bool save_is(const fs::path& folder, int checkpoint) {
    const fs::path f = fixtures / ("u3_" + std::to_string(checkpoint));
    return same_tree(folder / "SAVEDATA030", f / "SAVEDATA030") &&
           same_tree(folder / "SAVEDATA030.sce_sys", f / "SAVEDATA030.sce_sys");
}

// The game wrote its campaign save at this checkpoint (and closed it).
static void play_to(int checkpoint, bool close = true) {
    const fs::path f = fixtures / ("u3_" + std::to_string(checkpoint));
    for (const char* d : {"SAVEDATA030", "SAVEDATA030.sce_sys"}) {
        fs::remove_all(saves / d);
        fs::copy(f / d, saves / d, fs::copy_options::recursive);
    }
    if (close) {
        bbport_saves_closed(u8(saves / "SAVEDATA030").c_str());
    }
}

static bool profile_untouched() {
    return same_tree(saves / "SAVEDATA03P", fixtures / "SAVEDATA03P") &&
           same_tree(saves / "SAVEDATA03P.sce_sys", fixtures / "SAVEDATA03P.sce_sys");
}

static fs::path slot(const char* name) { return backups / "u3" / name; }

static std::string story_mode() {
    std::ifstream f(backups / "u3" / "story-state.txt");
    std::string task, mode;
    f >> task >> mode;
    return mode;
}

// The next start of the game: the restore of a requested Load happens before the first save mount.
static void next_start() { bbport_saves_apply_pending(); }

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: test_saves <fixtures> <scratch>\n");
        return 2;
    }
    _putenv("BB_EXE=big3-ps4_Shipping.elf");
    fixtures = fs::u8path(argv[1]);
    root = fs::u8path(argv[2]) / fs::u8path("user caf\xC3\xA9 saves");
    fs::remove_all(root);
    saves = root / "savedata" / "1" / "CUSA02343";
    backups = root / "savedata" / "1" / "CUSA02343.backups";
    saves_dir = u8(saves);
    fs::create_directories(saves);
    // The player's folder: the U3 campaign save at checkpoint 16, the U3 profile, a U1 save, and things that are
    // not saves (an old backup folder, a text file) that the one-time backup must leave out.
    play_to(16, false);
    for (const char* d : {"SAVEDATA03P", "SAVEDATA03P.sce_sys"}) {
        fs::copy(fixtures / d, saves / d, fs::copy_options::recursive);
    }
    fs::copy(fixtures / "u3_28" / "SAVEDATA030", saves / "SAVEDATA010", fs::copy_options::recursive);
    fs::create_directories(saves / "bak-old");
    std::ofstream(saves / "notes.txt") << "not a save\n";

    // 1. The first start: every save backed up once, the story save started from the campaign save.
    next_start();
    const fs::path first = backups / "before-save-slots";
    check(same_tree(first / "SAVEDATA030", saves / "SAVEDATA030"), "1 backup: campaign save copied exactly");
    check(same_tree(first / "SAVEDATA03P", saves / "SAVEDATA03P"), "1 backup: profile save copied exactly");
    check(same_tree(first / "SAVEDATA03P.sce_sys", saves / "SAVEDATA03P.sce_sys"), "1 backup: profile .sce_sys");
    check(same_tree(first / "SAVEDATA010", saves / "SAVEDATA010"), "1 backup: another game's save copied");
    check(!fs::exists(first / "bak-old") && !fs::exists(first / "notes.txt"), "1 backup: only save folders");
    check(save_is(slot("story"), 16) && story_mode() == "following", "1 story save starts at the campaign save");
    // A second start (or a changed save) never touches the backup.
    play_to(18, false);
    next_start();
    check(save_is(first, 16), "1 backup: not made again on the next start");
    check(profile_untouched(), "1 profile untouched");

    // 2. Playing on: the story save follows.
    play_to(18);
    check(save_is(slot("story"), 18) && story_mode() == "following", "2 story follows 16 -> 18");
    play_to(20);
    check(save_is(slot("story"), 20) && story_mode() == "following", "2 story follows 18 -> 20");

    // 3. A jump back (Chapter Select, a new game): the story save stays, and stays after playing on from there.
    play_to(12);
    check(save_is(slot("story"), 20) && story_mode() == "away", "3 jump back: story stays at 20, away");
    play_to(16);
    check(save_is(slot("story"), 20) && story_mode() == "away", "3 playing on after a jump back: still away");
    check(save_is(saves, 16), "3 the game's own save is what the game wrote");

    // 4. Make this my story save.
    std::string message;
    check(BbSaves::MakeStory(message), "4 make story: " + message);
    check(save_is(slot("story"), 16) && story_mode() == "following", "4 make story: story at 16, following");

    // 5. Chapter ends: from the middle of a chapter to the next chapter is a skip; from its last checkpoints, play.
    play_to(22);
    check(save_is(slot("story"), 22), "5 story follows to 22");
    play_to(28);
    check(save_is(slot("story"), 22) && story_mode() == "away", "5 mid-chapter (22) to next chapter (28): away");
    play_to(26, false);
    check(BbSaves::MakeStory(message), "5 make story at 26");
    play_to(28);
    check(save_is(slot("story"), 28) && story_mode() == "following", "5 from 26 (2nd last of ch 2) to 28: follows");

    // 6. A skip ahead (Chapter Select forward): away.
    play_to(45);
    check(save_is(slot("story"), 28) && story_mode() == "away", "6 skip ahead 28 -> 45: story stays at 28");
    check(profile_untouched(), "6 profile untouched");

    // 7. Slots: save into one; not while the game is saving.
    check(BbSaves::Store(0, message) && save_is(slot("slot1"), 45), "7 slot 1 holds the campaign save (45)");
    mounted = 1;
    check(!BbSaves::Store(1, message) && !fs::exists(slot("slot2")), "7 no save into a slot while the game saves");
    check(!BbSaves::Load(0, message), "7 no load while the game saves");
    mounted = 0;

    // 8. Load a slot: the save from before goes to "before the last load", the restart restores the slot.
    play_to(12, false);
    BbSaves::Refresh();
    int before = restarts;
    check(BbSaves::Load(0, message) && restarts == before + 1, "8 load slot 1 restarts the game");
    check(save_is(slot("before-load"), 12), "8 the save from before (12) kept as before-load");
    check(save_is(saves, 12), "8 nothing restored before the restart");
    next_start();
    check(save_is(saves, 45), "8 after the restart the campaign save is slot 1's (45)");
    check(!fs::exists(backups / "pending-load.txt"), "8 the request is done once");
    check(save_is(slot("slot1"), 45), "8 slot 1 itself unchanged");
    check(profile_untouched(), "8 profile untouched");
    next_start();
    check(save_is(saves, 45), "8 a further start restores nothing again");

    // 9. Load the story save: it follows again.
    BbSaves::Refresh();
    check(BbSaves::Load(BbSaves::StorySlot, message), "9 load story save");
    next_start();
    check(save_is(saves, 28) && story_mode() == "following", "9 campaign save = story (28), following again");
    play_to(28);
    play_to(28);
    check(save_is(slot("story"), 28) && story_mode() == "following", "9 the same checkpoint saved again: follows");

    // 10. A request for another game of the collection waits for that game.
    std::ofstream(backups / "pending-load.txt") << "u2 slot1\n";
    next_start();
    check(save_is(saves, 28) && fs::exists(backups / "pending-load.txt"), "10 u2's request left for U2");
    fs::remove(backups / "pending-load.txt");

    // 11. Empty slot, unknown slot index.
    BbSaves::Refresh();
    check(!BbSaves::Load(3, message), "11 an empty slot does not load");
    check(!BbSaves::Load(42, message) && !BbSaves::Store(-1, message), "11 bad slot numbers refused");

    // 12. Practice mode: the game saves to the practice copy; the story and the player's save never see it.
    check(BbSaves::StartPractice(message) && practice == 1, "12 practice starts");
    check(save_is(slot("practice"), 28), "12 the practice copy starts as the campaign save");
    const fs::path practice_save = slot("practice") / "SAVEDATA030";
    for (const char* d : {"SAVEDATA030", "SAVEDATA030.sce_sys"}) {  // the game's autosave after a jump to 45
        fs::remove_all(slot("practice") / d);
        fs::copy(fixtures / "u3_45" / d, slot("practice") / d, fs::copy_options::recursive);
    }
    bbport_saves_closed(u8(practice_save).c_str());
    check(save_is(slot("story"), 28) && story_mode() == "following", "12 practice saves do not move the story");
    check(save_is(saves, 28), "12 the player's own save untouched by practice");

    // 13. Keep this progress: the practice save becomes the campaign save and the story save.
    before = restarts;
    check(BbSaves::KeepPractice(message) && restarts == before + 1, "13 keep this progress restarts");
    check(save_is(slot("before-load"), 28), "13 the save from before kept as before-load");
    practice = 0;  // a new process
    next_start();
    check(save_is(saves, 45), "13 campaign save = the practice save (45)");
    check(save_is(slot("story"), 45) && story_mode() == "following", "13 story save = 45, following");
    check(!fs::exists(slot("practice")), "13 the practice copy is cleared");
    check(!BbSaves::KeepPractice(message), "13 nothing to keep outside practice mode");
    check(profile_untouched(), "13 profile untouched");

    // 14. A damaged save: no crash, the story save does not move.
    std::ofstream(saves / "SAVEDATA030" / "USR-DATA", std::ios::binary | std::ios::trunc) << "garbage";
    bbport_saves_closed(u8(saves / "SAVEDATA030").c_str());
    check(save_is(slot("story"), 45), "14 a damaged save leaves the story save alone");
    BbSaves::Refresh();
    check(BbSaves::Describe(BbSaves::GetSlot(BbSaves::StorySlot).checkpoint).find("45") != std::string::npos,
          "14 story slot still reads as checkpoint 45");
    play_to(45, false);

    // 15. A broken slot copy (its .sce_sys gone): the load fails and the campaign save stays exactly as it was.
    fs::remove_all(slot("slot1") / "SAVEDATA030.sce_sys");
    play_to(20, false);
    BbSaves::Refresh();
    check(BbSaves::Load(0, message), "15 load of a broken slot is requested");
    next_start();
    check(save_is(saves, 20), "15 a failed restore leaves the campaign save as it was");
    check(!fs::exists(saves / "SAVEDATA030.bbload") && !fs::exists(saves / "SAVEDATA030.sce_sys.bbload") &&
              !fs::exists(saves / "SAVEDATA030.bbold") && !fs::exists(saves / "SAVEDATA030.sce_sys.bbold"),
          "15 no half-copied or moved-aside folders left");
    // ...and a good load afterwards still works, leaving nothing behind either.
    BbSaves::Store(4, message);
    play_to(12, false);
    BbSaves::Refresh();
    BbSaves::Load(4, message);
    next_start();
    check(save_is(saves, 20), "15 a good load after a failed one restores slot 5 (20)");
    size_t leftovers = 0;
    for (const auto& e : fs::directory_iterator(saves)) {
        const std::string n = u8(e.path().filename());
        leftovers += n.find(".bbload") != std::string::npos || n.find(".bbold") != std::string::npos;
    }
    check(leftovers == 0, "15 no temporary folders after a good load");
    check(profile_untouched(), "15 profile untouched");

    // 16. The one-time backup when it cannot be made: retried on the next start.
    const fs::path other = root / "savedata" / "1" / "CUSA02320";
    fs::create_directories(other);
    fs::copy(fixtures / "u3_16" / "SAVEDATA030", other / "SAVEDATA030", fs::copy_options::recursive);
    fs::copy(fixtures / "u3_16" / "SAVEDATA030.sce_sys", other / "SAVEDATA030.sce_sys", fs::copy_options::recursive);
    std::ofstream(root / "savedata" / "1" / "CUSA02320.backups") << "a file where the backup folder goes";
    saves_dir = u8(other);
    next_start();
    check(!fs::is_directory(root / "savedata" / "1" / "CUSA02320.backups"), "16 backup could not be made");
    check(same_tree(other / "SAVEDATA030", fixtures / "u3_16" / "SAVEDATA030"), "16 the save itself untouched");
    fs::remove(root / "savedata" / "1" / "CUSA02320.backups");
    next_start();
    check(same_tree(root / "savedata" / "1" / "CUSA02320.backups" / "before-save-slots" / "SAVEDATA030",
                    fixtures / "u3_16" / "SAVEDATA030"),
          "16 made on the next start");

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
