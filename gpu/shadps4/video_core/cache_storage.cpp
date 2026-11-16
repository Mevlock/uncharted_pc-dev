// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "bbport_nan_guard.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>

namespace {

// bbport: several writer threads (a first shader preparation writes ~25,000 files, 27 s on one thread). A file always
// goes to the same thread (by its path), so the writes and removals of one file keep their order.
struct IoQueue {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::queue<std::packaged_task<void()>> requests;
};
std::array<IoQueue, Storage::DataBase::IoWorkers> io_queues{};
std::atomic<u32> num_requests{};
std::atomic<u64> num_completed{};
// bbport: shader files that could not be saved (create, write or rename failed): no prepared marker after one.
std::atomic<u64> num_failed{};

// bbport: the slow-write state (ProcessIO). slow_since: the start of the first slow write of the current run of slow
// ones (0: the writes are not slow); last_slow_end: when the last slow write finished; busy_since: when each writer's
// current write began (0: idle). All steady-clock nanoseconds.
std::atomic<s64> slow_since{0}, last_slow_end{0};
std::array<std::atomic<s64>, Storage::DataBase::IoWorkers> busy_since{};
std::atomic<int> unthrottled{0};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// bbport: where a slow write spent its time (set by the write itself, on its writer thread), for the slow-write line.
struct WritePhases {
    double create_ms, write_ms, rename_ms;
};
thread_local WritePhases t_write_phases{};

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

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken, u32 index) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
    auto& io = io_queues[index];
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
            // bbport (issue #30): on a disk where one write takes over a second (an antivirus scanning each
            // new file, or a hard drive), four writers kept it busy while the game loaded its level from it and
            // the loading screen crawled for minutes. While the writes are slow, one write at a time with a
            // short pause after it: the game's reads get the disk, the cache still fills behind the game (each
            // file keeps its writer, so its writes stay in order). BB_CACHE_SLOW_DISK=0: as before.
            // Not latched (0.3.6.57): a player's first start (NVMe SSD) had its first four writes held 3.6 s at
            // once, most likely an antivirus looking at the new folder, then 1.4 ms per file; the session stayed at
            // under 50 writes a second and 15,590 shader files were still unsaved at the exit. The slowness ends once
            // a second of writes has passed without a slow one. While the page or the exit waits for the writes
            // (SetUnthrottled) no game reads need the disk: no throttle.
            static std::mutex slow_lane;
            static const bool slow_allowed = [] {
                const char* env = std::getenv("BB_CACHE_SLOW_DISK");
                return !(env && env[0] == '0');
            }();
            static std::atomic<bool> hinted{false};
            std::unique_lock<std::mutex> lane{slow_lane, std::defer_lock};
            if (slow_allowed && slow_since.load(std::memory_order_relaxed) != 0 &&
                unthrottled.load(std::memory_order_relaxed) == 0) {
                lane.lock();
            }
            const auto start = std::chrono::steady_clock::now();
            const s64 start_ns = NowNs();
            busy_since[index].store(start_ns, std::memory_order_relaxed);
            t_write_phases = {};
            if (const auto& delay = TestWriteDelay(); delay.ms) {
                if (delay.count == 0 || num_started.fetch_add(1, std::memory_order_relaxed) < delay.count) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay.ms));
                }
            }
            request();
            request.get_future().wait();
            busy_since[index].store(0, std::memory_order_relaxed);
            const s64 end_ns = NowNs();
            // A write over a second: Windows held the file call (seen at an exit after a preparation, all writers
            // stuck in create/rename; an antivirus scanning each new file is the likely cause).
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start).count();
            if (ms > 1000) {
                std::printf("Shader cache: one write took %lld ms (writer %u, %u queued; create %.0f ms, write %.0f, "
                            "close and rename %.0f)\n",
                            static_cast<long long>(ms), index, num_requests.load(), t_write_phases.create_ms,
                            t_write_phases.write_ms, t_write_phases.rename_ms);
                last_slow_end.store(end_ns, std::memory_order_relaxed);
                s64 none = 0;
                if (slow_since.compare_exchange_strong(none, start_ns)) {
                    if (!hinted.exchange(true)) {
                        std::printf("Shader cache: the writes are slow: one at a time while they are, behind the game "
                                    "(BB_CACHE_SLOW_DISK=0: off). Excluding the game folder from the antivirus "
                                    "helps.\n");
                    } else {
                        std::printf("Shader cache: the writes are slow again: one at a time while they are\n");
                    }
                }
            } else if (s64 since = slow_since.load(std::memory_order_relaxed);
                       since != 0 && end_ns - last_slow_end.load(std::memory_order_relaxed) >= 1000000000LL) {
                if (slow_since.compare_exchange_strong(since, 0)) {
                    std::printf("Shader cache: the writes are fast again after %.1f s: four at a time\n",
                                double(end_ns - since) / 1e9);
                }
            }
            if (lane.owns_lock()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        ++num_completed;
        --num_requests;
    }
}

