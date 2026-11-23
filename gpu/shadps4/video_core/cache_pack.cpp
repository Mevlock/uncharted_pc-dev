// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/cache_pack.h"

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Storage::Pack {

namespace {

constexpr char HeaderMagic[8] = {'B', 'B', 'S', 'H', 'P', 'A', 'C', 'K'};
constexpr uint32_t RecordMagic = 0x4B505342; // "BSPK"
constexpr uint16_t FlagErased = 1;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t id;
    uint64_t created;
    char port[16];        ///< PortName, zero-padded (all zero: a pack of this port's test builds before 2026-11-22)
    uint8_t reserved[8];
    uint64_t hash; ///< XXH3 of the 56 bytes before it
};
struct RecordHeader {
    uint32_t magic;
    uint16_t type;
    uint16_t flags;
    uint32_t name_len;
    uint32_t data_len;
    uint64_t body_hash;   ///< XXH3 of the name, then the data
    uint64_t header_hash; ///< XXH3 of the 24 bytes before it
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == HeaderSize);
static_assert(sizeof(RecordHeader) == RecordHeaderSize);

uint64_t RecordHeaderHash(const RecordHeader& h) {
    return XXH3_64bits(&h, offsetof(RecordHeader, header_hash));
}

bool ValidRecordHeader(const RecordHeader& h) {
    return h.magic == RecordMagic && h.name_len > 0 && h.name_len <= MaxNameLength && h.data_len <= MaxDataLength &&
           (h.flags & ~FlagErased) == 0 && (!(h.flags & FlagErased) || h.data_len == 0) &&
           h.header_hash == RecordHeaderHash(h);
}

uint64_t NewId() {
    std::random_device rd;
    uint64_t id = 0;
    while (id == 0) {
        id = (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^
             uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
    }
    return id;
}

FileHeader MakeHeader(uint64_t id) {
    FileHeader h{};
    std::memcpy(h.magic, HeaderMagic, sizeof(h.magic));
    h.version = FormatVersion;
    h.header_size = HeaderSize;
    h.id = id;
    std::memcpy(h.port, PortName, std::min(sizeof(h.port), std::strlen(PortName)));
    h.created = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count());
    h.hash = XXH3_64bits(&h, offsetof(FileHeader, hash));
    return h;
}

/// 0: valid; 1: this port's pack with a damaged header; 2: this port's pack of another format version (`version`
/// set); 3: not this port's file (another magic, or another port's name in the header).
int CheckHeader(const FileHeader& h, uint32_t* version) {
    if (std::memcmp(h.magic, HeaderMagic, sizeof(h.magic)) != 0) {
        return 3;
    }
    if (h.hash != XXH3_64bits(&h, offsetof(FileHeader, hash))) {
        return 1;
    }
    char ours[sizeof(h.port)]{};
    std::memcpy(ours, PortName, std::min(sizeof(ours), std::strlen(PortName)));
    static constexpr char none[sizeof(h.port)]{};
    if (std::memcmp(h.port, ours, sizeof(ours)) != 0 && std::memcmp(h.port, none, sizeof(none)) != 0) {
        return 3;
    }
    *version = h.version;
    return h.version == FormatVersion && h.header_size == HeaderSize ? 0 : 2;
}

// ---- the OS file calls -------------------------------------------------------------------------------------------

#ifdef _WIN32
using Native = HANDLE;
const Native NoFile = INVALID_HANDLE_VALUE;

Native OpenNative(const std::filesystem::path& path, bool write, bool create, bool overlapped) {
    const DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0) | DELETE;
    // Every share mode: readers in other processes, and the rename over the pack while they read (POSIX rename).
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE h = CreateFileW(path.c_str(), access, share, nullptr, create ? CREATE_ALWAYS : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0), nullptr);
    if (h == INVALID_HANDLE_VALUE && !write) {
        // A read-only folder (or a reader without delete rights): plain read access.
        h = CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0), nullptr);
    }
    return h;
}

