// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <array>
#include <string>
#include <functional>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// bbport: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();
    /// bbport: removes one stored blob (after the writes queued before it).
    void Remove(BlobType type, const std::string& name);

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

    /// bbport: names of the stored blobs of a type, for loading them one by one through Load() (in parallel).
    std::vector<std::string> ListBlobNames(BlobType type);
    /// bbport: the cache folder (valid after Open()): the pack (shaders.pack), the driver's cache data, the
    /// prepared markers.
    [[nodiscard]] const std::filesystem::path& Directory() const {
        return directory;
    }

    /// bbport: waits (at most `max_ms`) until the queued writes are on disk, so a game that
    /// ends right after a compile does not lose it.
    void Flush(u32 max_ms);
    /// bbport: writes and removals still queued (the preparation's "saving" progress).
    u32 PendingWrites() const;
    /// bbport: writes and removals done since the start (progress, while new ones are also queued).
    u64 CompletedWrites() const;
    /// bbport: shader files that could not be saved since the start (disk full, refused): the prepared marker is
    /// not written after one, so the next start prepares what is missing.
    u64 FailedWrites() const;
    /// bbport: how long the writes have been slow on end, in seconds (0 when they are not): since the first of a run
    /// of writes over a second, or for how long a write still running has taken, once over a second. Slowness ends
    /// once a second of writes has gone by without a slow one (a one-off stall costs only the stall).
    double SlowForSeconds() const;
    /// bbport: while the preparation page or the exit waits for the writes (no game running whose reads need the
    /// disk), the slow-disk throttle (one write at a time, a pause after each) is off. Counted: on/off in pairs.
    void SetUnthrottled(bool on);
    /// bbport: waits until every queued write is done, or until none has completed for `stall_s` seconds (stuck);
    /// `tick(left)` about 4 times a second (false: stop waiting). True when all were done.
    bool WaitForWrites(double stall_s, const std::function<bool(u32 left)>& tick);
    /// bbport (pack cache): everything written so far is flushed to the disk (before a prepared marker). False: this
    /// game cannot write the cache, or the flush failed.
    bool Sync();
    /// bbport (pack cache): this game writes the cache (another game may hold it: then it only reads).
    [[nodiscard]] bool CanSave() const;
    /// bbport (pack cache): the pack's id (16 hex digits; new on every clear and when damage lost records), which a
    /// prepared marker names; empty when there is none or this game found damage it could not repair.
    [[nodiscard]] std::string PackIdText() const;
    /// bbport (pack cache): one log line with this session's appends (count, MB, write times), at the exit.
    void PrintWriteStats() const;
    /// bbport (pack cache): old cache files (one per blob, an earlier build's folder cache) still to be moved into the
    /// pack; 0: none.
    [[nodiscard]] u32 FolderCacheToMove() const;
    /// bbport (pack cache): what this build stores, for telling whether an old folder cache is still usable.
    struct CacheVersions {
        /// The prepared marker's version words of this build ("cache <binary>.<meta>.<key>").
        std::string marker_words;
        /// True when a stored blob is of this build's versions (a meta's or key's version fields, the profile).
        std::function<bool(BlobType type, const std::vector<u8>& data)> current;
    };
    /// bbport (pack cache): moves them (read, appended, read back from the disk; the old files are then removed
    /// behind the game), only when the folder cache is obviously this port's (FolderCacheIsOurs in cache_storage.cpp; any
    /// other is left in place untouched). When the old cache is of other versions (its markers' version words, its profile, its metas
    /// and keys), nothing is moved: the pack starts empty and the old files are removed behind the game.
    /// `progress(done, total)` from the reading threads; (0, 0) at the end. False: the old files are kept (the next
    /// start tries again).
    bool MoveFolderCache(const CacheVersions& versions, const std::function<void(u32 done, u32 total)>& progress);
    /// bbport (pack cache): this build's prepared marker for a variant list ("shaders.pack.prepared_<list>"; other
    /// builds' "prepared_<list>" files in the same folder are never written; an earlier build of this port's are
    /// turned into this build's by the move).
    [[nodiscard]] std::filesystem::path MarkerPath(const std::string& list_name) const;
    /// bbport (pack cache): the identity line of this build's prepared markers (their third line); the pack's header
    /// carries Pack::PortName.
    static constexpr const char* MarkerIdentity = "by Uncharted PC (bbport), shader pack format 1";

private:
    std::jthread io_worker{}; ///< the one writer thread (appends to the pack in order)
    std::filesystem::path cache_path{};
    std::filesystem::path directory{}; ///< the cache folder
    bool opened{};
};

} // namespace Storage
