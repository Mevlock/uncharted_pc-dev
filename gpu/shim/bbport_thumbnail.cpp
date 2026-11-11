// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: save pictures (bbport_thumbnail.h). First ported from BilalReloaded's port (gow3_thumbnail.cpp, PR #349 by
// the-best-is-best).
//
// A save's picture is the last picture of play of that same game (that campaign save), taken when the game writes
// the save; nothing waits for later play. Pictures come from two places:
//   clean   the upscaler's image of the scene before the game's UI (vk_temporal_upscaler.cpp): no HUD, no
//           subtitles, no pause menu, so it is taken behind the pause menu too (a save from it shows where it was
//           made); about once a second.
//   frame   the frame as shown (vk_presenter.cpp), while a follow / cover camera runs (not in a menu or a
//           cutscene): when there is no clean picture (no upscaler in use).
// Each picture belongs to the game in play when it was taken (bbport_saves_active_name); a save of another game, or
// one with no picture of its game yet this session, keeps the picture it has.
#include "bbport_thumbnail.h"
#include "../bbgpu.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <miniz.h>

extern "C" int runtime_mouselook_camera_recent(void); // runtime_mouselook.c: a follow / cover camera ran lately
extern "C" int runtime_mouselook_enabled(void);
extern "C" void bbport_saves_active_name(char* out, size_t size); // bbport_saves.cpp: the campaign save in play

namespace BbThumbnail {
namespace {

using Clock = std::chrono::steady_clock;
std::mutex mutex;
std::vector<unsigned char> picture; // RGB8, Width x Height: the newest picture of play; empty: none yet
std::string picture_game;           // the campaign save in play when it was taken ("SAVEDATA031")
Clock::time_point clean_at{};       // the newest clean picture's time (a frame picture then waits its turn)
Clock::time_point last_request{}, last_clean_request{};
// The camera hooks keep running behind the pause menu, so frame pictures go on being taken there (with the menu in
// them). A save made from the save list then gets the frame picture from the moment Options was last pressed.
std::vector<unsigned char> before_pause;
std::string before_pause_game;
Clock::time_point save_list_at{};

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SAVE_PICTURE");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

bool WritePng(const std::string& path, const std::vector<unsigned char>& rgb) {
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), int(Width), int(Height), 3, &size, 6,
                                                           MZ_FALSE);
    if (!png) {
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    const bool ok = f && std::fwrite(png, 1, size, f) == size;
    if (f) {
        std::fclose(f);
    }
    mz_free(png);
    if (!ok) {
        std::remove(path.c_str());
    }
    return ok;
}

bool InPlay() {
    return !runtime_mouselook_enabled() || runtime_mouselook_camera_recent();
}

std::string GameInPlay() {
    char name[32] = {};
    bbport_saves_active_name(name, sizeof(name));
    return name;
}

std::vector<unsigned char> ToRgb(const unsigned char* rgba) {
    std::vector<unsigned char> rgb(size_t(Width) * Height * 3);
    for (size_t i = 0, n = size_t(Width) * Height; i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return rgb;
}

// "SAVEDATA031" from ".../SAVEDATA031.sce_sys/thumb.png"; empty for no save folder.
std::string SaveOf(const std::string& path) {
    const size_t at = path.rfind("SAVEDATA");
    if (at == std::string::npos) {
        return {};
    }
    const size_t end = path.find(".sce_sys", at);
    return end == std::string::npos ? std::string() : path.substr(at, end - at);
}

// A campaign save (SAVEDATA0<game><digit>), not the profile (SAVEDATA0<game>P) or the collection's.
bool IsCampaign(const std::string& save) {
    return save.size() == 11 && save.compare(0, 9, "SAVEDATA0") == 0 && save[10] >= '0' && save[10] <= '9';
}

} // namespace

bool Wanted() {
    if (!Enabled()) {
        return false;
    }
    const auto now = Clock::now();
    if (now - last_request < std::chrono::seconds(1) || !InPlay()) {
        return false;
    }
    // A gap since the last request: play has just started again (a save list from the pause menu is over).
    if (now - last_request > std::chrono::seconds(2)) {
        std::scoped_lock lock{mutex};
        save_list_at = {};
    }
    last_request = now;
    // While the upscaler gives clean pictures, frame pictures (with the HUD) are not needed.
    std::scoped_lock lock{mutex};
    return now - clean_at > std::chrono::seconds(3);
}

void Store(const unsigned char* rgba) {
    std::vector<unsigned char> rgb = ToRgb(rgba);
    const std::string game = GameInPlay();
    std::scoped_lock lock{mutex};
    if (Clock::now() - clean_at <= std::chrono::seconds(3)) {
        return; // a clean picture came in meanwhile
    }
    picture = std::move(rgb);
    picture_game = game;
}

bool WantedClean() {
    if (!Enabled()) {
        return false;
    }
    const auto now = Clock::now();
    if (now - last_clean_request < std::chrono::seconds(1)) {
        return false;
    }
    last_clean_request = now;
    return true;
}

void StoreClean(const unsigned char* rgba) {
    std::vector<unsigned char> rgb = ToRgb(rgba);
    const std::string game = GameInPlay();
    std::scoped_lock lock{mutex};
    picture = std::move(rgb);
    picture_game = game;
    clean_at = Clock::now();
}

void ForgetSave(const std::string&) {}

void NoteSaveList() {
    std::scoped_lock lock{mutex};
    save_list_at = Clock::now();
}

void CampaignOpened(const std::string&) {}

// The game loads one of its saves: the picture from before the last pause belongs to the play before it.
void Loading(const std::string&) {
    std::scoped_lock lock{mutex};
    save_list_at = {};
    before_pause.clear();
    before_pause_game.clear();
}

} // namespace BbThumbnail

// runtime_savedata.c, a save the game wrote is closed: its picture. 1 written, 0 none (the save keeps the one it has).
extern "C" int bbgpu_thumbnail_save(const char* path) {
    using namespace BbThumbnail;
    if (!path || !*path || !Enabled()) {
        return 0;
    }
    const std::string save = SaveOf(path);
    std::vector<unsigned char> rgb;
    {
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        const bool clean_recent = now - clean_at <= std::chrono::seconds(3);
        const auto mine = [&](const std::string& game) { return !IsCampaign(save) || game == save; };
        if (!clean_recent && save_list_at != Clock::time_point{} && now - save_list_at < std::chrono::minutes(2) &&
            !before_pause.empty() && mine(before_pause_game)) {
            // A save from the pause menu's list without clean pictures: the frame from before the pause (the menu
            // is in the newer ones).
            rgb = before_pause;
        } else if (!picture.empty() && mine(picture_game)) {
            rgb = picture;
        } else {
            return 0; // no picture of this game's play yet: it keeps the one it has
        }
    }
    // Off the game's thread (it is closing a save): encoding takes a few milliseconds.
    std::thread([path = std::string(path), rgb = std::move(rgb)] {
        if (!WritePng(path, rgb)) {
            std::printf("GPU: save picture not written (%s)\n", path.c_str());
        }
    }).detach();
    return 1;
}

extern "C" void bbgpu_thumbnail_pause(void) {
    using namespace BbThumbnail;
    std::scoped_lock lock{mutex};
    // With clean pictures the newest one is from behind the menu, without it: nothing to keep.
    if (Clock::now() - clean_at > std::chrono::seconds(3)) {
        before_pause = picture;
        before_pause_game = picture_game;
    }
}