void CloseNative(Native& h) {
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        CloseHandle(h);
    }
    h = INVALID_HANDLE_VALUE;
}

struct ThreadEvent {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ThreadEvent() {
        if (event) {
            CloseHandle(event);
        }
    }
};

/// Positional read on any handle (overlapped or not); parallel reads on an overlapped handle do not wait for each
/// other.
bool ReadNative(Native h, uint64_t offset, void* dst, size_t size, bool overlapped) {
    thread_local ThreadEvent ev;
    auto* p = static_cast<uint8_t*>(dst);
    while (size > 0) {
        const DWORD chunk = DWORD(std::min<size_t>(size, 64u << 20));
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        ov.hEvent = overlapped ? ev.event : nullptr;
        DWORD got = 0;
        if (!ReadFile(h, p, chunk, &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(h, &ov, &got, TRUE)) {
                return false;
            }
        } else if (overlapped && !GetOverlappedResult(h, &ov, &got, TRUE)) {
            return false;
        }
        if (got == 0) {
            return false; // end of file
        }
        p += got;
        offset += got;
        size -= got;
    }
    return true;
}

bool WriteNative(Native h, uint64_t offset, const void* src, size_t size) {
    const auto* p = static_cast<const uint8_t*>(src);
    while (size > 0) {
        const DWORD chunk = DWORD(std::min<size_t>(size, 64u << 20));
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD put = 0;
        if (!WriteFile(h, p, chunk, &put, &ov) || put == 0) {
            return false;
        }
        p += put;
        offset += put;
        size -= put;
    }
    return true;
}

bool TruncateNative(Native h, uint64_t size) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = LONGLONG(size);
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof(info)) != 0;
}

bool SyncNative(Native h) {
    return FlushFileBuffers(h) != 0;
}

uint64_t SizeNative(Native h) {
    LARGE_INTEGER size{};
    return GetFileSizeEx(h, &size) ? uint64_t(size.QuadPart) : 0;
}

/// Renames `tmp` over `target`, also while other processes read the target (POSIX semantics, NTFS); on a drive
/// without them (FAT32/exFAT) the classic replace, which works while nobody else has the target open.
bool RenameOver(const std::filesystem::path& tmp, const std::filesystem::path& target) {
    HANDLE h = CreateFileW(tmp.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const std::wstring name = target.wstring();
        std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
        // FileRenameInfoEx (22): Flags in place of ReplaceIfExists.
        *reinterpret_cast<DWORD*>(info) = 0x1 /*REPLACE_IF_EXISTS*/ | 0x2 /*POSIX_SEMANTICS*/;
        info->RootDirectory = nullptr;
        info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, name.c_str(), info->FileNameLength);
        const BOOL ok = SetFileInformationByHandle(h, FILE_INFO_BY_HANDLE_CLASS(22), info, DWORD(buf.size()));
        CloseHandle(h);
        if (ok) {
            return true;
        }
    }
    return MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

/// The writer lock: <pack>.lock opened with no sharing. Windows lets it go when the process ends, however it ends.
Native TakeLock(const std::filesystem::path& path) {
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}
#else
using Native = int;
const Native NoFile = -1;

