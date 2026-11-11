// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay's save slots for the campaign save (SAVEDATA0<game>0, the save Continue loads), kept beside the
// saves in <title>.backups/u<game>/:
//   slot1..slot5   copies the player makes (Save here) and puts back (Load)
//   before-load    the campaign save as it was before the last Load
//   story          the story save: the campaign save as reached by playing on. Each time the game closes its
//                  campaign save, the new checkpoint is compared with the story save's: the next checkpoints of
//                  the same chapter, or the start of the next chapter from the end of the last one, move the
//                  story save along; anything else (Chapter Select back or ahead, a new game) leaves it where
//                  it is, and only loading it again or "Make this my story save" lets it follow again.
// Loading puts a copy back while the game is not running: the game restarts and the restore happens before its
// first save mount (runtime_savedata.c). The profile save (SAVEDATA0<game>P: treasures, unlocks) is never touched.
//
// The checkpoint comes from the campaign save itself: its body is Blowfish-encrypted (32-bit words little-endian)
// after a 12-byte header, and the decrypted data holds the task (checkpoint) id at 0x9a8.
#include "bbport_saves.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <openssl/blowfish.h>

#include "bbport_i18n.h"
#include "bbport_thumbnail.h"
#include "video_core/renderer_vulkan/vk_vram.h"

extern "C" {
int runtime_savedata_dir(char* out, size_t size);
int runtime_savedata_mounted(void);
void runtime_restart(void);
void runtime_checkpoint_request(uint32_t task);
int runtime_checkpoint_practice(void);
void runtime_checkpoint_set_practice(int on);
int runtime_checkpoint_result(void);
int runtime_checkpoint_available(void);
}

namespace fs = std::filesystem;
using BbI18n::T;

namespace {

struct TaskRow {
    int game;
    uint32_t task;
    int number, chapter;
    const char* chapter_title;
    const char* name;
};
const TaskRow tasks[] = {
#include "bbport_checkpoints.inc"
};

const unsigned char blowfish_key[20] = {0x28, 0x53, 0x48, 0x5b, 0x40, 0x32, 0x3e, 0x72, 0x36, 0x32,
                                        0x25, 0x35, 0x2b, 0x51, 0x4b, 0x70, 0x79, 0x7c, 0x67, 0x36};
constexpr size_t task_offset = 0x9a8;  // in the decrypted body
constexpr size_t header_size = 0xc;
// The story save follows into the next chapter only from its last checkpoints (Chapter Select from the middle of
// a chapter to the next one is a skip).
constexpr int chapter_end_checkpoints = 2;

std::recursive_mutex lock;
BbSaves::Slot slots[BbSaves::StorySlot + 1];
BbSaves::Checkpoint current;
fs::file_time_type current_time{};
std::chrono::steady_clock::time_point current_checked{};

// A path as UTF-8 text (C++20's u8string() is a std::u8string).
std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
fs::path SavesDir() {
    char dir[700];
    runtime_savedata_dir(dir, sizeof(dir));
    return fs::u8path(dir);
}
// The name a save has inside a slot folder: always the first campaign save's (SAVEDATA0<game>0), whichever
// playthrough it was copied from, so slots made before several playthroughs read the same.
std::string SlotName(int game) {
    return "SAVEDATA0" + std::to_string(game) + "0";
}
fs::path GameBackups(int game) {
    fs::path saves = SavesDir();
    return saves.parent_path() / (Utf8(saves.filename()) + ".backups") / ("u" + std::to_string(game));
}

// Playthroughs: the games keep several campaign saves, as on the console (SAVEDATA0<game>0 to 9; "New save" in the
// game's list makes the next). The one the game plays on is the one it opened last (bbport_saves_before_mount),
// kept in active.txt across starts; the slots, the undo copy and Load work on it, and each playthrough has its own
// story save. A campaign save's last digit is its playthrough.
int CampaignDigit(int game, const std::string& dir) {
    const std::string prefix = "SAVEDATA0" + std::to_string(game);
    if (dir.size() != prefix.size() + 1 || dir.compare(0, prefix.size(), prefix) != 0) {
        return -1;
    }
    const char c = dir.back();
    return c >= '0' && c <= '9' ? c - '0' : -1;
}
std::string CampaignName(int game, int digit) {
    return "SAVEDATA0" + std::to_string(game) + std::to_string(digit);
}
fs::path ActiveFile(int game) {
    return GameBackups(game) / "active.txt";
}
int active_digit = -1; // read from active.txt once, then kept here; guarded by `lock`
int ActiveDigit(int game) {
    if (active_digit >= 0) {
        return active_digit;
    }
    std::ifstream f(ActiveFile(game));
    int d = -1;
    if (f >> d && d >= 0 && d <= 9) {
        return active_digit = d;
    }
    // Not recorded yet: the newest campaign save (the one Continue loads), else the first.
    std::error_code ec;
    fs::file_time_type newest{};
    d = 0;
    for (int i = 0; i <= 9; ++i) {
        const auto t = fs::last_write_time(SavesDir() / CampaignName(game, i) / "USR-DATA", ec);
        if (!ec && t > newest) {
            newest = t;
            d = i;
        }
    }
    return active_digit = d;
}
void SetActiveDigit(int game, int digit) {
    if (digit < 0 || digit > 9 || digit == active_digit) {
        return;
    }
    active_digit = digit;
    std::error_code ec;
    fs::create_directories(GameBackups(game), ec);
    std::ofstream(ActiveFile(game)) << digit << "\n";
    std::printf("Save slots: the game plays on %s\n", CampaignName(game, digit).c_str());
}
// The campaign save the game plays on.
std::string ActiveName(int game) {
    return CampaignName(game, ActiveDigit(game));
}
// The story save of a playthrough: "story" for the first (as before several playthroughs), "story-<n>" else.
std::string StoryFolder(int digit) {
    return digit <= 0 ? std::string("story") : "story-" + std::to_string(digit);
}
std::string SlotFolder(int index) {
    return index == BbSaves::UndoSlot    ? std::string("before-load")
           : index == BbSaves::StorySlot ? StoryFolder(active_digit)
                                         : "slot" + std::to_string(index + 1);
}
fs::path SlotDir(int game, int index) {
    if (index == BbSaves::StorySlot) {
        return GameBackups(game) / StoryFolder(ActiveDigit(game));
    }
    return GameBackups(game) / SlotFolder(index);
}
fs::path PendingFile(int game) {
    return GameBackups(game).parent_path() / "pending-load.txt";
}
fs::path StoryState(int game, int digit) {
    return GameBackups(game) / (digit <= 0 ? std::string("story-state.txt")
                                           : "story-state-" + std::to_string(digit) + ".txt");
}
fs::path StoryState(int game) {
    return StoryState(game, ActiveDigit(game));
}

BbSaves::Checkpoint Lookup(int game, uint32_t task) {
    BbSaves::Checkpoint c;
    c.task = task;
    int row = 0;
    for (const TaskRow& t : tasks) {
        if (t.game != game) {
            continue;
        }
        if (t.task == task) {
            c.known = true;
            c.row = row;
            c.number = t.number;
            c.chapter = t.chapter;
            c.chapter_title = t.chapter_title;
            c.name = t.name;
            break;
        }
        ++row;
    }
    return c;
}

// The checkpoint a campaign save is at: the one decrypted 8-byte block that holds the task id.
BbSaves::Checkpoint ReadCheckpoint(int game, const fs::path& slot) {
    std::ifstream f(slot / "USR-DATA", std::ios::binary);
    unsigned char block[8];
    if (!f.seekg(header_size + task_offset) || !f.read(reinterpret_cast<char*>(block), sizeof(block))) {
        return {};
    }
    BF_KEY key;
    BF_set_key(&key, sizeof(blowfish_key), blowfish_key);
    // Blowfish works on big-endian words, the save stores little-endian ones: each word byte-swapped going in,
    // and the decrypted little-endian word read back as big-endian.
    unsigned char in[8], out[8];
    for (int i = 0; i < 8; ++i) {
        in[i] = block[(i & ~3) + 3 - (i & 3)];
    }
    BF_ecb_encrypt(in, out, &key, BF_DECRYPT);
    const uint32_t task = uint32_t(out[0]) << 24 | uint32_t(out[1]) << 16 | uint32_t(out[2]) << 8 | out[3];
    return Lookup(game, task);
}

// Playing on from `from` reaches `to`: a later checkpoint of the same chapter, or the next chapter when `from`
// was among the last checkpoints of its chapter.
bool PlaysOn(int game, const BbSaves::Checkpoint& from, const BbSaves::Checkpoint& to) {
    if (!from.known || !to.known || to.row <= from.row) {
        return false;
    }
    if (to.chapter == from.chapter) {
        return true;
    }
    if (to.chapter != from.chapter + 1) {
        return false;
    }
    int left = 0, row = 0;
    for (const TaskRow& t : tasks) {
        if (t.game != game) {
            continue;
        }
        if (row++ > from.row && t.chapter == from.chapter && t.number) {
            ++left;
        }
    }
    return left <= chapter_end_checkpoints;
}

// The game's own progress text (param.bin's subtitle, 128 bytes at 128).
std::string ReadProgress(const fs::path& meta) {
    std::ifstream f(meta / "param.bin", std::ios::binary);
    char text[129] = {};
    if (!f.seekg(128) || !f.read(text, 128)) {
        return {};
    }
    return text;
}

std::string Now() {
    std::time_t t = std::time(nullptr);
    char text[64];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", std::localtime(&t));
    return text;
}

// `to` replaced by a copy of the two folders of a save (the save and its .sce_sys) found in `from`: copied next
// to it first, so a failed copy leaves `to` as it was.
bool CopySave(const fs::path& from, const fs::path& to, const std::string& name, std::string& error,
              const std::string& to_name = {}) {
    std::error_code ec;
    const fs::path temp = to.parent_path() / (Utf8(to.filename()) + ".new");
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    const std::string out = to_name.empty() ? name : to_name;  // the copy's name (a playthrough into a slot)
    for (const std::string& suffix : {std::string(), std::string(".sce_sys")}) {
        fs::copy(from / (name + suffix), temp / (out + suffix), fs::copy_options::recursive, ec);
        if (ec) {
            error = ec.message();
            fs::remove_all(temp, ec);
            return false;
        }
    }
    const fs::path old = to.parent_path() / (Utf8(to.filename()) + ".old");
    fs::remove_all(old, ec);
    if (fs::exists(to)) {
        fs::rename(to, old, ec);
        if (ec) {
            error = ec.message();
            fs::remove_all(temp, ec);
            return false;
        }
    }
    fs::rename(temp, to, ec);
    if (ec) {
        error = ec.message();
        std::error_code ignore;
        fs::rename(old, to, ignore);
        return false;
    }
    fs::remove_all(old, ec);
    return true;
}

// story-state.txt: "<task id of the story save> following|away"
struct StoryStatus {
    bool exists = false;
    bool following = true;
};
StoryStatus ReadStory(int game) {
    StoryStatus s;
    std::ifstream f(StoryState(game));
    std::string task, mode;
    if (f >> task >> mode) {
        s.exists = std::filesystem::exists(SlotDir(game, BbSaves::StorySlot) / SlotName(game) / "USR-DATA");
        s.following = mode != "away";
    }
    return s;
}
void WriteStory(int game, uint32_t task, bool following) {
    std::error_code ec;
    fs::create_directories(GameBackups(game), ec);
    char id[16];
    std::snprintf(id, sizeof(id), "%08x", task);
    std::ofstream(StoryState(game)) << id << " " << (following ? "following" : "away") << "\n";
}

// The campaign save the game plays on (or, from `from`, another folder holding it: the practice copy) copied into a
// slot folder under the slot's name, with the time.
bool CopyCampaign(int game, int index, std::string& error, const fs::path& from = {}) {
    const fs::path to = SlotDir(game, index);
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!CopySave(from.empty() ? SavesDir() : from, to, ActiveName(game), error, SlotName(game))) {
        return false;
    }
    std::ofstream(to / "saved.txt") << Now() << "\n";
    return true;
}

} // namespace

