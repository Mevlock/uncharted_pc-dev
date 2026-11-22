// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "bbport_nan_guard.h"
#include "bbport_threads.h"

#include "video_core/cache_pack.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>

namespace {

// bbport (pack cache): the blobs live in one file, <cache folder>/shaders.pack (video_core/cache_pack.h), written by
// one thread in the order the game saves them (appends: no file is created per blob, so the antivirus looks at one
// file instead of ~32,000). Loads read the pack in parallel.
struct IoQueue {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::queue<std::packaged_task<void()>> requests;
};
IoQueue io_queue{};
std::atomic<u32> num_requests{};
std::atomic<u64> num_completed{};
// bbport: shader blobs that could not be saved (disk full, refused, or this game only reads the cache): no prepared
// marker after one.
std::atomic<u64> num_failed{};

// bbport: the slow-write state (ProcessIO). slow_since: the start of the first slow write of the current run of slow
// ones (0: the writes are not slow); last_slow_end: when the last slow write finished; busy_since: when the writer's
// current write began (0: idle). All steady-clock nanoseconds.
std::atomic<s64> slow_since{0}, last_slow_end{0};
std::atomic<s64> busy_since{0};
std::atomic<int> unthrottled{0};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// bbport (test only, off by default): BB_TEST_WRITE_DELAY=<n>:<ms>: the first n writes take <ms> longer (a stall as
// an antivirus scanning a new folder makes); n = 0: every write (a slow disk).
struct WriteDelay {
    u64 count = 0;
    u32 ms = 0;
};
const WriteDelay& TestWriteDelay() {
    static const WriteDelay delay = [] {
        WriteDelay d{};
        if (const char* env = std::getenv("BB_TEST_WRITE_DELAY")) {
            unsigned long long n = 0;
            unsigned ms = 0;
            if (std::sscanf(env, "%llu:%u", &n, &ms) == 2 && ms) {
                d.count = n;
                d.ms = ms;
                std::printf("Test: BB_TEST_WRITE_DELAY: %s writes take %u ms longer\n",
                            n ? std::to_string(n).c_str() : "all", ms);
            }
        }
        return d;
    }();
    return delay;
}
std::atomic<u64> num_started{};

Storage::Pack::PackFile pack;
constexpr const char* PackName = "shaders.pack";
// The pack's own record of when the folder cache was moved into it (file clock ticks): loose blob files no newer than
// that are leftovers of the move, removed without reading them again.
constexpr u16 InternalType = 0x100;
constexpr const char* FoldedName = "folded";
// Removes the old cache files (after the move) behind the game, at low priority.
std::jthread cleanup_thread;

u16 PackType(Storage::BlobType type) {
    return u16(type);
}

std::string MB(u64 bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f MB", double(bytes) / (1024.0 * 1024.0));
    return text;
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
    auto& io = io_queue;
    while (true) {
        std::packaged_task<void()> request{};
        {
            std::unique_lock lk{io.mutex};
            Common::CondvarWait(io.cv, lk, stoken, [&] { return !io.requests.empty(); });
            if (stoken.stop_requested()) {
                return;
            }
            if (io.requests.empty()) {
                continue;
            }
            request = std::move(io.requests.front());
            io.requests.pop();
        }
        if (request.valid()) {
            // bbport (issue #30): on a disk where one write takes over a second (an antivirus holding the file, or a
            // hard drive), the writes kept it busy while the game loaded its level from it and the loading screen
            // crawled for minutes. While the writes are slow, a short pause after each: the game's reads get the
            // disk, the cache still fills behind the game. BB_CACHE_SLOW_DISK=0: no pause. The slowness ends once a
            // second of writes has passed without a slow one. While the page or the exit waits for the writes
            // (SetUnthrottled) no game reads need the disk: no pause.
            static const bool slow_allowed = [] {
                const char* env = std::getenv("BB_CACHE_SLOW_DISK");
                return !(env && env[0] == '0');
            }();
            static std::atomic<bool> hinted{false};
            const bool pause = slow_allowed && slow_since.load(std::memory_order_relaxed) != 0 &&
                               unthrottled.load(std::memory_order_relaxed) == 0;
            const auto start = std::chrono::steady_clock::now();
            const s64 start_ns = NowNs();
            busy_since.store(start_ns, std::memory_order_relaxed);
            if (const auto& delay = TestWriteDelay(); delay.ms) {
                if (delay.count == 0 || num_started.fetch_add(1, std::memory_order_relaxed) < delay.count) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay.ms));
                }
            }
            request();
            request.get_future().wait();
            busy_since.store(0, std::memory_order_relaxed);
            const s64 end_ns = NowNs();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start).count();
            if (ms > 1000) {
                std::printf("Shader cache: one write took %lld ms (%u queued)\n", static_cast<long long>(ms),
                            num_requests.load());
                last_slow_end.store(end_ns, std::memory_order_relaxed);
                s64 none = 0;
                if (slow_since.compare_exchange_strong(none, start_ns)) {
                    if (!hinted.exchange(true)) {
                        std::printf("Shader cache: the writes are slow: a pause after each while they are, behind the "
                                    "game (BB_CACHE_SLOW_DISK=0: off). Excluding the game folder from the antivirus "
                                    "helps.\n");
                    } else {
                        std::printf("Shader cache: the writes are slow again: a pause after each while they are\n");
                    }
                }
            } else if (s64 since = slow_since.load(std::memory_order_relaxed);
                       since != 0 && end_ns - last_slow_end.load(std::memory_order_relaxed) >= 1000000000LL) {
                if (slow_since.compare_exchange_strong(since, 0)) {
                    std::printf("Shader cache: the writes are fast again after %.1f s\n",
                                double(end_ns - since) / 1e9);
                }
            }
            if (pause) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        ++num_completed;
        --num_requests;
    }
}