Native OpenNative(const std::filesystem::path& path, bool write, bool create, bool) {
    return ::open(path.c_str(), (write ? O_RDWR : O_RDONLY) | (create ? O_CREAT | O_TRUNC : 0) | O_CLOEXEC, 0644);
}
void CloseNative(Native& h) {
    if (h >= 0) {
        ::close(h);
    }
    h = -1;
}
bool ReadNative(Native h, uint64_t offset, void* dst, size_t size, bool) {
    auto* p = static_cast<uint8_t*>(dst);
    while (size > 0) {
        const ssize_t got = ::pread(h, p, size, off_t(offset));
        if (got <= 0) {
            return false;
        }
        p += got;
        offset += uint64_t(got);
        size -= size_t(got);
    }
    return true;
}
bool WriteNative(Native h, uint64_t offset, const void* src, size_t size) {
    const auto* p = static_cast<const uint8_t*>(src);
    while (size > 0) {
        const ssize_t put = ::pwrite(h, p, size, off_t(offset));
        if (put <= 0) {
            return false;
        }
        p += put;
        offset += uint64_t(put);
        size -= size_t(put);
    }
    return true;
}
bool TruncateNative(Native h, uint64_t size) {
    return ::ftruncate(h, off_t(size)) == 0;
}
bool SyncNative(Native h) {
    return ::fsync(h) == 0;
}
uint64_t SizeNative(Native h) {
    struct stat st{};
    return ::fstat(h, &st) == 0 ? uint64_t(st.st_size) : 0;
}
bool RenameOver(const std::filesystem::path& tmp, const std::filesystem::path& target) {
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        return false;
    }
    const int dir = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
    return true;
}
Native TakeLock(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
#endif

/// Writes a whole file through a native handle that was just created: the bytes, then a flush to the disk.
bool WriteWholeNew(const std::filesystem::path& path, const void* data, size_t size) {
    Native h = OpenNative(path, true, true, false);
    if (h == NoFile) {
        return false;
    }
    const bool ok = WriteNative(h, 0, data, size) && SyncNative(h);
    CloseNative(h);
    return ok;
}

std::filesystem::path TmpPath(const std::filesystem::path& path) {
    auto tmp = path;
    tmp += ".tmp";
    return tmp;
}

} // namespace

// ---- PackFile -----------------------------------------------------------------------------------------------------

#ifdef _WIN32
#define READ_H static_cast<HANDLE>(read_handle_)
#define WRITE_H static_cast<HANDLE>(write_handle_)
#else
#define READ_H read_fd_
#define WRITE_H write_fd_
#endif

PackFile::~PackFile() {
    Close();
}

std::string PackFile::Key(uint16_t type, std::string_view name) {
    std::string key;
    key.reserve(name.size() + 2);
    key.push_back(char(type & 0xFF));
    key.push_back(char(type >> 8));
    key.append(name);
    return key;
}

uint64_t PackFile::BodyHash(std::string_view name, const void* data, size_t size) {
    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);
    XXH3_64bits_update(state, name.data(), name.size());
    if (size) {
        XXH3_64bits_update(state, data, size);
    }
    const uint64_t hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return hash;
}

uint64_t PackFile::ReadId(const std::filesystem::path& path) {
    Native h = OpenNative(path, false, false, false);
    if (h == NoFile) {
        return 0;
    }
    FileHeader header{};
    uint32_t version = 0;
    const bool ok = ReadNative(h, 0, &header, sizeof(header), false) && CheckHeader(header, &version) == 0;
    CloseNative(h);
    return ok ? header.id : 0;
}

std::string PackFile::IdText() const {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(id_));
    return text;
}

