// SPDX-License-Identifier: GPL-2.0-or-later
// Background compiler queue, from the God of War III port (FelipePrado0/gow3_pc, a fork of this
// port): graphics pipelines are built on worker threads and draws skip until theirs is ready.
#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Vulkan {

/// Bounded compiler queue. Stop drains accepted work before Vulkan resources are released.
class AsyncCompiler {
public:
    explicit AsyncCompiler(unsigned count, size_t capacity = 64,
                           std::function<void(unsigned)> initialize = {})
        : capacity{capacity} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, i, initialize] {
                worker_index = i;
                if (initialize) {
                    initialize(i);
                }
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock{mutex};
                        cv.wait(lock, [&] { return stopping || !tasks.empty(); });
                        if (tasks.empty()) {
                            return;
                        }
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                }
            });
        }
    }
    ~AsyncCompiler() {
        Stop();
    }

    bool Submit(std::function<void()> task) {
        {
            std::scoped_lock lock{mutex};
            if (stopping || tasks.size() >= capacity) {
                return false;
            }
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
        return true;
    }
    bool HasCapacity() {
        std::scoped_lock lock{mutex};
        return !stopping && tasks.size() < capacity;
    }
    static unsigned WorkerIndex() {
        return worker_index;
    }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    size_t capacity;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    static inline thread_local unsigned worker_index = 0;
    bool stopping = false;
    std::vector<std::thread> workers;
};

/// One job per cache entry; a failure stays a failure until the next session.
template <typename T>
class AsyncJob {
public:
    template <typename F>
    bool Start(AsyncCompiler& compiler, F&& function) {
        if (started) {
            return false;
        }
        auto task = std::make_shared<std::packaged_task<T()>>(std::forward<F>(function));
        auto completion = task->get_future();
        if (!compiler.Submit([task] { (*task)(); })) {
            return false;
        }
        future = std::move(completion);
        started = true;
        return true;
    }
    bool Failed() const {
        return failed;
    }
    const std::string& Error() const {
        return error;
    }
    void Wait() const {
        if (future.valid()) {
            future.wait();
        }
    }
    std::optional<T> Poll() {
        if (!future.valid() ||
            future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return std::nullopt;
        }
        try {
            return future.get();
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown compiler error";
        }
        failed = true;
        return std::nullopt;
    }

private:
    std::future<T> future;
    bool started = false;
    bool failed = false;
    std::string error;
};

} // namespace Vulkan