static void Enqueue(std::packaged_task<void()>&& request) {
    auto& io = io_queue;
    {
        std::scoped_lock lock{io.mutex};
        io.requests.emplace(std::move(request));
        ++num_requests;
    }
    io.cv.notify_one();
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

/// The blob type of an old cache file (by its extension), or -1.
static int LooseBlobType(const std::filesystem::path& file) {
    const auto ext = file.extension().string();
    for (const BlobType type :
         {BlobType::ShaderMeta, BlobType::ShaderBinary, BlobType::PipelineKey, BlobType::ShaderProfile}) {
        if (ext.size() > 1 && ext.substr(1) == GetBlobFileExtension(type)) {
            return int(type);
        }
    }
    return -1;
}

/// The prepared markers of the folder cache ("prepared_<list>", one line) get the pack's id as their second line:
/// what they were written for is in the pack now.
static void TieMarkersToPack(const std::filesystem::path& folder) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("prepared_") || name.ends_with(".tmp") || !entry.is_regular_file(ec)) {
            continue;
        }
        std::string stamp, second;
        {
            std::ifstream in{entry.path()};
            if (!std::getline(in, stamp) || std::getline(in, second)) {
                continue; // empty, or already tied
            }
        }
        auto tmp = entry.path();
        tmp += ".tmp";
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            out << stamp << "\npack " << pack.IdText() << "\n";
        }
        std::filesystem::rename(tmp, entry.path(), ec);
    }
}

/// bbport (pack cache): the old cache files found by DataBase::Open (moved by DataBase::MoveFolderCache).
struct LooseFile {
    std::filesystem::path path;
    int type;  ///< BlobType, or -1 for an earlier build's unfinished write
    bool fold; ///< read and moved in (else a leftover of an earlier move, removed unread)
};
std::vector<LooseFile> loose_files;
u32 loose_to_fold = 0;