bool PackFile::OpenHandles(bool writable) {
    CloseHandles();
#ifdef _WIN32
    read_handle_ = OpenNative(path_, false, false, true);
    if (read_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (writable) {
        write_handle_ = OpenNative(path_, true, false, false);
        if (write_handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
    }
#else
    read_fd_ = OpenNative(path_, false, false, false);
    if (read_fd_ < 0) {
        return false;
    }
    if (writable) {
        write_fd_ = OpenNative(path_, true, false, false);
        if (write_fd_ < 0) {
            return false;
        }
    }
#endif
    return true;
}

void PackFile::CloseHandles() {
#ifdef _WIN32
    HANDLE r = read_handle_ ? static_cast<HANDLE>(read_handle_) : INVALID_HANDLE_VALUE;
    HANDLE w = write_handle_ ? static_cast<HANDLE>(write_handle_) : INVALID_HANDLE_VALUE;
    CloseNative(r);
    CloseNative(w);
    read_handle_ = INVALID_HANDLE_VALUE;
    write_handle_ = INVALID_HANDLE_VALUE;
#else
    CloseNative(read_fd_);
    CloseNative(write_fd_);
#endif
}

bool PackFile::WriteHeaderFile(const std::filesystem::path& file, uint64_t id, uint64_t* written_size) {
    const FileHeader header = MakeHeader(id);
    if (!WriteWholeNew(file, &header, sizeof(header))) {
        return false;
    }
    if (written_size) {
        *written_size = sizeof(header);
    }
    return true;
}

bool PackFile::ReadAt(uint64_t offset, void* dst, size_t size) const {
    return ReadNative(READ_H, offset, dst, size, true);
}

bool PackFile::WriteAt(uint64_t offset, const void* src, size_t size) {
    if (test_write_limit) {
        const size_t allowed = std::min(size, test_write_limit(offset, size));
        if (allowed < size) {
            if (allowed) {
                WriteNative(WRITE_H, offset, src, allowed);
            }
            return false;
        }
    }
    return WriteNative(WRITE_H, offset, src, size);
}

bool PackFile::Truncate(uint64_t size) {
    return TruncateNative(WRITE_H, size);
}

OpenStats PackFile::Open(const std::filesystem::path& path, bool want_writer, bool compact) {
    const auto t0 = std::chrono::steady_clock::now();
    OpenStats stats{};
    Close();
    path_ = path;
    index_.clear();
    dead_bytes_ = 0;
    same_skipped_ = 0;
    damaged_ = false;
    id_ = 0;
    end_ = HeaderSize;
    const auto finish = [&](bool ok) {
        stats.ok = ok;
        open_ = ok;
        stats.writer = ok && writer_;
        stats.live = index_.size();
        stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) {
            CloseHandles();
        }
        return stats;
    };

    if (want_writer) {
        auto lock_path = path;
        lock_path += ".lock";
#ifdef _WIN32
        HANDLE lock = TakeLock(lock_path);
        if (lock != INVALID_HANDLE_VALUE) {
            lock_handle_ = lock;
            writer_ = true;
        }
#else
        lock_fd_ = TakeLock(lock_path);
        writer_ = lock_fd_ >= 0;
#endif
    }

    std::error_code ec;
    const bool exists = std::filesystem::is_regular_file(path, ec);
    if (!exists) {
        if (!writer_) {
            stats.error = "no pack yet";
            // A reader with nothing to read: open and empty (another process is making it).
            return finish(true);
        }
        if (!WriteHeaderFile(TmpPath(path), NewId(), nullptr) || !RenameOver(TmpPath(path), path)) {
            stats.error = "the pack could not be created";
            return finish(false);
        }
        stats.created = true;
    }
    if (!OpenHandles(writer_)) {
        stats.error = "the pack could not be opened";
        return finish(false);
    }
    FileHeader header{};
    uint32_t version = 0;
    const uint64_t on_disk = SizeNative(READ_H);
    int header_state = 1;
    if (ReadAt(0, &header, sizeof(header))) {
        header_state = CheckHeader(header, &version);
    } else if (on_disk > 0) {
        // Shorter than a header: this port's only if what is there begins like its magic (an empty file holds
        // nothing and is made new).
        std::vector<char> head(size_t(std::min<uint64_t>(on_disk, sizeof(header.magic))));
        header_state = ReadAt(0, head.data(), head.size()) &&
                               std::memcmp(head.data(), HeaderMagic, head.size()) == 0
                           ? 1
                           : 3;
    }
    if (header_state == 3) {
        // Another program's file of this name (forks or other ports may share the cache folder): never replaced,
        // written or removed; this game keeps no cache.
        stats.foreign = true;
        stats.error = "it is not this port's file (left as it is)";
        CloseHandles();
        return finish(false);
    }
    // A damaged header (not another version) over records: the records check themselves, so they are kept; the
    // pack gets a new header and id (a damaged pack's markers no longer count).
    const bool header_damaged = header_state == 1 && writer_ && on_disk > HeaderSize &&
                                std::memcmp(header.magic, HeaderMagic, sizeof(header.magic)) == 0;
    if (header_damaged) {
        header = MakeHeader(NewId());
    } else if (header_state != 0) {
        if (!writer_) {
            stats.error = header_state == 2 ? "another format version" : "not a pack";
            damaged_ = header_state == 1;
            CloseHandles();
            return finish(true);
        }
        stats.replaced = true;
        stats.old_version = header_state == 2 ? version : 0;
        stats.file_bytes = SizeNative(READ_H);
        if (!Clear()) {
            stats.error = "the pack could not be replaced";
            return finish(false);
        }
        return finish(true);
    }
    id_ = header.id;
    if (!Scan(stats)) {
        stats.error = "the pack could not be read";
        return finish(false);
    }
    if (header_damaged) {
        ++stats.bad_records;
        stats.bad_bytes += HeaderSize;
    }
    if (writer_) {
        if (stats.torn_bytes) {
            // The end the last session did not finish: cut off (the next record goes where it began).
            if (!Truncate(end_.load())) {
                stats.error = "the torn end could not be cut off";
                return finish(false);
            }
        }
        if (stats.bad_records) {
            // Records lost inside the file: whatever was prepared from them may be missing, so the prepared markers
            // (tied to the id) no longer count; the bad bytes go with the compaction.
            id_ = NewId();
        }
        const bool many_dead = stats.dead_bytes > 32ull * 1024 * 1024 && stats.dead_bytes * 4 > stats.file_bytes;
        if (compact && (stats.bad_records || many_dead)) {
            std::unique_lock lk{mutex_};
            std::scoped_lock wl{write_mutex_};
            stats.compacted = CompactLocked();
        }
        if (stats.bad_records && !stats.compacted) {
            // Not compacted (not asked for, or it failed: no room for the copy): the bad bytes stay (skipped at every
            // open), the new id still has to be in the file.
            FileHeader fresh = header;
            fresh.id = id_;
            fresh.hash = XXH3_64bits(&fresh, offsetof(FileHeader, hash));
            if (!WriteAt(0, &fresh, sizeof(fresh)) || !SyncNative(WRITE_H)) {
                stats.error = "the new id could not be written";
                return finish(false);
            }
        }
    } else if (stats.bad_records) {
        damaged_ = true;
    }
    return finish(true);
}

bool PackFile::Scan(OpenStats& stats) {
    const uint64_t size = SizeNative(READ_H);
    stats.file_bytes = size;
    std::vector<uint8_t> buf;
    uint64_t buf_off = 0;
    size_t buf_len = 0;
    // Makes [pos, pos + n) readable in buf (n must fit the file).
    const auto ensure = [&](uint64_t pos, size_t n) -> const uint8_t* {
        if (pos >= buf_off && pos + n <= buf_off + buf_len) {
            return buf.data() + (pos - buf_off);
        }
        const size_t want = size_t(std::min<uint64_t>(std::max<size_t>(n, 16u << 20), size - pos));
        if (buf.size() < want) {
            buf.resize(want);
        }
        if (!ReadAt(pos, buf.data(), want)) {
            buf_len = 0;
            return nullptr;
        }
        buf_off = pos;
        buf_len = want;
        return buf.data();
    };
    uint64_t pos = HeaderSize;
    uint64_t good_end = HeaderSize;
    uint64_t pending_bad_bytes = 0, pending_bad_records = 0;
    while (pos + RecordHeaderSize <= size) {
        const uint8_t* p = ensure(pos, RecordHeaderSize);
        if (!p) {
            return false;
        }
        RecordHeader h;
        std::memcpy(&h, p, sizeof(h));
        if (ValidRecordHeader(h)) {
            const uint64_t rec = uint64_t(RecordHeaderSize) + h.name_len + h.data_len;
            if (pos + rec > size) {
                break; // torn: the rest of the file is the end the last session did not finish
            }
            const uint8_t* r = ensure(pos, size_t(rec));
            if (!r) {
                return false;
            }
            const std::string_view name{reinterpret_cast<const char*>(r + RecordHeaderSize), h.name_len};
            if (BodyHash(name, r + RecordHeaderSize + h.name_len, h.data_len) == h.body_hash) {
                std::string key = Key(h.type, name);
                const auto it = index_.find(key);
                if (it != index_.end()) {
                    stats.dead_bytes += RecordHeaderSize + it->second.name_len + it->second.data_len;
                    stats.live_bytes -= it->second.data_len;
                }
                if (h.flags & FlagErased) {
                    if (it != index_.end()) {
                        index_.erase(it);
                    }
                    stats.dead_bytes += rec;
                } else {
                    index_.insert_or_assign(std::move(key), Entry{pos, h.name_len, h.data_len, h.body_hash});
                    stats.live_bytes += h.data_len;
                }
                ++stats.records;
                stats.bad_records += pending_bad_records;
                stats.bad_bytes += pending_bad_bytes;
                pending_bad_records = pending_bad_bytes = 0;
                pos += rec;
                good_end = pos;
                continue;
            }
            // The header is sound, its data is not: this record alone is dropped.
            ++pending_bad_records;
            pending_bad_bytes += rec;
            pos += rec;
            continue;
        }
        // Not a record header: look for the next one.
        uint64_t q = pos + 1;
        bool found = false;
        while (q + RecordHeaderSize <= size && !found) {
            const size_t span = size_t(std::min<uint64_t>(size - q, 16u << 20));
            const uint8_t* s = ensure(q, span);
            if (!s) {
                return false;
            }
            size_t i = 0;
            for (; i + RecordHeaderSize <= span; ++i) {
                if (s[i] == 'B' && s[i + 1] == 'S' && s[i + 2] == 'P' && s[i + 3] == 'K') {
                    RecordHeader c;
                    std::memcpy(&c, s + i, sizeof(c));
                    if (ValidRecordHeader(c)) {
                        found = true;
                        break;
                    }
                }
            }
            q += i;
            if (!found && span < RecordHeaderSize + 1) {
                break;
            }
        }
        if (!found) {
            break; // nothing more: the rest is the torn end
        }
        ++pending_bad_records;
        pending_bad_bytes += q - pos;
        pos = q;
    }
    stats.torn_bytes = size - good_end;
    end_ = good_end;
    dead_bytes_ = stats.dead_bytes;
    return true;
}

void PackFile::Close() {
    if (open_ && writer_) {
        Sync();
    }
    CloseHandles();
#ifdef _WIN32
    if (lock_handle_ && lock_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(lock_handle_));
    }
    lock_handle_ = nullptr;
#else
    CloseNative(lock_fd_);
#endif
    std::unique_lock lk{mutex_};
    index_.clear();
    open_ = false;
    writer_ = false;
}

