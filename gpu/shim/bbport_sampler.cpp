// SPDX-License-Identifier: GPL-2.0-or-later
// bbport diagnosis: BB_SAMPLE_THREADS=1 (Windows). A thread samples where the GPU threads run,
// about once per millisecond: it suspends each one, reads its instruction pointer and resumes it
// (nothing is allocated while a thread is suspended: it may hold the heap lock). Every 10 s the
// log gets each thread's busiest addresses as module+offset; the offsets of bb-probe.exe resolve
// to functions with llvm-symbolizer on the unstripped build (image base 0x140000000).
// BB_SAMPLE_THREADS=<name>,<name>: thread name prefixes instead of the GPU threads.

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "common/thread.h"

namespace {

struct Sampled {
    std::string name;
    HANDLE handle = nullptr;
    DWORD id = 0;
    u64 samples = 0;
    std::unordered_map<u64, u32> hits;
};

std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; text && *text; ++text) {
        out.push_back(*text < 128 ? char(*text) : '?');
    }
    return out;
}

void Discover(std::vector<Sampled>& threads, const std::vector<std::string>& prefixes) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid ||
            std::ranges::any_of(threads, [&](const Sampled& s) { return s.id == entry.th32ThreadID; })) {
            continue;
        }
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (!handle) {
            continue;
        }
        PWSTR description = nullptr;
        std::string name;
        if (SUCCEEDED(GetThreadDescription(handle, &description))) {
            name = Narrow(description);
            LocalFree(description);
        }
        const bool wanted = !name.empty() && std::ranges::any_of(prefixes, [&](const std::string& p) {
            return name.rfind(p, 0) == 0;
        });
        if (!wanted) {
            CloseHandle(handle);
            continue;
        }
        threads.push_back({name, handle, entry.th32ThreadID});
        std::printf("Sampler: sampling thread '%s'\n", name.c_str());
    }
    CloseHandle(snapshot);
}

std::string Describe(u64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        char text[32];
        std::snprintf(text, sizeof(text), "guest/anon %#llx", static_cast<unsigned long long>(address));
        return text;
    }
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    std::string name = Narrow(length ? path : L"?");
    if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    char text[320];
    std::snprintf(text, sizeof(text), "%s+%#llx", name.c_str(),
                  static_cast<unsigned long long>(address - reinterpret_cast<u64>(module)));
    return text;
}

void Report(std::vector<Sampled>& threads, double seconds) {
    for (auto& t : threads) {
        if (!t.samples) {
            continue;
        }
        std::vector<std::pair<u64, u32>> top(t.hits.begin(), t.hits.end());
        std::ranges::sort(top, std::greater{}, &std::pair<u64, u32>::second);
        // Per module too: driver, kernel, the port itself, the game's code.
        std::unordered_map<std::string, u64> modules;
        for (const auto& [address, count] : t.hits) {
            std::string module = Describe(address);
            module = module.substr(0, module.find_first_of("+ "));
            modules[module] += count;
        }
        std::printf("Sampler: '%s' %llu samples in %.0f s; by module:", t.name.c_str(),
                    static_cast<unsigned long long>(t.samples), seconds);
        std::vector<std::pair<std::string, u64>> by_module(modules.begin(), modules.end());
        std::ranges::sort(by_module, std::greater{}, &std::pair<std::string, u64>::second);
        for (size_t i = 0; i < by_module.size() && i < 6; ++i) {
            std::printf(" %s %.1f%%", by_module[i].first.c_str(),
                        100.0 * by_module[i].second / t.samples);
        }
        std::printf("\n");
        for (size_t i = 0; i < top.size() && i < 30; ++i) {
            std::printf("Sampler:   %5.2f%% %s\n", 100.0 * top[i].second / t.samples,
                        Describe(top[i].first).c_str());
        }
        t.samples = 0;
        t.hits.clear();
    }
}

} // namespace

void BbSamplerStart() {
    const char* env = std::getenv("BB_SAMPLE_THREADS");
    if (!env || !env[0] || env[0] == '0') {
        return;
    }
    std::vector<std::string> prefixes;
    if (env[0] == '1' && !env[1]) {
        prefixes = {"shadPS4:GpuCommandProcessor", "bb:DrawRec"};
    } else {
        for (std::string list = env; !list.empty();) {
            const auto comma = list.find(',');
            prefixes.push_back(list.substr(0, comma));
            list = comma == std::string::npos ? "" : list.substr(comma + 1);
        }
    }
    std::thread([prefixes] {
        Common::SetCurrentThreadName("bb:Sampler");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::vector<Sampled> threads;
        auto window = std::chrono::steady_clock::now();
        auto last_scan = window - std::chrono::seconds(10);
        std::vector<u64> rips;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_scan >= std::chrono::seconds(2) && threads.size() < prefixes.size()) {
                Discover(threads, prefixes);
                last_scan = now;
            }
            rips.assign(threads.size(), 0);
            for (size_t i = 0; i < threads.size(); ++i) {
                if (SuspendThread(threads[i].handle) == DWORD(-1)) {
                    continue;
                }
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(threads[i].handle, &context)) {
                    rips[i] = context.Rip;
                }
                ResumeThread(threads[i].handle);
            }
            for (size_t i = 0; i < threads.size(); ++i) {
                if (rips[i]) {
                    ++threads[i].hits[rips[i]];
                    ++threads[i].samples;
                }
            }
            if (now - window >= std::chrono::seconds(10)) {
                Report(threads, std::chrono::duration<double>(now - window).count());
                window = now;
            }
            LARGE_INTEGER due;
            due.QuadPart = -10000; // 1 ms
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
        }
    }).detach();
}

#else

void BbSamplerStart() {}

#endif
