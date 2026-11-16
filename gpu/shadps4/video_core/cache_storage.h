// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <array>
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

    /// bbport: names (without extension) of the stored blobs of a type, for loading them one by
    /// one through Load() (in parallel for a folder cache; the zip reader is not thread-safe).
    std::vector<std::string> ListBlobNames(BlobType type);
    /// bbport: the cache folder (valid after Open(); empty for an archived cache).
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

public:
    static constexpr u32 IoWorkers = 4; ///< file writer threads (one for an archive)

private:
    std::array<std::jthread, 4> io_workers{};
    std::filesystem::path cache_path{};
    std::filesystem::path directory{}; ///< cache_path for a folder cache, empty for an archive
    bool opened{};
};

} // namespace Storage