bool PackFile::AppendRecord(uint16_t type, uint16_t flags, std::string_view name, const void* data, size_t size) {
    if (!open_ || !writer_ || name.empty() || name.size() > MaxNameLength || size > MaxDataLength) {
        return false;
    }
    RecordHeader h{};
    h.magic = RecordMagic;
    h.type = type;
    h.flags = flags;
    h.name_len = uint32_t(name.size());
    h.data_len = uint32_t(size);
    h.body_hash = BodyHash(name, data, size);
    h.header_hash = RecordHeaderHash(h);
    const size_t rec = RecordHeaderSize + name.size() + size;
    std::vector<uint8_t> buf(rec);
    std::memcpy(buf.data(), &h, sizeof(h));
    std::memcpy(buf.data() + RecordHeaderSize, name.data(), name.size());
    if (size) {
        std::memcpy(buf.data() + RecordHeaderSize + name.size(), data, size);
    }
    std::scoped_lock wl{write_mutex_};
    const uint64_t at = end_.load(std::memory_order_relaxed);
    if (!WriteAt(at, buf.data(), rec)) {
        // Disk full or refused: whatever part was written is cut off again (if that fails too, the next record
        // overwrites it, and the open drops what is left after the last good record).
        Truncate(at);
        return false;
    }
    {
        std::unique_lock lk{mutex_};
        std::string key = Key(type, name);
        const auto it = index_.find(key);
        if (it != index_.end()) {
            dead_bytes_ += RecordHeaderSize + it->second.name_len + it->second.data_len;
        }
        if (flags & FlagErased) {
            if (it != index_.end()) {
                index_.erase(it);
            }
            dead_bytes_ += rec;
        } else {
            index_.insert_or_assign(std::move(key), Entry{at, h.name_len, h.data_len, h.body_hash});
        }
        end_.store(at + rec, std::memory_order_release);
    }
    return true;
}