namespace BbSaves {

int Game() {
    static const int game = [] {
        const char* exe = std::getenv("BB_EXE");
        if (!exe) {
            return 0;
        }
        if (std::strstr(exe, "big3")) {
            return 3;
        }
        if (std::strstr(exe, "big2")) {
            return 2;
        }
        return std::strcmp(exe, "eboot.bin") == 0 ? 1 : 0;
    }();
    return game;
}

Checkpoint Current() {
    std::lock_guard guard(lock);
    const auto now = std::chrono::steady_clock::now();
    if (Game() && now - current_checked > std::chrono::seconds(1)) {
        current_checked = now;
        std::error_code ec;
        // In practice mode the game saves to the practice copy: "Now" follows it.
        const fs::path file = (runtime_checkpoint_practice() ? GameBackups(Game()) / "practice" : SavesDir()) /
                              ActiveName(Game()) / "USR-DATA";
        const auto time = fs::last_write_time(file, ec);
        if (!ec && time != current_time && !runtime_savedata_mounted()) {
            current_time = time;
            current = ReadCheckpoint(Game(), file.parent_path());
        }
    }
    return current;
}

void Refresh() {
    std::lock_guard guard(lock);
    const int game = Game();
    for (int i = 0; i <= StorySlot; ++i) {
        Slot s;
        const fs::path dir = SlotDir(game, i);
        std::error_code ec;
        if (game && fs::exists(dir / SlotName(game) / "USR-DATA", ec)) {
            s.used = true;
            s.progress = ReadProgress(dir / (SlotName(game) + ".sce_sys"));
            s.checkpoint = ReadCheckpoint(game, dir / SlotName(game));
            std::ifstream info(dir / "saved.txt");
            std::getline(info, s.date);
        }
        slots[i] = s;
    }
}

const Slot& GetSlot(int index) {
    return slots[index];
}

bool AwayFromStory() {
    std::lock_guard guard(lock);
    const StoryStatus s = ReadStory(Game());
    return s.exists && !s.following;
}

bool Store(int index, std::string& message) {
    std::lock_guard guard(lock);
    const int game = Game();
    if (!game || index < 0 || index > StorySlot) {
        message = T("Save slots are only for Uncharted 1, 2 and 3.");
        return false;
    }
    if (runtime_savedata_mounted()) {
        message = T("The game is saving: try again in a moment.");
        return false;
    }
    // In practice mode the save the page shows as "Now" is the practice copy: a slot or the story save takes that.
    // The undo copy (before a Load or Keep this progress) is always the player's own campaign save.
    const bool practice = index != UndoSlot && runtime_checkpoint_practice();
    const fs::path source = practice ? GameBackups(game) / "practice" : SavesDir();
    std::error_code ec;
    if (!fs::exists(source / ActiveName(game) / "USR-DATA", ec)) {
        message = practice ? T("Nothing to save yet: the game has not saved since the jump.")
                           : T("There is no campaign save yet.");
        return false;
    }
    std::string error;
    if (!CopyCampaign(game, index, error, source)) {
        message = fmt::format(fmt::runtime(T("Could not copy the save: {}")), error);
        return false;
    }
    std::printf("Save slots: %s copied to %s\n", practice ? "practice save" : "campaign save",
                Utf8(SlotDir(game, index)).c_str());
    Refresh();
    message = index < SlotCount ? fmt::format(fmt::runtime(T("Saved in slot {}.")), index + 1) : std::string(T("Saved."));
    return true;
}

bool MakeStory(std::string& message) {
    std::lock_guard guard(lock);
    if (!Store(StorySlot, message)) {
        return false;
    }
    WriteStory(Game(), ReadCheckpoint(Game(), SlotDir(Game(), StorySlot) / SlotName(Game())).task, true);
    message = T("This is now your story save.");
    return true;
}

bool Load(int index, std::string& message) {
    std::lock_guard guard(lock);
    const int game = Game();
    if (!game || index < 0 || index > StorySlot || !GetSlot(index).used) {
        message = T("That slot is empty.");
        return false;
    }
    if (runtime_savedata_mounted()) {
        message = T("The game is saving: try again in a moment.");
        return false;
    }
    // Loading the before-load copy itself puts it back as it is (no new copy over it).
    if (index != UndoSlot && !Store(UndoSlot, message)) {
        return false;
    }
    std::ofstream pending(PendingFile(game));
    pending << "u" << game << " " << SlotFolder(index) << "\n";
    pending.close();
    if (!pending) {
        message = T("Could not write the load request.");
        return false;
    }
    std::printf("Save slots: %s requested, restarting\n", Utf8(SlotDir(game, index)).c_str());
    std::fflush(stdout);
    runtime_restart();
    return true;
}

// Entries the game's task list marks playable that are no place to start from: Naughty Dog's test entries
// (U1 ricks-test-1/2, U2 playtest, destructible-test-1, treasure-test-1) and a bookkeeping step (U3
// ldn-buddy-remove-combat-skills, between London and the Chateau). Left out of the Checkpoints page; their
// numbers stay, so the list keeps the game's numbering.
static bool HiddenTask(int game, uint32_t task) {
    static const struct { int game; uint32_t task; } hidden[] = {
        {1, 0x626acf3c}, {1, 0x6f29e9e5},
        {2, 0x5d5e2861}, {2, 0xcdfaf8f7}, {2, 0x74d7c0f9},
        {3, 0x490efeff},
    };
    for (const auto& h : hidden) {
        if (h.game == game && h.task == task) {
            return true;
        }
    }
    return false;
}

std::vector<Checkpoint> Checkpoints() {
    std::vector<Checkpoint> list;
    for (const TaskRow& t : tasks) {
        if (t.game == Game() && t.number && !HiddenTask(t.game, t.task)) {
            list.push_back(Lookup(t.game, t.task));
        }
    }
    return list;
}

bool JumpsAvailable() {
    return runtime_checkpoint_available() != 0;
}

bool Practising() {
    return runtime_checkpoint_practice() != 0;
}

bool StartPractice(std::string& message) {
    std::lock_guard guard(lock);
    const int game = Game();
    if (!game) {
        message = T("Practice mode is only for Uncharted 1, 2 and 3.");
        return false;
    }
    if (!Practising()) {
        if (runtime_savedata_mounted()) {
            message = T("The game is saving: try again in a moment.");
            return false;
        }
        // The practice copies start as the game's campaign saves; from now until the game restarts the game saves
        // there. Every campaign save (SAVEDATA0<game>0 to 9, runtime_savedata.c practice_redirect), not only the
        // first: the game also plays on a second one (0.3.6.52's Chapter Select made SAVEDATA0<game>1, and Continue
        // picks the newest), and the jumps were written into that one, the player's own save.
        std::error_code ec;
        const fs::path practice = GameBackups(game) / "practice";
        const fs::path temp = practice.parent_path() / "practice.new";
        fs::remove_all(temp, ec);
        fs::create_directories(temp, ec);
        std::string error = ec ? ec.message() : "";
        for (int n = 0; n <= 9 && error.empty(); ++n) {
            const std::string name = "SAVEDATA0" + std::to_string(game) + std::to_string(n);
            if (!fs::exists(SavesDir() / name / "USR-DATA", ec)) {
                continue;
            }
            for (const std::string& dir : {name, name + ".sce_sys"}) {
                if (fs::exists(SavesDir() / dir, ec)) {
                    fs::copy(SavesDir() / dir, temp / dir, fs::copy_options::recursive, ec);
                    if (ec) {
                        error = ec.message();
                        break;
                    }
                }
            }
        }
        if (error.empty()) {
            // An old session's practice copies go (none must come back for a save that is gone).
            fs::remove_all(practice, ec);
            fs::rename(temp, practice, ec);
            if (ec) {
                error = ec.message();
            }
        }
        if (!error.empty()) {
            fs::remove_all(temp, ec);
            message = fmt::format(fmt::runtime(T("Could not start practice mode: {}")), error);
            return false;
        }
        fs::create_directories(practice, ec);
        runtime_checkpoint_set_practice(1);
        std::printf("Checkpoints: practice mode on (campaign saves go to %s)\n", Utf8(practice).c_str());
    }
    return true;
}

bool Jump(const Checkpoint& target, std::string& message) {
    std::lock_guard guard(lock);
    if (!Game() || !target.known || !JumpsAvailable()) {
        message = T("Checkpoint jumps are not available for this release of the game.");
        return false;
    }
    if (!StartPractice(message)) {
        return false;
    }
    runtime_checkpoint_request(target.task);
    message = fmt::format(fmt::runtime(T("Going to {}...")), Describe(target));
    return true;
}

bool KeepPractice(std::string& message) {
    std::lock_guard guard(lock);
    const int game = Game();
    std::error_code ec;
    if (!game || !Practising() ||
        !fs::exists(GameBackups(game) / "practice" / ActiveName(game) / "USR-DATA", ec)) {
        message = T("Nothing to keep yet: the game has not saved since the jump.");
        return false;
    }
    if (runtime_savedata_mounted()) {
        message = T("The game is saving: try again in a moment.");
        return false;
    }
    // The player's save as it was goes to "Before the last load" (undo), then the restart puts the practice save
    // in its place and makes it the story save (bbport_saves_apply_pending).
    if (!Store(UndoSlot, message)) {
        return false;
    }
    std::ofstream pending(PendingFile(game));
    pending << "u" << game << " practice\n";
    pending.close();
    if (!pending) {
        message = T("Could not write the request.");
        return false;
    }
    std::printf("Checkpoints: keeping the practice save, restarting\n");
    std::fflush(stdout);
    runtime_restart();
    return true;
}

int JumpResult() {
    return runtime_checkpoint_result();
}

std::string Describe(const Checkpoint& c, bool translated) {
    const auto tr = [translated](const char* english) { return translated ? T(english) : english; };
    if (!c.known) {
        return c.task ? tr("an unknown checkpoint") : tr("no save yet");
    }
    std::string text =
        c.number ? fmt::format(fmt::runtime(tr("Checkpoint {}")), c.number) : std::string(tr("Cutscene"));
    if (!c.name.empty()) {
        text += " \xC2\xB7 " + c.name;
    }
    return text + " \xC2\xB7 " + fmt::format(fmt::runtime(tr("Chapter {}")), c.chapter) + " " + c.chapter_title;
}

} // namespace BbSaves