/// Lists the folder cache of an earlier build (one file per blob) beside the pack. The time of an earlier move is kept
/// in the pack: leftover files no newer than it whose blob the pack holds are removed without reading them; any other
/// (an earlier build run on this folder since, a copied-in old cache) is moved in again.
static void ListFolderCache(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    loose_files.clear();
    loose_to_fold = 0;
    fs::file_time_type::rep folded_at = 0;
    if (std::vector<u8> data; pack.Get(InternalType, FoldedName, data) && data.size() == sizeof(folded_at)) {
        std::memcpy(&folded_at, data.data(), sizeof(folded_at));
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto& path = entry.path();
        const auto name = path.filename().string();
        if (name.starts_with(PackName)) {
            continue; // the pack and its lock
        }
        if (const int type = LooseBlobType(path); type >= 0) {
            const auto written = entry.last_write_time(ec).time_since_epoch().count();
            const bool leftover = folded_at != 0 && written <= folded_at &&
                                  pack.Find(PackType(BlobType(type)), path.stem().string(), nullptr, nullptr);
            loose_files.push_back({path, type, !leftover});
            loose_to_fold += leftover ? 0 : 1;
        } else if (path.extension() == ".tmp" && !name.starts_with("prepared_")) {
            loose_files.push_back({path, -1, false});
        }
    }
}