bool PackFile::Put(uint16_t type, std::string_view name, const void* data, size_t size) {
    if (!open_ || !writer_) {
        return false;
    }
    if (uint64_t have_size = 0, have_hash = 0;
        Find(type, name, &have_size, &have_hash) && have_size == size && have_hash == BodyHash(name, data, size)) {
        same_skipped_.fetch_add(1, std::memory_order_relaxed);
        return true; // already there, unchanged: a start that saves the same blobs again grows nothing
    }
    return AppendRecord(type, 0, name, data, size);
}

bool PackFile::Erase(uint16_t type, std::string_view name) {
    if (!Find(type, name, nullptr, nullptr)) {
        return true;
    }
    return AppendRecord(type, FlagErased, name, nullptr, 0);
}

bool PackFile::Find(uint16_t type, std::string_view name, uint64_t* size, uint64_t* hash) const {
    std::shared_lock lk{mutex_};
    const auto it = index_.find(Key(type, name));
    if (it == index_.end()) {
        return false;
    }
    if (size) {
        *size = it->second.data_len;
    }
    if (hash) {
        *hash = it->second.hash;
    }
    return true;
}

bool PackFile::Get(uint16_t type, std::string_view name, std::vector<uint8_t>& out) const {
    std::shared_lock lk{mutex_};
    const auto it = index_.find(Key(type, name));
    if (it == index_.end()) {
        return false;
    }
    const Entry e = it->second;
    std::vector<uint8_t> body(size_t(e.name_len) + e.data_len);
    if (!ReadAt(e.offset + RecordHeaderSize, body.data(), body.size())) {
        return false;
    }
    const std::string_view stored{reinterpret_cast<const char*>(body.data()), e.name_len};
    if (stored != name || BodyHash(stored, body.data() + e.name_len, e.data_len) != e.hash) {
        return false; // changed on the disk since the open: treated as missing (built and saved again)
    }
    out.assign(body.begin() + e.name_len, body.end());
    return true;
}