namespace {

bool PutBack(int game, const std::string& folder);

// A slot chosen with Load put back as the campaign save.
void ApplyPending(int game) {
    const fs::path pending = PendingFile(game);
    std::ifstream f(pending);
    std::string which_game, folder;
    if (!(f >> which_game >> folder)) {
        return;
    }
    f.close();
    if (which_game != "u" + std::to_string(game)) {
        return;  // a request for another game of the collection: it applies when that game starts
    }
    std::error_code ec;
    fs::remove(pending, ec);
    if (fs::exists(pending, ec)) {
        // Could not be removed (antivirus, a synced or read-only folder): applied now it would be applied again at
        // every start, each time throwing away that session's progress.
        std::printf("Save slots: %s could not be removed: the load is not applied\n", Utf8(pending).c_str());
        return;
    }
    PutBack(game, folder);
}

// A slot folder (slot1..5, before-load, story, practice) put back as the campaign save, while the game has no save
// open: at start-up (a Load from the overlay) or from the game's own save list. False when it was not (the
// player's save is then as it was).
bool PutBack(int game, const std::string& folder) {
    std::error_code ec;
    if (folder.find_first_of("/\\.") != std::string::npos) {
        return false;
    }
    const fs::path from = GameBackups(game) / folder;
    // Into the playthrough the game plays on; a slot folder holds its save under the slot's name, the practice
    // folder under the playthrough's own.
    const std::string name = ActiveName(game);
    const std::string src = folder == "practice" ? name : SlotName(game);
    // A picture still waiting to be written for the save replaced here would land on the one put in its place.
    BbThumbnail::ForgetSave(name + ".sce_sys");
    const fs::path saves = SavesDir();
    std::string error;
    // Both folders copied beside the campaign save first (.bbload). Then the swap: the player's folders moved aside
    // (.bbold), the copies renamed into place, and only then the old folders removed. A failure at any step puts
    // the player's folders back as they were and removes every temporary folder.
    const std::string dirs[] = {name, name + ".sce_sys"};
    const std::string src_dirs[] = {src, src + ".sce_sys"};
    const auto temp_of = [&](const std::string& dir, const char* suffix) { return saves / (dir + suffix); };
    bool ok = true;
    for (const std::string& dir : dirs) {
        fs::remove_all(temp_of(dir, ".bbload"), ec);
        fs::remove_all(temp_of(dir, ".bbold"), ec);
    }
    for (int i = 0; i < 2; ++i) {
        fs::copy(from / src_dirs[i], temp_of(dirs[i], ".bbload"), fs::copy_options::recursive, ec);
        if (ec) {
            error = ec.message();
            ok = false;
            break;
        }
    }
    bool moved_aside[2] = {false, false}, moved_in[2] = {false, false};
    for (int i = 0; ok && i < 2; ++i) {
        if (fs::exists(saves / dirs[i], ec)) {
            fs::rename(saves / dirs[i], temp_of(dirs[i], ".bbold"), ec);
            if (ec) {
                error = ec.message();
                ok = false;
                break;
            }
            moved_aside[i] = true;
        }
    }
    for (int i = 0; ok && i < 2; ++i) {
        fs::rename(temp_of(dirs[i], ".bbload"), saves / dirs[i], ec);
        if (ec) {
            error = ec.message();
            ok = false;
            break;
        }
        moved_in[i] = true;
    }
    std::error_code ignore;
    if (!ok) {
        for (int i = 0; i < 2; ++i) {
            if (moved_in[i]) {
                fs::remove_all(saves / dirs[i], ignore);
            }
            if (moved_aside[i]) {
                fs::rename(temp_of(dirs[i], ".bbold"), saves / dirs[i], ignore);
            }
        }
    }
    // Loading "Before the last load" swaps: the save it replaced becomes the new before-load copy (undo and redo,
    // as the confirmation says), instead of being thrown away. A step that fails leaves .bbold in place.
    bool keep_old = false;
    if (ok && folder == SlotFolder(BbSaves::UndoSlot)) {
        bool swapped = true;
        for (int i = 0; i < 2; ++i) {
            const std::string& dir = dirs[i];        // in the saves folder
            const std::string& kept = src_dirs[i];   // in the before-load folder
            if (!fs::exists(temp_of(dir, ".bbold"), ignore)) {
                continue;
            }
            fs::remove_all(from / (kept + ".bbswap"), ignore);
            std::error_code e1, e2;
            if (fs::exists(from / kept, ignore)) {
                fs::rename(from / kept, from / (kept + ".bbswap"), e1);
            }
            if (!e1) {
                fs::rename(temp_of(dir, ".bbold"), from / kept, e2);
            }
            if (e1 || e2) {
                swapped = false;
                if (!e1 && fs::exists(from / (kept + ".bbswap"), ignore)) {
                    fs::rename(from / (kept + ".bbswap"), from / kept, ignore);
                }
                continue;
            }
            fs::remove_all(from / (kept + ".bbswap"), ignore);
        }
        if (swapped) {
            std::ofstream(from / "saved.txt") << Now() << "\n";
        } else {
            keep_old = true;
            std::printf("Save slots: the replaced save could not become before-load; kept beside the save (.bbold)\n");
        }
    }
    for (const std::string& dir : dirs) {
        fs::remove_all(temp_of(dir, ".bbload"), ignore);
        if (ok && !keep_old) {
            fs::remove_all(temp_of(dir, ".bbold"), ignore);
        }
    }
    if (ok && folder == StoryFolder(ActiveDigit(game))) {
        WriteStory(game, ReadCheckpoint(game, saves / name).task, true);
    }
    // Keep this progress: the practice save is the player's save now, and their story from here on.
    if (ok && folder == "practice") {
        std::string story_error;
        // The story save it replaces (the player's playthrough, maybe away after Chapter Select) is kept.
        const fs::path story = SlotDir(game, BbSaves::StorySlot);
        if (fs::exists(story / SlotName(game) / "USR-DATA", ignore) &&
            !CopySave(story, GameBackups(game) / "story-before-keep", SlotName(game), story_error)) {
            std::printf("Save slots: the story save could not be kept (%s): it stays as it was\n",
                        story_error.c_str());
        } else if (CopyCampaign(game, BbSaves::StorySlot, story_error)) {
            WriteStory(game, ReadCheckpoint(game, saves / name).task, true);
        }
        fs::remove_all(from, ec);
    }
    std::printf("Save slots: %s %s\n", Utf8(from).c_str(),
                ok ? "restored as the campaign save" : ("NOT restored: " + error).c_str());
    return ok;
}

} // namespace