/// Removes the old files behind the game, at low priority (a later start removes what is left).
static void RemoveLooseFilesBehindTheGame() {
    std::vector<std::filesystem::path> paths;
    paths.reserve(loose_files.size());
    for (auto& f : loose_files) {
        paths.push_back(std::move(f.path));
    }
    loose_files.clear();
    loose_to_fold = 0;
    cleanup_thread = std::jthread{[paths = std::move(paths)](std::stop_token stoken) {
        Common::SetCurrentThreadName("bb:CacheCleanup");
        BbThreads::MakeBelowNormal();
        const auto start = std::chrono::steady_clock::now();
        size_t removed = 0;
        for (const auto& path : paths) {
            if (stoken.stop_requested()) {
                break;
            }
            std::error_code error;
            removed += std::filesystem::remove(path, error) ? 1 : 0;
        }
        std::printf("Shader cache: %zu of %zu old cache files removed in %.1f s%s\n", removed, paths.size(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    removed < paths.size() ? " (the rest at the next start)" : "");
    }};
}

u32 DataBase::FolderCacheToMove() const {
    return loose_to_fold;
}

/// bbport (pack cache): the folder cache of an earlier build moves into the pack: every blob file is read and
/// appended (unless the pack already holds the same), the pack is flushed, then opened a second time from the disk
/// (every record read and checked) and each moved blob looked up there; only then is the move's time recorded, the
/// old prepared markers tied to the pack, and the old files removed (behind the game). Anything short of that keeps
/// the old files (the next start tries again).
bool DataBase::MoveFolderCache(const std::function<void(u32 done, u32 total)>& progress) {
    namespace fs = std::filesystem;
    if (!opened || !pack.IsWriter() || loose_files.empty()) {
        return true;
    }
    if (loose_to_fold == 0) {
        std::printf("Shader cache: %zu old cache files left from the move into %s: removed behind the game\n",
                    loose_files.size(), PackName);
        RemoveLooseFilesBehindTheGame();
        return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto start_time = fs::file_time_type::clock::now().time_since_epoch().count();
    std::vector<size_t> to_fold;
    for (size_t i = 0; i < loose_files.size(); ++i) {
        if (loose_files[i].fold) {
            to_fold.push_back(i);
        }
    }
    const u32 total = u32(to_fold.size());
    std::printf("Shader cache: moving %u cache files of an earlier build into %s (only this once)\n", total,
                PackName);
    std::fflush(stdout);
    std::atomic<u64> moved{0}, same{0}, unreadable{0}, failed{0}, bytes{0};
    std::atomic<u32> done{0};
    std::vector<u64> hashes(loose_files.size(), 0);
    if (progress) {
        progress(0, total);
    }
    {
        // Read in parallel (as the preload read them), appended in whatever order they come.
        const u32 threads = std::clamp<u32>(BbThreads::Available() > 2 ? BbThreads::Available() - 2 : 1, 1, 16);
        std::atomic<size_t> next{0};
        std::vector<std::jthread> pool;
        for (u32 t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                for (size_t k; (k = next.fetch_add(1)) < to_fold.size();) {
                    struct Count {
                        std::atomic<u32>& n;
                        const std::function<void(u32, u32)>& progress;
                        u32 total;
                        ~Count() {
                            const u32 now = n.fetch_add(1, std::memory_order_relaxed) + 1;
                            if (progress) {
                                progress(now, total);
                            }
                        }
                    } count{done, progress, total};
                    auto& file = loose_files[to_fold[k]];
                    std::vector<u8> data;
                    {
                        const Common::FS::IOFile in{file.path, Common::FS::FileAccessMode::Read};
                        if (!in.IsOpen()) {
                            ++unreadable;
                            continue;
                        }
                        data.resize(in.GetSize());
                        if (in.Read(data) != data.size()) {
                            ++unreadable;
                            continue;
                        }
                    }
                    const auto name = file.path.stem().string();
                    const u16 type = PackType(BlobType(file.type));
                    const u64 hash = Pack::PackFile::BodyHash(name, data.data(), data.size());
                    hashes[to_fold[k]] = hash;
                    u64 have_hash = 0;
                    if (pack.Find(type, name, nullptr, &have_hash) && have_hash == hash) {
                        ++same;
                        continue;
                    }
                    if (pack.Put(type, name, data.data(), data.size())) {
                        ++moved;
                        bytes += data.size();
                    } else {
                        ++failed;
                    }
                }
            });
        }
    } // joins
    if (progress) {
        progress(0, 0); // the move is over
    }
    if (failed || unreadable) {
        std::printf("Shader cache: moving the old cache files into %s: %llu could not be saved, %llu not read: the "
                    "old files are kept\n",
                    PackName, static_cast<unsigned long long>(failed.load()),
                    static_cast<unsigned long long>(unreadable.load()));
        return false;
    }
    if (!pack.Sync()) {
        std::printf("Shader cache: %s could not be flushed: the old cache files are kept\n", PackName);
        return false;
    }
    {
        // A second, read-only open of the pack from the disk (this game keeps its own open, for the queued writes).
        Pack::PackFile check;
        const auto st = check.Open(pack.Path(), false);
        bool verified = st.ok && st.bad_records == 0 && st.torn_bytes == 0;
        for (const size_t i : to_fold) {
            if (!verified) {
                break;
            }
            u64 hash = 0;
            verified = check.Find(PackType(BlobType(loose_files[i].type)), loose_files[i].path.stem().string(),
                                  nullptr, &hash) &&
                       hash == hashes[i];
        }
        if (!verified) {
            std::printf("Shader cache: %s did not read back after the move: the old cache files are kept\n",
                        PackName);
            return false;
        }
    }
    if (!pack.Put(InternalType, FoldedName, &start_time, sizeof(start_time)) || !pack.Sync()) {
        std::printf("Shader cache: %s could not be written after the move: the old cache files are kept\n", PackName);
        return false;
    }
    TieMarkersToPack(directory);
    std::printf("Shader cache: moved %llu cache files (%s) into %s in %.1f s (%llu were in it already); the %zu old "
                "files are removed behind the game\n",
                static_cast<unsigned long long>(moved.load()), MB(bytes.load()).c_str(), PackName,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                static_cast<unsigned long long>(same.load()), loose_files.size());
    RemoveLooseFilesBehindTheGame();
    return true;
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    // Offline perf build (feature/native-batch): its own cache. Its shader profile differs from
    // the release builds', so sharing one cache made each build clear the other's at every start.
    const std::string perf_suffix = "-perf";
    cache_path = GetUserPath(PathType::CacheDir) /
                 (std::string{game_info.GameSerial()} + BbNanGuard::ShaderCacheSuffix() + perf_suffix);
    std::error_code ec;
    if (!std::filesystem::exists(cache_path, ec)) {
        std::filesystem::create_directories(cache_path, ec);
    }
    directory = cache_path;

    const auto stats = pack.Open(cache_path / PackName, true);
    if (!stats.ok) {
        std::printf("Shader cache: %s could not be opened (%s): shaders are compiled again at each start and "
                    "nothing is saved\n",
                    PackName, stats.error.c_str());
    } else {
        if (stats.replaced) {
            if (stats.old_version) {
                std::printf("Shader cache: %s was of another format (version %u): it is made new\n", PackName,
                            stats.old_version);
            } else {
                std::printf("Shader cache: %s was not readable (its header is damaged): it is made new\n", PackName);
            }
        }
        if (stats.torn_bytes) {
            std::printf("Shader cache: the last session's unfinished write cut off (%llu bytes at the end)\n",
                        static_cast<unsigned long long>(stats.torn_bytes));
        }
        if (stats.bad_records) {
            std::printf("Shader cache: %llu damaged records dropped (%llu KiB): what they held is built again, and the "
                        "shader preparation checks the cache again%s\n",
                        static_cast<unsigned long long>(stats.bad_records),
                        static_cast<unsigned long long>(stats.bad_bytes / 1024),
                        stats.writer ? "" : " (this game only reads it: not repaired)");
        }
        std::printf("Shader cache: %s: %llu blobs (%s) read and checked in %.0f ms%s%s\n", PackName,
                    static_cast<unsigned long long>(stats.live), MB(stats.live_bytes).c_str(), stats.ms,
                    stats.created ? " (new)" : "", stats.compacted ? ", compacted" : "");
        if (!stats.writer) {
            std::printf("Shader cache: another game has the cache open for writing: this one only reads it (what it "
                        "compiles is not saved)\n");
        } else {
            std::error_code rm;
            std::filesystem::remove(cache_path / (std::string{PackName} + ".tmp"), rm);
            ListFolderCache(cache_path); // moved by MoveFolderCache, with the preparation page
        }
    }

    io_worker = std::jthread{[](std::stop_token stoken) { ProcessIO(stoken); }};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    if (io_worker.joinable()) {
        io_worker.request_stop();
        io_worker.join();
    }
    if (cleanup_thread.joinable()) {
        cleanup_thread.request_stop();
        cleanup_thread.join();
    }
    pack.Close();

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

/// Queued for the writer thread: appended to the pack in the order saved.
// bbport (pack cache): the session's appends, for the exit line (PrintWriteStats).
static std::atomic<u64> appended{0}, appended_bytes{0}, append_ns{0}, append_max_ns{0}, appends_over_5ms{0};

static void QueueWrite(BlobType type, const std::string& name, std::vector<u8>&& data) {
    auto request = std::packaged_task<void()>{[type, name, data = std::move(data)]() {
        const s64 t0 = NowNs();
        const bool ok = pack.Put(PackType(type), name, data.data(), data.size());
        const u64 ns = u64(NowNs() - t0);
        if (ok) {
            ++appended;
            appended_bytes += data.size();
            append_ns += ns;
            appends_over_5ms += ns > 5000000 ? 1 : 0;
            for (u64 max = append_max_ns.load(); ns > max && !append_max_ns.compare_exchange_weak(max, ns);) {
            }
        }
        if (!ok) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true)) {
                std::printf("Shader cache: %s.%s not saved (%s): the next start prepares what is missing\n",
                            name.c_str(), GetBlobFileExtension(type).c_str(),
                            !pack.IsOpen()    ? "the cache file is not open"
                            : pack.IsWriter() ? "the write failed: is the disk full?"
                                              : "this game only reads the cache");
            }
            ++num_failed;
        }
    }};
    Enqueue(std::move(request));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }
    QueueWrite(type, name, std::move(data));
    return true;
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }
    std::vector<u8> bytes(data.size() * sizeof(u32));
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), data.data(), bytes.size());
    }
    QueueWrite(type, name, std::move(bytes));
    return true;
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }
    if (!pack.Get(PackType(type), name, data)) {
        data.clear();
    }
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }
    std::vector<u8> bytes;
    if (!pack.Get(PackType(type), name, bytes)) {
        data.clear();
        return;
    }
    data.resize(bytes.size() / sizeof(u32));
    if (!data.empty()) {
        std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
    }
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    if (!opened) {
        return;
    }
    for (const auto& name : pack.Names(PackType(type))) {
        std::vector<u8> data;
        if (pack.Get(PackType(type), name, data)) {
            func(std::move(data));
        }
    }
}

