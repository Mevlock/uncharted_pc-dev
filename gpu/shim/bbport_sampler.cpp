// SPDX-License-Identifier: GPL-2.0-or-later
// bbport diagnosis: BB_SAMPLE_THREADS=1 (Windows). A thread samples where the GPU threads run,
// about once per millisecond: it suspends each one, reads its instruction pointer and resumes it
// (nothing is allocated while a thread is suspended: it may hold the heap lock). Every 10 s the
// log gets each thread's busiest addresses as module+offset; the offsets of bb-probe.exe resolve
// to functions with llvm-symbolizer on the unstripped build (image base 0x140000000).
// BB_SAMPLE_THREADS=<name>,<name>: thread name prefixes instead of the GPU threads.
// Each sample also notes its function and the three bb-probe.exe functions above it (unwind
// tables), so the log shows time per function and the call paths into it, not only addresses.
// BB_HITCH_MS=<ms>: the hitch catcher. The game's main thread, its submission and job threads and
// the port's GPU threads are sampled every 2 ms into a ring; each frame longer than <ms> (guest
// flip to flip, BbHitchFlip) prints, per thread, where it spent that frame: what stalled it.

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "common/thread.h"

extern "C" void bb_describe_address(char* out, size_t size, uintptr_t address); // src/probe.c

namespace {

struct Sampled {
    std::string name;
    HANDLE handle = nullptr;
    DWORD id = 0;
    u64 samples = 0;
    std::unordered_map<u64, u32> hits;
    /// Samples outside bb-probe.exe (waits, system calls, the driver).
    u64 outside = 0;
    /// Samples by function (start address) and by call path: the sampled function, then the
    /// first three bb-probe.exe return addresses above it.
    std::unordered_map<u64, u32> functions;
    std::map<std::array<u64, 4>, u32> paths;
};

u64 image_begin = 0, image_end = 0;

// The hitch catcher (BB_HITCH_MS): the samples of the last seconds, and the frame to report.
struct HitchSample {
    u64 ns;
    u32 thread;
    std::array<u64, 4> path;
};
constexpr size_t HitchRing = size_t{1} << 17;
std::vector<HitchSample> hitch_ring;
size_t hitch_next = 0;
double hitch_ms = 0.0;
std::atomic<u64> hitch_begin_ns{0}, hitch_end_ns{0}; ///< a frame to report (end 0: none)

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

bool InImage(u64 address) {
    return address >= image_begin && address < image_end;
}

bool ReadWord(u64 address, u64& value) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &value,
                             sizeof(value), &done) &&
           done == sizeof(value);
}