// runtime_checkpoint.c (BB_CHECKPOINT_TEST): practice mode on, with its copy, before a test jump.
static int StartPracticeImpl() {
    std::string message;
    const bool ok = BbSaves::StartPractice(message);
    if (!ok) {
        std::printf("Checkpoints: %s\n", message.c_str());
    }
    return ok;
}

// runtime_savedata.c, before the game's first save mount: a slot chosen with Load is put back now, and a player
// without a story save gets one from the campaign save as it is, before this session writes anything (a first
// save written from Chapter Select would otherwise start it in the wrong place).
static void ApplyPendingImpl() {
    const int game = BbSaves::Game();
    if (!game) {
        return;
    }
    std::lock_guard guard(lock);
    // Once per release folder, before anything here can change a save: every save of this release (the three
    // games' campaign and profile saves) copied to <saves>.backups/before-save-slots, so a player always has the
    // saves as they were before the save slots, story saves and checkpoint jumps first ran.
    {
        std::error_code ec;
        const fs::path saves = SavesDir();
        const fs::path first = saves.parent_path() / (Utf8(saves.filename()) + ".backups") / "before-save-slots";
        if (!fs::exists(first, ec) && fs::is_directory(saves, ec)) {
            const fs::path temp = first.parent_path() / "before-save-slots.new";
            fs::remove_all(temp, ec);
            fs::create_directories(temp, ec);
            bool ok = !ec;
            for (const auto& entry : fs::directory_iterator(saves, ec)) {
                std::error_code dir_ec;
                if (ok && entry.is_directory(dir_ec) && Utf8(entry.path().filename()).rfind("SAVEDATA", 0) == 0) {
                    fs::copy(entry.path(), temp / entry.path().filename(), fs::copy_options::recursive, ec);
                    ok = !ec;
                }
            }
            if (ok) {
                fs::rename(temp, first, ec);
                ok = !ec;
            }
            std::printf("Save slots: %s\n", ok ? ("every save backed up once to " + Utf8(first)).c_str()
                                                : ("the one-time save backup failed: " + ec.message()).c_str());
        }
    }
    ActiveDigit(game);  // the playthrough the game played on last (active.txt), before anything uses it
    ApplyPending(game);
    std::error_code ec;
    const fs::path campaign = SavesDir() / ActiveName(game);
    const char* story_env = std::getenv("BB_STORY_SAVES");
    if (story_env && story_env[0] == '1' && !ReadStory(game).exists && fs::exists(campaign / "USR-DATA", ec)) {
        const BbSaves::Checkpoint now = ReadCheckpoint(game, campaign);
        std::string error;
        if (now.known && CopyCampaign(game, BbSaves::StorySlot, error)) {
            WriteStory(game, now.task, true);
            std::printf("Story save: started at %s\n", BbSaves::Describe(now, false).c_str());
        }
    }
}

