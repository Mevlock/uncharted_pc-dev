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
std::string SlotName(int game) {
    return "SAVEDATA0" + std::to_string(game) + "0";
}
fs::path GameBackups(int game) {
    fs::path saves = SavesDir();
    return saves.parent_path() / (Utf8(saves.filename()) + ".backups") / ("u" + std::to_string(game));
}
std::string SlotFolder(int index) {
    return index == BbSaves::UndoSlot    ? std::string("before-load")
           : index == BbSaves::StorySlot ? std::string("story")
                                         : "slot" + std::to_string(index + 1);
}
fs::path SlotDir(int game, int index) {
    return GameBackups(game) / SlotFolder(index);
}
fs::path PendingFile(int game) {
    return GameBackups(game).parent_path() / "pending-load.txt";
}
fs::path StoryState(int game) {
    return GameBackups(game) / "story-state.txt";
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
bool CopySave(const fs::path& from, const fs::path& to, const std::string& name, std::string& error) {
    std::error_code ec;
    const fs::path temp = to.parent_path() / (Utf8(to.filename()) + ".new");
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    for (const std::string& dir : {name, name + ".sce_sys"}) {
        fs::copy(from / dir, temp / dir, fs::copy_options::recursive, ec);
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

// The campaign save (or, from `from`, another folder holding one) copied into a slot folder, with the time.
bool CopyCampaign(int game, int index, std::string& error, const fs::path& from = {}) {
    const fs::path to = SlotDir(game, index);
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!CopySave(from.empty() ? SavesDir() : from, to, SlotName(game), error)) {
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
                              SlotName(Game()) / "USR-DATA";
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
    if (!fs::exists(source / SlotName(game) / "USR-DATA", ec)) {
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

std::vector<Checkpoint> Checkpoints() {
    std::vector<Checkpoint> list;
    for (const TaskRow& t : tasks) {
        if (t.game == Game() && t.number) {
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
        // The practice copy starts as the campaign save; from now until the game restarts the game saves there.
        std::error_code ec;
        const fs::path practice = GameBackups(game) / "practice";
        std::string error;
        if (fs::exists(SavesDir() / SlotName(game) / "USR-DATA", ec)) {
            if (!CopySave(SavesDir(), practice, SlotName(game), error)) {
                message = fmt::format(fmt::runtime(T("Could not start practice mode: {}")), error);
                return false;
            }
        } else {
            fs::remove_all(practice, ec);  // no campaign save: an old session's practice copy must not come back
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
        !fs::exists(GameBackups(game) / "practice" / SlotName(game) / "USR-DATA", ec)) {
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
    if (folder.find_first_of("/\\.") != std::string::npos) {
        return;
    }
    const fs::path from = GameBackups(game) / folder;
    const std::string name = SlotName(game);
    const fs::path saves = SavesDir();
    std::string error;
    // Both folders copied beside the campaign save first (.bbload). Then the swap: the player's folders moved aside
    // (.bbold), the copies renamed into place, and only then the old folders removed. A failure at any step puts
    // the player's folders back as they were and removes every temporary folder.
    const std::string dirs[] = {name, name + ".sce_sys"};
    const auto temp_of = [&](const std::string& dir, const char* suffix) { return saves / (dir + suffix); };
    bool ok = true;
    for (const std::string& dir : dirs) {
        fs::remove_all(temp_of(dir, ".bbload"), ec);
        fs::remove_all(temp_of(dir, ".bbold"), ec);
    }
    for (const std::string& dir : dirs) {
        fs::copy(from / dir, temp_of(dir, ".bbload"), fs::copy_options::recursive, ec);
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
        for (const std::string& dir : dirs) {
            if (!fs::exists(temp_of(dir, ".bbold"), ignore)) {
                continue;
            }
            fs::remove_all(from / (dir + ".bbswap"), ignore);
            std::error_code e1, e2;
            if (fs::exists(from / dir, ignore)) {
                fs::rename(from / dir, from / (dir + ".bbswap"), e1);
            }
            if (!e1) {
                fs::rename(temp_of(dir, ".bbold"), from / dir, e2);
            }
            if (e1 || e2) {
                swapped = false;
                if (!e1 && fs::exists(from / (dir + ".bbswap"), ignore)) {
                    fs::rename(from / (dir + ".bbswap"), from / dir, ignore);
                }
                continue;
            }
            fs::remove_all(from / (dir + ".bbswap"), ignore);
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
    if (ok && folder == "story") {
        WriteStory(game, ReadCheckpoint(game, saves / name).task, true);
    }
    // Keep this progress: the practice save is the player's save now, and their story from here on.
    if (ok && folder == "practice") {
        std::string story_error;
        // The story save it replaces (the player's playthrough, maybe away after Chapter Select) is kept.
        const fs::path story = SlotDir(game, BbSaves::StorySlot);
        if (fs::exists(story / name / "USR-DATA", ignore) &&
            !CopySave(story, GameBackups(game) / "story-before-keep", name, story_error)) {
            std::printf("Save slots: the story save could not be kept (%s): it stays as it was\n",
                        story_error.c_str());
        } else if (CopyCampaign(game, BbSaves::StorySlot, story_error)) {
            WriteStory(game, ReadCheckpoint(game, saves / name).task, true);
        }
        fs::remove_all(from, ec);
    }
    std::printf("Save slots: %s %s\n", Utf8(from).c_str(),
                ok ? "restored as the campaign save" : ("NOT restored: " + error).c_str());
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
    ApplyPending(game);
    std::error_code ec;
    const fs::path campaign = SavesDir() / SlotName(game);
    if (!ReadStory(game).exists && fs::exists(campaign / "USR-DATA", ec)) {
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
    // Only the player's own campaign save (not the practice copy of checkpoint jumps).
    if (Utf8(dir.filename()) != SlotName(game) || dir.parent_path() != SavesDir()) {
        return;
    }
    std::lock_guard guard(lock);
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

extern "C" void bbport_saves_apply_pending(void) {
    Guarded("start-up check", [] { ApplyPendingImpl(); });
}

extern "C" void bbport_saves_closed(const char* host_dir) {
    Guarded("story save update", [&] { ClosedImpl(host_dir); });
}
