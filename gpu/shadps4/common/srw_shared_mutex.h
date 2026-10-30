// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <shared_mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

/// bbport (BB_SRW_LOCKS=1): a shared mutex over a Windows slim reader/writer lock. libc++'s
/// std::shared_mutex is a mutex and two condition variables: every shared lock takes and releases
/// that mutex, so readers on several threads (the GPU thread, the draw-preparation workers)
/// queue behind each other. An SRW lock takes a shared lock with one atomic operation. Same
/// rules as std::shared_mutex: not recursive, no upgrade. Chosen once per process.
/// `Switch` 0: BB_SRW_LOCKS (pipeline cache); 1: BB_SRW_RANGES (the buffer cache's GPU-modified ranges).
template <int Switch = 0>
class SrwSharedMutexT {
public:
    void lock() {
#ifdef _WIN32
        if (Srw()) {
            AcquireSRWLockExclusive(&srw);
            return;
        }
#endif
        fallback.lock();
    }
    bool try_lock() {
#ifdef _WIN32
        if (Srw()) {
            return TryAcquireSRWLockExclusive(&srw) != 0;
        }
#endif
        return fallback.try_lock();
    }
    void unlock() {
#ifdef _WIN32
        if (Srw()) {
            ReleaseSRWLockExclusive(&srw);
            return;
        }
#endif
        fallback.unlock();
    }
    void lock_shared() {
#ifdef _WIN32
        if (Srw()) {
            AcquireSRWLockShared(&srw);
            return;
        }
#endif
        fallback.lock_shared();
    }
    bool try_lock_shared() {
#ifdef _WIN32
        if (Srw()) {
            return TryAcquireSRWLockShared(&srw) != 0;
        }
#endif
        return fallback.try_lock_shared();
    }
    void unlock_shared() {
#ifdef _WIN32
        if (Srw()) {
            ReleaseSRWLockShared(&srw);
            return;
        }
#endif
        fallback.unlock_shared();
    }

private:
    static bool Srw() {
        static const bool on = [] {
            const char* env = std::getenv(Switch == 0 ? "BB_SRW_LOCKS" : "BB_SRW_RANGES");
            return env && env[0] == '1';
        }();
        return on;
    }
#ifdef _WIN32
    SRWLOCK srw = SRWLOCK_INIT;
#endif
    std::shared_mutex fallback;
};

using SrwSharedMutex = SrwSharedMutexT<0>;

} // namespace Common