/// The current stack (fibers switch it; the thread block follows) of a suspended thread.
bool StackBounds(HANDLE thread, u64& low, u64& high) {
    using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryThread>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    struct BasicInformation {
        LONG exit_status;
        PVOID teb;
        PVOID client_id[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } info{};
    if (!query || query(thread, 0, &info, sizeof(info), nullptr) != 0 || !info.teb) {
        return false;
    }
    const u64 teb = reinterpret_cast<u64>(info.teb);
    return ReadWord(teb + 0x08, high) && ReadWord(teb + 0x10, low) && low < high;
}

/// The start of the function holding an address (its unwind entry), else the address.
u64 FunctionOf(u64 address) {
    DWORD64 base = 0;
    if (const auto* entry = RtlLookupFunctionEntry(address, &base, nullptr)) {
        return base + entry->BeginAddress;
    }
    return address;
}

/// The sampled function, then the first three bb-probe.exe return addresses above a suspended
/// thread, by the unwind tables (through the system DLLs and the driver; stops at the game's
/// code, which has none).
std::array<u64, 4> Callers(HANDLE thread, CONTEXT context) {
    std::array<u64, 4> found{FunctionOf(context.Rip)};
    u32 count = 1;
    u64 low = 0, high = 0;
    if (!StackBounds(thread, low, high)) {
        return found;
    }
    for (int depth = 0; depth < 48 && count < found.size(); ++depth) {
        if (context.Rsp < low || context.Rsp + 64 > high) {
            break;
        }
        DWORD64 base = 0;
        if (auto* entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr)) {
            PVOID handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context,
                             &handler_data, &frame, nullptr);
        } else {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(context.Rip), &module)) {
                break; // the game's code: no unwind information
            }
            u64 ret = 0; // a leaf function: the return address is at rsp
            if (!ReadWord(context.Rsp, ret)) {
                break;
            }
            context.Rip = ret;
            context.Rsp += 8;
        }
        if (!context.Rip) {
            break;
        }
        if (InImage(context.Rip)) {
            found[count++] = context.Rip;
        }
    }
    return found;
}

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
        // BB_SAMPLE_TOP=<n>: more addresses per thread (default 30), for summing by function.
        static const size_t listed = [] {
            const char* env = std::getenv("BB_SAMPLE_TOP");
            return env ? static_cast<size_t>(std::strtoul(env, nullptr, 10)) : size_t{30};
        }();
        for (size_t i = 0; i < top.size() && i < listed; ++i) {
            std::printf("Sampler:   %5.2f%% %s\n", 100.0 * top[i].second / t.samples,
                        Describe(top[i].first).c_str());
        }
        std::vector<std::pair<u64, u32>> by_function(t.functions.begin(), t.functions.end());
        std::ranges::sort(by_function, std::greater{}, &std::pair<u64, u32>::second);
        std::printf("Sampler: '%s' by function (outside the port: waits, system calls, driver "
                    "%.1f%%):\n", t.name.c_str(), 100.0 * t.outside / t.samples);
        for (size_t i = 0; i < by_function.size() && i < 25; ++i) {
            std::printf("Sampler:   %5.2f%% %s\n", 100.0 * by_function[i].second / t.samples,
                        Describe(by_function[i].first).c_str());
        }
        std::vector<std::pair<std::array<u64, 4>, u32>> by_path(t.paths.begin(), t.paths.end());
        std::ranges::sort(by_path, std::greater{}, &std::pair<std::array<u64, 4>, u32>::second);
        std::printf("Sampler: '%s' by call path (function <- bb-probe.exe return addresses):\n",
                    t.name.c_str());
        for (size_t i = 0; i < by_path.size() && i < 25; ++i) {
            std::printf("Sampler:   %5.2f%% %s", 100.0 * by_path[i].second / t.samples,
                        Describe(by_path[i].first[0]).c_str());
            for (size_t f = 1; f < 4 && by_path[i].first[f]; ++f) {
                std::printf(" <- %#llx",
                            static_cast<unsigned long long>(by_path[i].first[f] - image_begin));
            }
            std::printf("\n");
        }
        t.samples = 0;
        t.hits.clear();
        t.outside = 0;
        t.functions.clear();
        t.paths.clear();
    }
}

std::string HitchDescribe(u64 address) {
    char text[160];
    bb_describe_address(text, sizeof(text), static_cast<uintptr_t>(address));
    return text;
}

/// The samples of one long frame, per thread: where each spent it, by call path (the guest's
/// code has no unwind tables: its addresses are grouped by 256 bytes).
void HitchReport(const std::vector<Sampled>& threads, u64 begin_ns, u64 end_ns) {
    static u32 reports;
    if (++reports > 200) {
        return;
    }
    std::vector<std::map<std::array<u64, 4>, u32>> per_thread(threads.size());
    std::vector<u32> totals(threads.size());
    for (size_t k = 0; k < HitchRing; ++k) {
        const HitchSample& s = hitch_ring[k];
        if (s.ns < begin_ns || s.ns > end_ns || s.thread >= threads.size()) {
            continue;
        }
        auto path = s.path;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(path[0]), &module)) {
            path[0] &= ~u64{0xff};
        }
        ++per_thread[s.thread][path];
        ++totals[s.thread];
    }
    std::printf("Hitch: %.1f ms frame; where each thread spent it (2 ms samples):\n",
                (end_ns - begin_ns) / 1e6);
    for (size_t t = 0; t < threads.size(); ++t) {
        if (!totals[t]) {
            continue;
        }
        std::vector<std::pair<std::array<u64, 4>, u32>> top(per_thread[t].begin(),
                                                            per_thread[t].end());
        std::ranges::sort(top, std::greater{}, &std::pair<std::array<u64, 4>, u32>::second);
        std::printf("Hitch:   '%s' (%u):", threads[t].name.c_str(), totals[t]);
        for (size_t i = 0; i < top.size() && i < 3; ++i) {
            std::printf(" %s%.0f%% %s", i ? "| " : "", 100.0 * top[i].second / totals[t],
                        HitchDescribe(top[i].first[0]).c_str());
            for (size_t f = 1; f < 4 && top[i].first[f]; ++f) {
                std::printf(" <- %#llx",
                            static_cast<unsigned long long>(top[i].first[f] - image_begin));
            }
        }
        std::printf("\n");
    }
}

} // namespace