std::vector<std::string> DataBase::ListBlobNames(BlobType type) {
    if (!opened) {
        return {};
    }
    return pack.Names(PackType(type));
}

u32 DataBase::PendingWrites() const {
    return num_requests.load();
}

u64 DataBase::FailedWrites() const {
    return num_failed.load();
}

u64 DataBase::CompletedWrites() const {
    return num_completed.load();
}

double DataBase::SlowForSeconds() const {
    const s64 now = NowNs();
    if (const s64 since = slow_since.load(std::memory_order_relaxed)) {
        return double(now - since) / 1e9;
    }
    const s64 busy = busy_since.load(std::memory_order_relaxed);
    return busy && now - busy > 1000000000LL ? double(now - busy) / 1e9 : 0.0;
}

void DataBase::SetUnthrottled(bool on) {
    unthrottled.fetch_add(on ? 1 : -1);
}

bool DataBase::WaitForWrites(double stall_s, const std::function<bool(u32 left)>& tick) {
    u64 done = num_completed.load();
    auto progress = std::chrono::steady_clock::now(), last_tick = progress - std::chrono::seconds(1);
    while (opened && num_requests.load() != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (const u64 d = num_completed.load(); d != done) {
            done = d;
            progress = now;
        } else if (std::chrono::duration<double>(now - progress).count() >= stall_s) {
            return false;
        }
        if (tick && now - last_tick >= std::chrono::milliseconds(250)) {
            last_tick = now;
            if (!tick(num_requests.load())) {
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Sync();
    return num_requests.load() == 0;
}

void DataBase::Flush(u32 max_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
    while (opened && num_requests.load() != 0 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Sync();
}

void DataBase::PrintWriteStats() const {
    std::printf("Shader cache: this session appended %llu blobs (%.1f MB) to %s: %.0f ms of writes in all, the "
                "longest %.1f ms, %llu over 5 ms; %llu not saved\n",
                static_cast<unsigned long long>(appended.load()), double(appended_bytes.load()) / (1024.0 * 1024.0),
                PackName, double(append_ns.load()) / 1e6, double(append_max_ns.load()) / 1e6,
                static_cast<unsigned long long>(appends_over_5ms.load()),
                static_cast<unsigned long long>(num_failed.load()));
    std::fflush(stdout);
}

bool DataBase::Sync() {
    if (!opened || !pack.IsWriter()) {
        return false;
    }
    const s64 t0 = NowNs();
    const bool ok = pack.Sync();
    if (const double ms = double(NowNs() - t0) / 1e6; ms >= 20.0) {
        std::printf("Shader cache: flushing %s to the disk took %.0f ms\n", PackName, ms);
    }
    return ok;
}

bool DataBase::CanSave() const {
    return pack.IsOpen() && pack.IsWriter();
}

std::string DataBase::PackIdText() const {
    if (!pack.IsOpen() || pack.Damaged() || pack.Id() == 0) {
        return {};
    }
    return pack.IdText();
}

void DataBase::Clear() {
    if (!opened) {
        return;
    }
    const size_t blobs = pack.Count();
    if (!pack.Clear()) {
        std::printf("Shader cache: %s could not be made new: shaders are compiled again\n", PackName);
    }
    // The rest of the folder belongs to the old cache too (the driver's cache data, prepared markers, old blob files).
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with(PackName) || !entry.is_regular_file(ec)) {
            continue;
        }
        if (std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} blobs, {} other files): it is rebuilt for this build", blobs,
                removed);
}

void DataBase::Remove(BlobType type, const std::string& name) {
    if (!opened) {
        return;
    }
    auto request = std::packaged_task<void()>{[type, name]() {
        if (pack.IsWriter()) {
            pack.Erase(PackType(type), name);
        }
    }};
    Enqueue(std::move(request));
}

void DataBase::FinishPreload() {}

} // namespace Storage