std::vector<std::string> PackFile::Names(uint16_t type) const {
    std::vector<std::string> names;
    std::shared_lock lk{mutex_};
    names.reserve(index_.size());
    for (const auto& [key, entry] : index_) {
        if (uint16_t(uint8_t(key[0]) | (uint16_t(uint8_t(key[1])) << 8)) == type) {
            names.emplace_back(key.substr(2));
        }
    }
    return names;
}

size_t PackFile::Count() const {
    std::shared_lock lk{mutex_};
    return index_.size();
}

bool PackFile::Sync() {
    if (!open_ || !writer_) {
        return false;
    }
    std::scoped_lock wl{write_mutex_};
    if (synced_end_ == end_.load(std::memory_order_relaxed)) {
        return true; // nothing written since the last flush (a flush of an unchanged file still costs the drive)
    }
    if (!SyncNative(WRITE_H)) {
        return false;
    }
    synced_end_ = end_.load(std::memory_order_relaxed);
    return true;
}

bool PackFile::ReplaceWith(const std::filesystem::path& tmp) {
    CloseHandles();
    synced_end_ = 0;
    const bool renamed = RenameOver(tmp, path_);
    if (!renamed) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }
    // Reopened either way (the old pack when the rename failed).
    return OpenHandles(writer_) && renamed;
}