/// Every guest flip (videoout driver): a frame longer than BB_HITCH_MS asks the sampler for a report.
void BbHitchFlip() {
    if (hitch_ms <= 0.0) {
        return;
    }
    static u64 last;
    const u64 now = NowNs();
    if (last && now - last > static_cast<u64>(hitch_ms * 1e6) && !hitch_end_ns.load()) {
        hitch_begin_ns.store(last);
        hitch_end_ns.store(now);
    }
    last = now;
}

void BbSamplerStart() {
    const char* env = std::getenv("BB_SAMPLE_THREADS");
    const bool sampling = env && env[0] && env[0] != '0';
    if (const char* h = std::getenv("BB_HITCH_MS")) {
        hitch_ms = std::strtod(h, nullptr);
    }
    if (!sampling && hitch_ms <= 0.0) {
        return;
    }
    std::vector<std::string> prefixes;
    if (!sampling) {   // the hitch catcher alone: the threads a frame waits on
        prefixes = {"bb:GuestMain", "Frame Submis", "NdJobWorker", "shadPS4:GpuCommandProcessor",
                    "bb:DrawRec", "bb:VkRecorder", "bb:Present", "shadPS4:PresentThread"};
        std::printf("Hitch catcher: frames over %.0f ms are reported (BB_HITCH_MS)\n", hitch_ms);
    } else if (env[0] == '1' && !env[1]) {
        prefixes = {"shadPS4:GpuCommandProcessor", "bb:DrawRec", "bb:VkRecorder"};
    } else {
        for (std::string list = env; !list.empty();) {
            const auto comma = list.find(',');
            prefixes.push_back(list.substr(0, comma));
            list = comma == std::string::npos ? "" : list.substr(comma + 1);
        }
    }
    {
        const auto base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        image_begin = base;
        image_end = base + nt->OptionalHeader.SizeOfImage;
    }
    if (hitch_ms > 0.0) {
        hitch_ring.assign(HitchRing, HitchSample{});
    }
    std::thread([prefixes, sampling] {
        Common::SetCurrentThreadName("bb:Sampler");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::vector<Sampled> threads;
        auto window = std::chrono::steady_clock::now();
        auto last_scan = window - std::chrono::seconds(10);
        const auto started = window;
        std::vector<u64> rips;
        std::vector<std::array<u64, 4>> callers;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            // A prefix can match many threads (BB_SAMPLE_THREADS=g: for the game's threads): scan
            // for 90 s, not only until each prefix found one, at most 64 threads.
            if (now - last_scan >= std::chrono::seconds(2) && threads.size() < 64 &&
                (threads.size() < prefixes.size() || now - started < std::chrono::seconds(90))) {
                Discover(threads, prefixes);
                last_scan = now;
            }
            rips.assign(threads.size(), 0);
            callers.assign(threads.size(), {});
            for (size_t i = 0; i < threads.size(); ++i) {
                if (SuspendThread(threads[i].handle) == DWORD(-1)) {
                    continue;
                }
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(threads[i].handle, &context)) {
                    rips[i] = context.Rip;
                    callers[i] = Callers(threads[i].handle, context);
                }
                ResumeThread(threads[i].handle);
            }
            if (hitch_ms > 0.0) {
                const u64 ns = NowNs();
                for (size_t i = 0; i < threads.size(); ++i) {
                    if (rips[i]) {
                        hitch_ring[hitch_next] = {ns, static_cast<u32>(i), callers[i]};
                        hitch_next = (hitch_next + 1) % HitchRing;
                    }
                }
                if (const u64 end = hitch_end_ns.load(); end && NowNs() > end + 5000000) {
                    HitchReport(threads, hitch_begin_ns.load(), end);
                    hitch_end_ns.store(0);
                }
            }
            for (size_t i = 0; i < threads.size() && sampling; ++i) {
                if (rips[i]) {
                    ++threads[i].hits[rips[i]];
                    ++threads[i].samples;
                    ++threads[i].functions[callers[i][0]];
                    ++threads[i].paths[callers[i]];
                    if (!InImage(rips[i])) {
                        ++threads[i].outside;
                    }
                }
            }
            if (sampling && now - window >= std::chrono::seconds(10)) {
                Report(threads, std::chrono::duration<double>(now - window).count());
                window = now;
            }
            LARGE_INTEGER due;
            due.QuadPart = sampling ? -10000 : -20000; // 1 ms (2 ms for the hitch catcher alone)
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
void BbHitchFlip() {}

#endif