// runtime_savedata.c, after the game closed a save: when it was the campaign save, the story save follows it if
// the new checkpoint plays on from the story save's, and stops following otherwise.
static void ClosedImpl(const char* host_dir) {
    const int game = BbSaves::Game();
    if (!game || !host_dir) {
        return;
    }
    const fs::path dir = fs::u8path(host_dir);
    // Only the player's own campaign saves (not the practice copies of checkpoint jumps): each playthrough's story
    // save follows its own.
    const int digit = CampaignDigit(game, Utf8(dir.filename()));
    if (digit < 0 || dir.parent_path() != SavesDir()) {
        return;
    }
    std::lock_guard guard(lock);
    SetActiveDigit(game, digit);
    // The game's lists keep it simple (as on the console: games, New save, Undo): no story saves.
    // BB_STORY_SAVES=1: the story save follows as before (the overlay's Saves page).
    static const bool story_saves = [] {
        const char* env = std::getenv("BB_STORY_SAVES");
        return env && env[0] == '1';
    }();
    if (!story_saves) {
        return;
    }
    const BbSaves::Checkpoint now = ReadCheckpoint(game, dir);
    if (!now.known) {
        return;
    }
    const StoryStatus story = ReadStory(game);
    std::string error;
    if (!story.exists) {
        if (CopyCampaign(game, BbSaves::StorySlot, error)) {
            WriteStory(game, now.task, true);
            std::printf("Story save: started at %s\n", BbSaves::Describe(now, false).c_str());
        }
        return;
    }
    if (!story.following) {
        return;
    }
    const BbSaves::Checkpoint was =
        ReadCheckpoint(game, SlotDir(game, BbSaves::StorySlot) / SlotName(game));
    if (now.task == was.task || PlaysOn(game, was, now)) {
        if (CopyCampaign(game, BbSaves::StorySlot, error)) {
            WriteStory(game, now.task, true);
        } else {
            std::printf("Story save: could not copy the campaign save (%s)\n", error.c_str());
        }
        return;
    }
    WriteStory(game, was.task, false);
    std::printf("Story save: kept at %s (the campaign save moved to %s)\n", BbSaves::Describe(was, false).c_str(),
                BbSaves::Describe(now, false).c_str());
}