bool PackFile::Clear() {
    if (!writer_) {
        // A reader cannot change the file: it forgets what it read (the cache is not usable for this build).
        std::unique_lock lk{mutex_};
        index_.clear();
        return true;
    }
    std::unique_lock lk{mutex_};
    std::scoped_lock wl{write_mutex_};
    const uint64_t new_id = NewId();
    const auto tmp = TmpPath(path_);
    if (!WriteHeaderFile(tmp, new_id, nullptr) || !ReplaceWith(tmp)) {
        // No rename over the pack (a FAT32/exFAT drive while another process reads it, or no room for the new
        // file): emptied in place. First cut to nothing, then the new header: a crash in between leaves a file
        // without a header, which the next open makes new. Readers elsewhere then fail their checksums (treated as
        // missing), never read wrong data.
        const FileHeader header = MakeHeader(new_id);
        if (!TruncateNative(WRITE_H, 0) || !WriteNative(WRITE_H, 0, &header, sizeof(header)) ||
            !SyncNative(WRITE_H)) {
            return false;
        }
    }
    id_ = new_id;
    index_.clear();
    dead_bytes_ = 0;
    end_ = HeaderSize;
    return true;
}

bool PackFile::Compact() {
    if (!writer_) {
        return false;
    }
    std::unique_lock lk{mutex_};
    std::scoped_lock wl{write_mutex_};
    return CompactLocked();
}

bool PackFile::CompactLocked() {
    const auto tmp = TmpPath(path_);
    Native out = OpenNative(tmp, true, true, false);
    if (out == NoFile) {
        return false;
    }
    // Copied in file order (the order they were written), checked on the way.
    std::vector<std::pair<const std::string*, Entry*>> order;
    order.reserve(index_.size());
    for (auto& [key, entry] : index_) {
        order.emplace_back(&key, &entry);
    }
    std::ranges::sort(order, {}, [](const auto& p) { return p.second->offset; });
    const FileHeader header = MakeHeader(id_);
    std::vector<uint8_t> chunk;
    chunk.reserve(8u << 20);
    chunk.insert(chunk.end(), reinterpret_cast<const uint8_t*>(&header),
                 reinterpret_cast<const uint8_t*>(&header) + sizeof(header));
    uint64_t written = 0;
    bool ok = true;
    std::vector<std::pair<const std::string*, uint64_t>> moved;
    moved.reserve(order.size());
    std::vector<const std::string*> lost;
    std::vector<uint8_t> rec;
    for (const auto& [key, entry] : order) {
        const size_t size = RecordHeaderSize + entry->name_len + entry->data_len;
        rec.resize(size);
        RecordHeader h{};
        const bool read = ReadAt(entry->offset, rec.data(), size);
        if (read) {
            std::memcpy(&h, rec.data(), sizeof(h));
        }
        if (!read || !ValidRecordHeader(h) ||
            BodyHash({reinterpret_cast<const char*>(rec.data() + RecordHeaderSize), entry->name_len},
                     rec.data() + RecordHeaderSize + entry->name_len, entry->data_len) != entry->hash) {
            lost.push_back(key); // damaged since the open: left out
            continue;
        }
        moved.emplace_back(key, written + chunk.size());
        chunk.insert(chunk.end(), rec.begin(), rec.end());
        if (chunk.size() >= (8u << 20)) {
            if (test_write_limit && test_write_limit(written, chunk.size()) < chunk.size()) {
                ok = false;
                break;
            }
            ok = WriteNative(out, written, chunk.data(), chunk.size());
            if (!ok) {
                break;
            }
            written += chunk.size();
            chunk.clear();
        }
    }
    if (ok && !chunk.empty()) {
        ok = !(test_write_limit && test_write_limit(written, chunk.size()) < chunk.size()) &&
             WriteNative(out, written, chunk.data(), chunk.size());
        written += chunk.size();
    }
    ok = ok && SyncNative(out);
    CloseNative(out);
    if (!ok || !ReplaceWith(tmp)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    for (const auto& [key, offset] : moved) {
        index_[*key].offset = offset;
    }
    for (const auto* key : lost) {
        const std::string copy = *key; // not a reference into the node being erased
        index_.erase(copy);
    }
    end_ = written;
    dead_bytes_ = 0;
    return true;
}

} // namespace Storage::Pack