/// The writer of a file: always the same one for a path (one writer for an archive: miniz is not thread-safe).
static void Enqueue(const std::filesystem::path& path, std::packaged_task<void()>&& request) {
    const u32 index = EmulatorSettings.IsPipelineCacheArchived()
                          ? 0
                          : u32(std::filesystem::hash_value(path) % DataBase::IoWorkers);
    auto& io = io_queues[index];
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

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    // Offline perf build (feature/native-batch): its own cache. Its shader profile differs from
    // the release builds', so sharing one cache made each build clear the other's at every start.
    const std::string perf_suffix = "-perf";
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{std::string{game_info.GameSerial()} + BbNanGuard::ShaderCacheSuffix() + perf_suffix}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) /
                     (std::string{game_info.GameSerial()} + BbNanGuard::ShaderCacheSuffix() + perf_suffix);
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        directory = cache_path;
    }

    const u32 workers = EmulatorSettings.IsPipelineCacheArchived() ? 1 : IoWorkers;
    for (u32 i = 0; i < workers; ++i) {
        io_workers[i] = std::jthread{[i](std::stop_token stoken) { ProcessIO(stoken, i); }};
    }
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    for (auto& worker : io_workers) {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
    }

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                // bbport: write beside and rename into place, so a game that ends during the
                // write (the queue is not drained at exit) never leaves a truncated blob that
                // the next start would load.
                using namespace Common::FS;
                auto temp{path};
                temp += ".tmp";
                const auto since = [](std::chrono::steady_clock::time_point from) {
                    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
                };
                const auto t0 = std::chrono::steady_clock::now();
                auto t2 = t0;
                {
                    const auto file = IOFile{temp, FileAccessMode::Create};
                    t_write_phases.create_ms = since(t0);
                    const auto t1 = std::chrono::steady_clock::now();
                    if (!file.IsOpen() || file.Write(v) != v.size()) {
                        LOG_WARNING(Render, "Cache file {} not written", temp.string());
                        ++num_failed;
                        return;
                    }
                    t_write_phases.write_ms = since(t1);
                    t2 = std::chrono::steady_clock::now();
                }
                // bbport: Windows refuses to replace a file another thread has open (the preload
                // reads it): retry briefly instead of keeping the old file beside the new one of
                // the shader's other half (the meta checksum catches a pair that still differs).
                std::error_code error;
                for (int attempt = 0; attempt < 20; ++attempt) {
                    std::filesystem::rename(temp, path, error);
                    if (!error) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (error) {
                    LOG_WARNING(Render, "Cache file {} not replaced: {}", path.string(),
                                error.message());
                    std::filesystem::remove(temp, error);
                    ++num_failed;
                }
                t_write_phases.rename_ms = since(t2);
            }
        }};
        auto queued{path_};
        queued.replace_extension(GetBlobFileExtension(type));
        Enqueue(queued, std::move(request));
    }
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
    }
}

std::vector<std::string> DataBase::ListBlobNames(BlobType type) {
    std::vector<std::string> names;
    if (!opened) {
        return names;
    }
    const auto ext = "." + GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            const std::string entry{file_name.data()};
            if (entry.ends_with(ext)) {
                names.push_back(entry.substr(0, entry.size() - ext.size()));
            }
        }
        return names;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.path().extension().string() == ext) {
            names.push_back(entry.path().stem().string());
        }
    }
    return names;
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
    s64 oldest = 0;
    for (const auto& busy : busy_since) {
        if (const s64 b = busy.load(std::memory_order_relaxed); b && (!oldest || b < oldest)) {
            oldest = b;
        }
    }
    return oldest && now - oldest > 1000000000LL ? double(now - oldest) / 1e9 : 0.0;
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
    return num_requests.load() == 0;
}

void DataBase::Flush(u32 max_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
    while (opened && num_requests.load() != 0 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::Remove(BlobType type, const std::string& name) {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    auto path = cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    {
        auto request = std::packaged_task<void()>{[path]() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }};
        Enqueue(path, std::move(request));
    }
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
