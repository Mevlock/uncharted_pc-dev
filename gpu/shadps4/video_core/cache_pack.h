// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: the shader cache in one file (shaders.pack) instead of one file per blob.
//
// A player's cache held ~32,000 small files; opening and reading each (and the antivirus scanning each) took minutes
// on some PCs at every start (224 s for 13,000 shaders on one), and the first start's saving waited on Windows' file
// calls, not on the drive. The pack is one file:
//
//   header (64 bytes): magic, format version, pack id (random, new on every clear), creation time, checksum
//   records, appended one after another: a 32-byte header (magic, type, flags, name and data length, checksum of the
//   name and data, checksum of the header itself), then the name, then the data.
//
// The index (type + name -> where its newest record is) is rebuilt from the records at every open. A record whose
// header checksum fails is skipped by looking for the next record header; a record whose data checksum fails is
// dropped alone. A torn last record (the game ended during a write) is cut off; nothing before it is lost. A newer
// record of a name replaces the older; a removal is a record without data. Reads check the data checksum again.
//
// One writer at a time (it holds <pack>.lock); other processes open the pack read-only alongside it and only see
// what was in it when they opened it. Clearing and compaction write a new file beside the pack, flush it and rename
// it over the old one.
//
// No dependencies beyond the C++ library, xxHash and the OS, so the offline tests build it alone.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Storage::Pack {

constexpr uint32_t FormatVersion = 1;
constexpr uint32_t HeaderSize = 64;
constexpr uint32_t RecordHeaderSize = 32;
constexpr uint32_t MaxNameLength = 1024;
constexpr uint32_t MaxDataLength = 256u << 20;

struct OpenStats {
    bool ok = false;           ///< the pack is usable (also when it was made new)
    bool writer = false;       ///< this process may write (it holds the lock)
    bool created = false;      ///< there was no pack (or none usable): a new empty one
    bool replaced = false;     ///< an unusable pack (bad header, other format version) was replaced by a new one
    uint32_t old_version = 0;  ///< the format version of a replaced pack (0: unreadable header)
    uint64_t records = 0;      ///< good records read
    uint64_t live = 0;         ///< names in the index
    uint64_t live_bytes = 0;   ///< their data
    uint64_t dead_bytes = 0;   ///< records replaced or removed by later ones (compaction frees them)
    uint64_t bad_records = 0;  ///< records dropped inside the file (checksum failed)
    uint64_t bad_bytes = 0;    ///< bytes skipped inside the file
    uint64_t torn_bytes = 0;   ///< the cut-off end (a record the last session did not finish)
    uint64_t file_bytes = 0;   ///< file size when opened
    bool compacted = false;    ///< rewritten at open (dead or bad bytes)
    double ms = 0;             ///< time to open
    std::string error;         ///< why it is not usable
};

class PackFile {
public:
    PackFile() = default;
    ~PackFile();
    PackFile(const PackFile&) = delete;
    PackFile& operator=(const PackFile&) = delete;

    /// Opens (or creates) the pack. `want_writer`: take the writer lock if no other process holds it (else read-only).
    /// `compact`: a writer rewrites a pack with many dead bytes, or with bad records, at open.
    OpenStats Open(const std::filesystem::path& path, bool want_writer, bool compact = true);
    void Close();
    [[nodiscard]] bool IsOpen() const {
        return open_;
    }
    [[nodiscard]] bool IsWriter() const {
        return writer_;
    }
    /// The pack's id (random, new on every Clear and when damage lost records), as 16 hex digits.
    [[nodiscard]] std::string IdText() const;
    [[nodiscard]] uint64_t Id() const {
        return id_;
    }
    /// This reader found damage (dropped records) it could not repair: what was prepared may be missing.
    [[nodiscard]] bool Damaged() const {
        return damaged_;
    }
    [[nodiscard]] const std::filesystem::path& Path() const {
        return path_;
    }

    /// Writer only. Appends a record (the newest of the name wins). False: not written (disk full, refused); the
    /// pack stays as it was. Thread-safe against readers; writes are serialised.
    bool Put(uint16_t type, std::string_view name, const void* data, size_t size);
    /// Writer only. Appends a removal record (nothing when the name is not in the pack).
    bool Erase(uint16_t type, std::string_view name);
    /// Thread-safe. The data of a name (checksum checked); false when it is not in the pack or damaged.
    bool Get(uint16_t type, std::string_view name, std::vector<uint8_t>& out) const;
    /// Thread-safe. The size and data checksum of a name without reading it.
    bool Find(uint16_t type, std::string_view name, uint64_t* size, uint64_t* hash) const;
    /// Thread-safe. The names of a type in the index.
    std::vector<std::string> Names(uint16_t type) const;
    /// Writer only. Flushes everything written to the disk (before the prepared marker, at the exit).
    bool Sync();
    /// Writer only. A new empty pack (new id) in place of this one. Readers forget the index.
    bool Clear();
    /// Writer only. Rewrites the live records into a new file (same id) and renames it over the pack.
    bool Compact();

    [[nodiscard]] uint64_t EndOffset() const {
        return end_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t DeadBytes() const {
        return dead_bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t Count() const;

    /// The data checksum used by the records (XXH3 of name then data), for comparing a file with a record.
    static uint64_t BodyHash(std::string_view name, const void* data, size_t size);
    /// Reads only the header of a pack file: its id (for tools and the prepared marker check). 0: none or invalid.
    static uint64_t ReadId(const std::filesystem::path& path);

    /// Test hook: called before each write's system call with the bytes about to be written; returning a smaller
    /// count writes only that many (a simulated crash or a full disk). Never set in the game.
    std::function<size_t(uint64_t offset, size_t size)> test_write_limit;

private:
    struct Entry {
        uint64_t offset;    ///< of the record header
        uint32_t name_len;
        uint32_t data_len;
        uint64_t hash;
    };
    static std::string Key(uint16_t type, std::string_view name);
    bool OpenHandles(bool writable);
    void CloseHandles();
    bool WriteHeaderFile(const std::filesystem::path& file, uint64_t id, uint64_t* written_size);
    bool Scan(OpenStats& stats);
    bool AppendRecord(uint16_t type, uint16_t flags, std::string_view name, const void* data, size_t size);
    bool ReadAt(uint64_t offset, void* dst, size_t size) const;
    bool WriteAt(uint64_t offset, const void* src, size_t size);
    bool Truncate(uint64_t size);
    bool ReplaceWith(const std::filesystem::path& tmp);
    bool CompactLocked();

    std::filesystem::path path_;
    bool open_ = false;
    bool writer_ = false;
    bool damaged_ = false;
    uint64_t id_ = 0;
    std::atomic<uint64_t> end_{0};
    std::atomic<uint64_t> dead_bytes_{0};
    uint64_t synced_end_ = 0; ///< the end at the last flush (under write_mutex_); 0: never flushed
    mutable std::shared_mutex mutex_;  ///< index and file identity (shared: reads; unique: index changes, Clear)
    std::mutex write_mutex_;           ///< one append at a time
    std::unordered_map<std::string, Entry> index_;
#ifdef _WIN32
    void* read_handle_ = nullptr;  ///< overlapped: parallel positional reads
    void* write_handle_ = nullptr;
    void* lock_handle_ = nullptr;
#else
    int read_fd_ = -1;
    int write_fd_ = -1;
    int lock_fd_ = -1;
#endif
};

} // namespace Storage::Pack