// BB_TEST_JUMP_EVERY=<seconds> (tests only; off unless set): a soak across many areas and chapters. Every that
// many seconds of play the game jumps on BB_TEST_JUMP_STEP (default 5) checkpoints in its list, from the
// checkpoint it is at, with the overlay's jump (BbSaves::Jump: the first one turns practice mode on, so the
// player's own save is never written), and wraps at the end. Dev and test entries (Naughty Dog's test levels at
// the end of the U1 and U2 lists, kiosk demos, the end credits) are left out. Play time is the time between the
// game's frames (runtime_checkpoint.c calls this once a frame); a gap over half a second (a load) does not count.
// BB_TEST_JUMP_FIRST=<seconds>: the first jump after that many (default BB_TEST_JUMP_EVERY).
// Each jump logs "Test jump: #N name (chapter C title)" and starts a "VRAM load:" line (BB_VRAM_STATS).
namespace {
bool SoakSkipped(const BbSaves::Checkpoint& c) {
    const std::string& n = c.name;
    return n.empty() || n == "?" || n == "end" || n.find("kiosk") != std::string::npos;
}
bool DevTail(const BbSaves::Checkpoint& c) {
    for (const char* word : {"test", "mocap", "tech-dive", "unused", "empty-level"}) {
        if (c.name.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}
std::vector<BbSaves::Checkpoint> SoakList() {
    std::vector<BbSaves::Checkpoint> list;
    for (const BbSaves::Checkpoint& c : BbSaves::Checkpoints()) {
        if (DevTail(c)) {
            break;  // everything after the first dev entry is Naughty Dog's (U1 ch22 "start" x3 too)
        }
        if (!SoakSkipped(c)) {
            list.push_back(c);
        }
    }
    return list;
}
} // namespace

static void TestJumpImpl() {
    static const double every = [] {
        const char* env = std::getenv("BB_TEST_JUMP_EVERY");
        return env && env[0] ? std::atof(env) : 0.0;
    }();
    if (every <= 0.0) {
        return;
    }
    static const int step = [] {
        const char* env = std::getenv("BB_TEST_JUMP_STEP");
        const int value = env && env[0] ? std::atoi(env) : 5;
        return value != 0 ? value : 5;
    }();
    static const double first = [] {
        const char* env = std::getenv("BB_TEST_JUMP_FIRST");
        return env && env[0] ? std::atof(env) : every;
    }();
    static std::vector<BbSaves::Checkpoint> list;
    static bool started = false, reported = false;
    static int index = -1, jumps = 0;
    static double played = 0.0, due = 0.0;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (!started) {
        started = true;
        last = now;
        due = first;
        list = SoakList();
        std::printf("Test jump: every %.0f s of play (first after %.0f s), %d checkpoints a step, %zu checkpoints "
                    "in the list (BB_TEST_JUMP_EVERY)\n",
                    every, first, step, list.size());
        std::fflush(stdout);
        return;
    }
    const double gap = std::chrono::duration<double>(now - last).count();
    last = now;
    if (gap < 0.5) {
        played += gap;
    }
    if (played < due || list.empty()) {
        return;
    }
    if (!BbSaves::JumpsAvailable()) {
        if (!reported) {
            reported = true;
            std::printf("Test jump: checkpoint jumps are not available for this release; no jumps\n");
        }
        due = played + every;
        return;
    }
    if (index < 0) {
        // From the checkpoint the game is at (its campaign save), or the start of the list.
        const BbSaves::Checkpoint now_at = BbSaves::Current();
        index = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            if (now_at.known && list[i].task == now_at.task) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::printf("Test jump: starting from %s\n", BbSaves::Describe(now_at, false).c_str());
    }
    const int count = static_cast<int>(list.size());
    index = ((index + step) % count + count) % count;
    const BbSaves::Checkpoint& target = list[index];
    std::string message;
    ++jumps;
    if (BbSaves::Jump(target, message)) {
        Vulkan::Vram::test_jump_seq.fetch_add(1, std::memory_order_relaxed);
        std::printf("Test jump: #%d %s (chapter %d %s) [jump %d at %.0f s of play]\n", target.number,
                    target.name.c_str(), target.chapter, target.chapter_title.c_str(), jumps, played);
    } else {
        std::printf("Test jump: #%d %s not made: %s\n", target.number, target.name.c_str(), message.c_str());
    }
    std::fflush(stdout);
    due = played + every;
}

// The game's threads call these (runtime_checkpoint.c, runtime_savedata.c). A C++ exception escaping into that C code
// ended the game with a fail-fast (0xC0000409) and no crash report; here it is logged, and the save in question is
// left as it was (every step above copies first and replaces last).
namespace {
template <typename F>
bool Guarded(const char* what, F&& body) {
    try {
        body();
        return true;
    } catch (const std::exception& e) {
        std::printf("Save slots: %s failed: %s (saves left as they were)\n", what, e.what());
    } catch (...) {
        std::printf("Save slots: %s failed (saves left as they were)\n", what);
    }
    std::fflush(stdout);
    return false;
}
} // namespace

extern "C" int bbport_saves_start_practice(void) {
    int ok = 0;
    Guarded("practice start", [&] { ok = StartPracticeImpl(); });
    return ok;
}

extern "C" void bbport_saves_test_jump_tick(void) {
    static const bool on = [] {
        const char* env = std::getenv("BB_TEST_JUMP_EVERY");
        return env && env[0] && std::atof(env) > 0.0;
    }();
    if (on) {
        Guarded("test jump", [] { TestJumpImpl(); });
    }
}

extern "C" void bbport_saves_apply_pending(void) {
    Guarded("start-up check", [] { ApplyPendingImpl(); });
}

extern "C" void bbport_saves_closed(const char* host_dir) {
    Guarded("story save update", [&] { ClosedImpl(host_dir); });
}

// The game's own save list (runtime_services.c, sceSaveDataDialog load list): the slots join it, after the game's
// own saves. Slot indexes as in BbSaves (0..4 the slots, UndoSlot, StorySlot).
extern "C" int bbport_saves_game(void) {
    return BbSaves::Game();
}

namespace {
constexpr int UndoOverwrite = 200;
fs::path OverwriteDir(int game) {
    return GameBackups(game) / "overwrite";
}
std::string UndoName(int game) {
    std::ifstream f(OverwriteDir(game) / "name.txt");
    std::string name;
    std::error_code ec;
    if (!(f >> name) || CampaignDigit(game, name) < 0 ||
        !fs::exists(OverwriteDir(game) / name / "USR-DATA", ec)) {
        return {};
    }
    return name;
}
// The undo copy and the save of the same name trade places (the game has not opened it yet).
bool SwapUndo(int game, const std::string& name) {
    const fs::path saves = SavesDir(), undo = OverwriteDir(game);
    std::error_code ec;
    for (const std::string suffix : {std::string(), std::string(".sce_sys")}) {
        const fs::path a = saves / (name + suffix), b = undo / (name + suffix), t = saves / (name + suffix + ".bbswap");
        fs::remove_all(t, ec);
        const bool had = fs::exists(a, ec);
        if (had) {
            fs::rename(a, t, ec);
            if (ec) {
                return false;
            }
        }
        fs::rename(b, a, ec);
        if (ec) {
            std::error_code ignore;
            if (had) {
                fs::rename(t, a, ignore);
            }
            return false;
        }
        if (had) {
            fs::rename(t, b, ec);  // the replaced save becomes the next Undo
        }
    }
    std::ofstream(undo / "saved.txt") << Now() << "\n";
    return true;
}
} // namespace

// The campaign save the game plays on (SAVEDATA0<game><n>), for the game's save lists.
extern "C" void bbport_saves_active_name(char* out, size_t size) {
    std::lock_guard guard(lock);
    const int game = BbSaves::Game();
    std::snprintf(out, size, "%s", game ? ActiveName(game).c_str() : "");
}

// The playthrough number (0-9) of a campaign save name, -1 for another save.
extern "C" int bbport_saves_campaign_digit(const char* dir) {
    const int game = BbSaves::Game();
    return game && dir ? CampaignDigit(game, dir) : -1;
}

// A story save entry of any playthrough: index BB_STORY_OF(n) = 100 + n.
namespace {
constexpr int StoryOfBase = 100;
bool StoryOf(int index, int& digit) {
    if (index < StoryOfBase || index > StoryOfBase + 9) {
        return false;
    }
    digit = index - StoryOfBase;
    return true;
}
} // namespace

// 1 when the slot holds a save: `label` gets its lines (name, checkpoint, the game's progress text, when it was
// saved) and `picture` the save's picture (empty when it has none). Index 100 + n: playthrough n's story save.
extern "C" int bbport_saves_list_entry(int index, char* label, size_t label_size, char* picture, size_t picture_size) {
    int used = 0;
    Guarded("save list entry", [&] {
        std::lock_guard guard(lock);
        const int game = BbSaves::Game();
        int digit = -1;
        const bool undo_entry = game && index == UndoOverwrite && !UndoName(game).empty();
        if (undo_entry || (game && StoryOf(index, digit))) {
            const std::string inner = undo_entry ? UndoName(game) : SlotName(game);
            const fs::path dir = undo_entry ? OverwriteDir(game) : GameBackups(game) / StoryFolder(digit);
            std::error_code ec;
            if (!fs::exists(dir / inner / "USR-DATA", ec)) {
                return;
            }
            std::string text =
                undo_entry ? fmt::format(fmt::runtime(T("Undo - Game {} as it was before it was saved over")),
                                         CampaignDigit(game, inner) + 1)
                : digit == ActiveDigit(game) ? std::string(T("Story save - the furthest point of this game"))
                                             : fmt::format(fmt::runtime(T("Story save of game {}")), digit + 1);
            const std::string progress = ReadProgress(dir / (inner + ".sce_sys"));
            if (!progress.empty()) {
                text += "\n" + progress;
            }
            const BbSaves::Checkpoint c = ReadCheckpoint(game, dir / inner);
            if (c.known) {
                text += "\n" + fmt::format(fmt::runtime(T("Chapter {}")), c.chapter) + " " + c.chapter_title +
                        " \xC2\xB7 " +
                        (c.number ? fmt::format(fmt::runtime(T("Checkpoint {}")), c.number) : std::string(T("Cutscene")));
            }
            std::string date;
            std::ifstream info(dir / "saved.txt");
            if (std::getline(info, date) && !date.empty()) {
                text += "\n" + fmt::format(fmt::runtime(undo_entry ? T("Kept {}") : T("Saved {}")), date);
            }
            std::snprintf(label, label_size, "%s", text.c_str());
            picture[0] = 0;
            for (const char* file : {"thumb.png", "icon0.png"}) {
                const fs::path p = dir / (inner + ".sce_sys") / file;
                if (fs::file_size(p, ec) > 0 && !ec) {
                    std::snprintf(picture, picture_size, "%s", Utf8(p).c_str());
                    break;
                }
                ec.clear();
            }
            used = 1;
            return;
        }
        if (!game || index < 0 || index > BbSaves::StorySlot) {
            return;
        }
        BbSaves::Refresh();
        const BbSaves::Slot& s = BbSaves::GetSlot(index);
        if (!s.used) {
            return;
        }
        // Name; the game's progress text; where (chapter and checkpoint, not the task's inner name); when. The undo
        // copy is made at the load, so its time is the load's: in its name, so an old one is plain to see.
        const bool undo = index == BbSaves::UndoSlot;
        const std::string name =
            index == BbSaves::StorySlot ? std::string(T("Story save"))
            : undo && !s.date.empty()
                ? fmt::format(fmt::runtime(T("Undo the load of {} - back to your save from before it")), s.date)
            : undo ? std::string(T("Undo last load - back to your save from before it"))
                   : fmt::format(fmt::runtime(T("Slot {}")), index + 1);
        std::string text = name;
        if (!s.progress.empty()) {
            text += "\n" + s.progress;
        }
        if (s.checkpoint.known) {
            const BbSaves::Checkpoint& c = s.checkpoint;
            text += "\n" + fmt::format(fmt::runtime(T("Chapter {}")), c.chapter) + " " + c.chapter_title + " \xC2\xB7 " +
                    (c.number ? fmt::format(fmt::runtime(T("Checkpoint {}")), c.number) : std::string(T("Cutscene")));
        }
        if (!s.date.empty() && !undo) {
            text += "\n" + fmt::format(fmt::runtime(T("Saved {}")), s.date);
        }
        std::snprintf(label, label_size, "%s", text.c_str());
        picture[0] = 0;
        const fs::path meta = SlotDir(game, index) / (SlotName(game) + ".sce_sys");
        std::error_code ec;
        for (const char* file : {"thumb.png", "icon0.png"}) {
            if (fs::file_size(meta / file, ec) > 0 && !ec) {
                std::snprintf(picture, picture_size, "%s", Utf8(meta / file).c_str());
                break;
            }
            ec.clear();
        }
        used = 1;
    });
    return used;
}

// Picked in the game's save list: the slot is put back as the campaign save only when the game opens that save
// next (bbport_saves_before_mount), so a load the game does not go on with (a declined question, a new list)
// changes nothing. The save it replaces is kept as "Before the last load" (loading that one swaps back).
namespace {
int list_pick = -1; // slot waiting for the game to open its campaign save; guarded by `lock`
}

// 1 when the slot holds a save and waits for the game's mount; 0 not (nothing changes).
extern "C" int bbport_saves_list_pick(int index) {
    int ok = 0;
    Guarded("load from the save list", [&] {
        std::lock_guard guard(lock);
        BbSaves::Refresh();
        const int game = BbSaves::Game();
        int digit = -1;
        std::error_code ec;
        if (game && index == UndoOverwrite) {
            if (!UndoName(game).empty()) {
                list_pick = index;
                ok = 1;
            }
        } else if (game && StoryOf(index, digit)) {
            if (fs::exists(GameBackups(game) / StoryFolder(digit) / SlotName(game) / "USR-DATA", ec)) {
                list_pick = index;
                ok = 1;
            }
        } else if (game && index >= 0 && index <= BbSaves::StorySlot && BbSaves::GetSlot(index).used) {
            list_pick = index;
            ok = 1;
        }
    });
    return ok;
}

// A new list, or the game's dialog closed without a pick: a slot waiting for the mount is forgotten.
extern "C" void bbport_saves_list_cancel(void) {
    std::lock_guard guard(lock);
    list_pick = -1;
}

// runtime_savedata.c, before the game opens a save: the slot picked in the list becomes the campaign save, when the
// game opens the campaign save to read it (the load). Opened to write instead (the load was not gone on with and
// play reached a checkpoint), the pick is forgotten. A failure leaves the player's save as it was (the game then
// loads that) and says so.
extern "C" void bbport_saves_before_mount(const char* dir, int read_only) {
    Guarded("load from the save list", [&] {
        std::lock_guard guard(lock);
        const int game = BbSaves::Game();
        const int digit = game && dir ? CampaignDigit(game, dir) : -1;
        if (digit < 0) {
            return;
        }
        // The playthrough the game opens is the one it plays on (Continue, a pick in its list, a new save).
        SetActiveDigit(game, digit);
        if (list_pick < 0) {
            return;
        }
        const int index = list_pick;
        list_pick = -1;
        if (!read_only) {
            std::printf("Save slots: the game saves instead of loading: the slot picked in the list is not put back\n");
            return;
        }
        // Undo: the kept game and the save of its name trade places, as the game opens it to load.
        if (index == UndoOverwrite) {
            const std::string name = UndoName(game);
            if (name.empty() || name != dir || runtime_checkpoint_practice()) {
                std::printf("Saves: Undo not applied (%s)\n", name.empty() ? "nothing kept"
                                                              : runtime_checkpoint_practice() ? "practice mode"
                                                                                              : "the game opened another save");
                return;
            }
            BbThumbnail::ForgetSave(name + ".sce_sys");
            std::printf("Saves: %s %s\n", name.c_str(),
                        SwapUndo(game, name) ? "put back as it was (the save it replaced is the next Undo)"
                                             : "could NOT be put back: the save stays as it was");
            return;
        }
        BbSaves::Refresh();
        // A playthrough's story save goes back into that playthrough (the game opens it: the list chose it).
        int story_digit = -1;
        const bool story_of = StoryOf(index, story_digit);
        std::error_code exists_ec;
        const bool used = story_of ? fs::exists(GameBackups(game) / StoryFolder(story_digit) / SlotName(game) /
                                                    "USR-DATA", exists_ec)
                                   : BbSaves::GetSlot(index).used;
        if (!used || runtime_checkpoint_practice() || (story_of && story_digit != digit)) {
            std::printf("Save slots: the slot picked in the list is not put back (%s)\n",
                        runtime_checkpoint_practice() ? "practice mode"
                        : !used                       ? "the slot is empty"
                                                      : "the game opened another of its saves");
            return;
        }
        const std::string folder = story_of ? StoryFolder(story_digit) : SlotFolder(index);
        // The campaign save is not open (the game is about to open it); another save (the profile) may be, so the
        // copy is made here rather than by Store, which waits for no save at all to be open.
        std::string error;
        std::error_code ec;
        if (index != BbSaves::UndoSlot && fs::exists(SavesDir() / ActiveName(game) / "USR-DATA", ec) &&
            !CopyCampaign(game, BbSaves::UndoSlot, error)) {
            std::printf("Save slots: the save could not be kept as Before the last load (%s): slot not put back\n",
                        error.c_str());
            return;
        }
        PutBack(game, folder);
        BbSaves::Refresh();
    });
}

// Undo, the one safety net of the game's lists: before the game saves over one of the player's games from
// Chapter Select or New Game (or the pause menu's Save game onto a game other than the one in play), that game is
// copied to <saves>.backups/u<game>/overwrite (its own name; name.txt, saved.txt). Load game offers it back
// ("Undo"); putting it back swaps, so the save it replaces becomes the next Undo.


// Before the game saves over `dir` (picked in its "where to save" list): a copy for Undo. 1 done.
extern "C" int bbport_saves_keep_before_overwrite(const char* dir) {
    int ok = 0;
    Guarded("keep a save before it is saved over", [&] {
        std::lock_guard guard(lock);
        const int game = BbSaves::Game();
        std::error_code ec;
        if (!game || !dir || CampaignDigit(game, dir) < 0 || !fs::exists(SavesDir() / dir / "USR-DATA", ec)) {
            return;
        }
        std::string error;
        fs::create_directories(GameBackups(game), ec);
        if (!CopySave(SavesDir(), OverwriteDir(game), dir, error)) {
            std::printf("Saves: %s could not be kept for Undo (%s)\n", dir, error.c_str());
            return;
        }
        std::ofstream(OverwriteDir(game) / "name.txt") << dir << "\n";
        std::ofstream(OverwriteDir(game) / "saved.txt") << Now() << "\n";
        std::printf("Saves: %s kept for Undo before the game saves over it\n", dir);
        ok = 1;
    });
    return ok;
}

// The save Undo puts back (its name in `out`), 1 when there is one.
extern "C" int bbport_saves_undo_name(char* out, size_t size) {
    std::lock_guard guard(lock);
    const int game = BbSaves::Game();
    const std::string name = game ? UndoName(game) : std::string();
    std::snprintf(out, size, "%s", name.c_str());
    return !name.empty();
}

// Deleted in the game's save list: a slot (1..5 only) moves to <saves>.backups/u<game>/deleted/slot<n>-<time>,
// never removed. 1 done.
extern "C" int bbport_saves_trash(int index) {
    int ok = 0;
    Guarded("slot delete", [&] {
        std::lock_guard guard(lock);
        const int game = BbSaves::Game();
        if (!game || index < 0 || index >= BbSaves::SlotCount) {
            return;
        }
        std::time_t t = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
        const fs::path to = GameBackups(game) / "deleted" / (SlotFolder(index) + "-" + stamp);
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::rename(SlotDir(game, index), to, ec);
        ok = !ec;
        std::printf("Save slots: %s %s\n", Utf8(SlotDir(game, index)).c_str(),
                    ok ? ("moved to " + Utf8(to)).c_str() : ("NOT deleted: " + ec.message()).c_str());
        BbSaves::Refresh();
    });
    return ok;
}

// The game's "Save game" list (runtime_services.c): next to the save Continue loads, slots 1-5 as places to save. The
// game always writes its one campaign save (a second campaign save, "New save", would leave the slots, the story save
// and Current save behind); a slot picked there gets a copy of it once the game has written it
// (bbport_saves_written), as Save here does.
namespace {
int save_pick = -1; // slot (0..4) waiting for the game's write of its campaign save; guarded by `lock`
}

// A slot's line in the "Save game" list: as bbport_saves_list_entry when it holds a save (1), else "Slot N - empty"
// (2); 0 for no slot.
extern "C" int bbport_saves_save_entry(int index, char* label, size_t label_size, char* picture, size_t picture_size) {
    if (index < 0 || index >= BbSaves::SlotCount) {
        return 0;
    }
    if (bbport_saves_list_entry(index, label, label_size, picture, picture_size)) {
        return 1;
    }
    std::snprintf(label, label_size, "%s\n%s", fmt::format(fmt::runtime(T("Slot {} - empty")), index + 1).c_str(),
                  T("Your game saves as usual, and a copy goes here"));
    picture[0] = 0;
    return 2;
}

// Picked in the "Save game" list: the slot gets the campaign save once the game has written it.
extern "C" void bbport_saves_save_pick(int index) {
    std::lock_guard guard(lock);
    save_pick = index >= 0 && index < BbSaves::SlotCount ? index : -1;
}

extern "C" void bbport_saves_save_cancel(void) {
    std::lock_guard guard(lock);
    save_pick = -1;
}

// runtime_savedata.c: a save the game opened to write is closed. The campaign save after a slot was picked in the
// "Save game" list: copied into that slot.
extern "C" void bbport_saves_written(const char* host_dir) {
    Guarded("save into a slot", [&] {
        std::lock_guard guard(lock);
        const int game = BbSaves::Game();
        if (save_pick < 0 || !game || !host_dir) {
            return;
        }
        const fs::path dir = fs::u8path(host_dir);
        if (CampaignDigit(game, Utf8(dir.filename())) != ActiveDigit(game) || dir.parent_path() != SavesDir()) {
            return;
        }
        const int index = save_pick;
        save_pick = -1;
        std::string error;
        if (CopyCampaign(game, index, error)) {
            std::printf("Save slots: the game's save copied into slot %d (picked in the Save game list)\n", index + 1);
        } else {
            std::printf("Save slots: the game's save could not be copied into slot %d: %s\n", index + 1, error.c_str());
        }
        BbSaves::Refresh();
    });
}
