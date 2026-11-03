// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#include <x86intrin.h>

#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

/// bbport (on; BB_RING_COLLECTOR=0: off): the constant ring's work for a packet - clamping each candidate
/// to mapped memory, the GPU-modified check, the ring allocation and the copy - runs on its own
/// thread in packet order instead of on stage A (the command processor, U3's critical stage).
/// Stage A keeps what needs its own state: reading the V#s, noting written buffers, the check
/// against writes still queued for stage B. Stage B waits for a packet's results (Wait) and
/// reads its bindings and ring end (Result).
class RingCollector {
public:
    /// The layout of Rasterizer::RingBinding (stage B reads them as those).
    struct Binding {
        u32 index; ///< buffer resource of the stage
        u32 size;
        u64 offset; ///< in the ring
        VAddr address;
    };
    static constexpr u32 MaxStages = 8;
    struct Result {
        u64 first;    ///< in the bindings ring; a packet's bindings never wrap
        u64 ring_end; ///< ring position after the packet's allocations
        std::array<u16, MaxStages> counts; ///< bindings per stage of the packet, in order
    };
    /// What the thread does per candidate; returns false when it is not bound from the ring.
    /// `source` is set for bytes queued with the item (flat user data), else guest memory.
    /// `size` may be clamped (to mapped memory).
    using CollectFn = bool (*)(void* context, VAddr address, u64& size, const u8* source,
                               u64& ring_offset, u64& ring_end);

    static bool Enabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("BB_RING_COLLECTOR");
            return !(env && env[0] == '0');
        }();
        return enabled;
    }

    void Start(CollectFn fn, void* context) {
        collect_fn = fn;
        collect_context = context;
        worker = std::jthread([this](std::stop_token stop) { Loop(stop); });
    }

    ~RingCollector() {
        if (worker.joinable()) {
            worker.request_stop();
            wake.store(1, std::memory_order_seq_cst);
            wake.notify_one();
            worker.join();
        }
    }

    /// Stage A: a candidate of the packet being built.
    void Add(u32 stage, u32 index, VAddr address, u64 size, u64 pipe_position = 0) {
        Push({address, size, stage, index, ItemKind::Candidate, pipe_position});
    }

    /// Diagnosis (U2 train flicker, 2026-11-02): BB_COLLECT_AFTER_B=1: a candidate is copied only
    /// once stage B has run every packet before its own (`reached`, with the position Add got).
    using ReachedFn = bool (*)(void* context, u64 position);
    using PeekFn = void (*)(void* context, VAddr address, u8* out, u64 size);
    using GpuModFn = int (*)(void* context, VAddr address, u64 size);
    void SetReached(ReachedFn fn, PeekFn peek = nullptr, GpuModFn gpu_mod = nullptr) {
        reached_fn = fn;
        peek_fn = peek;
        gpu_mod_fn = gpu_mod;
    }

    /// Stage A: bytes (flat user data) to copy into the ring as the binding `index`.
    void AddBytes(u32 stage, u32 index, const void* data, u64 size) {
        u64 at = bytes_head;
        if (at % Bytes + size > Bytes) {
            at += Bytes - at % Bytes; // keep it contiguous
        }
        if (at + size - bytes_done.load(std::memory_order_acquire) > Bytes) {
            Kick(); // items published lazily may not have woken the thread
            while (at + size - bytes_done.load(std::memory_order_acquire) > Bytes) {
                _mm_pause();
            }
        }
        std::memcpy(bytes.data() + at % Bytes, data, size);
        bytes_head = at + size;
        Push({at % Bytes, size, stage, index, ItemKind::Bytes, at + size});
    }

    /// Stage A: ends the packet; returns its sequence number (0: nothing to wait for).
    u64 EndPacket() {
        if (!items_in_packet) {
            return 0;
        }
        const u64 seq = ++packets_queued;
        Push({0, 0, 0, 0, ItemKind::End, seq});
        items_in_packet = 0;
        return seq;
    }

    /// Stage B: waits for the packet `seq`; its result stays valid until Release(seq).
    const Result& Wait(u64 seq) {
        if (packets_done.load(std::memory_order_acquire) < seq) {
            ++waits;
            while (packets_done.load(std::memory_order_acquire) < seq) {
                _mm_pause();
            }
        }
        return results[seq % results.size()];
    }
    [[nodiscard]] const Binding* Bindings(const Result& result) const {
        return &bindings[result.first % bindings.size()];
    }
    /// Stage B: done with the packet `seq` (its results and bindings may be reused).
    void Release(u64 seq) {
        released.store(seq, std::memory_order_release);
    }

    /// Stage A: waits until the thread has handled every packet queued so far.
    void Drain() {
        const u64 seq = packets_queued;
        while (packets_done.load(std::memory_order_acquire) < seq) {
            _mm_pause();
        }
    }

    u64 waits = 0; ///< stage B: packets it had to wait for

private:
    enum class ItemKind : u32 { Candidate, Bytes, End };
    struct Item {
        u64 address; ///< guest address, or offset in `bytes`
        u64 size;
        u32 stage;
        u32 index;
        ItemKind kind;
        u64 extra; ///< End: the packet's sequence; Bytes: the byte ring position after it
    };
    static constexpr u64 Bytes = 4ull << 20;
    static constexpr u64 MaxPerPacket = 512; ///< more than MaxStages x Shader::NUM_BUFFERS

    /// BB_RING_LAZY=0: every item published with a full fence and a wake check (the old way).
    /// Default: candidates and bytes go out with a plain release store (no fence, no read of the
    /// thread's progress while there is room); the fence and wake check come with the packet's
    /// End and before any wait for room, so a sleeping thread is always woken before stage A or
    /// B can wait on it.
    static bool Lazy() {
        static const bool lazy = [] {
            const char* env = std::getenv("BB_RING_LAZY");
            return !(env && env[0] == '0');
        }();
        return lazy;
    }

    void Kick() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.store(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    void Push(const Item& item) {
        const u64 at = head.load(std::memory_order_relaxed);
        if (at - done_seen >= items.size()) {
            done_seen = done.load(std::memory_order_acquire);
            if (at - done_seen >= items.size()) {
                Kick();
                while (at - (done_seen = done.load(std::memory_order_acquire)) >= items.size()) {
                    _mm_pause();
                }
            }
        }
        items[at % items.size()] = item;
        ++items_in_packet;
        if (Lazy() && item.kind != ItemKind::End) {
            head.store(at + 1, std::memory_order_release);
            return;
        }
        head.store(at + 1, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.store(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    void Loop(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:RingCollect");
        u64 at = 0;
        u64 out = 0;          ///< bindings written
        u64 packet_first = 0; ///< first binding of the packet being collected
        u64 ring_end = 0;
        std::array<u16, MaxStages> counts{};
        while (!stop.stop_requested()) {
            const u64 h = head.load(std::memory_order_acquire);
            if (at == h) {
                bool more = false;
                for (u32 i = 0; i < 4096 && !more; ++i) {
                    _mm_pause();
                    more = head.load(std::memory_order_acquire) != h;
                }
                if (more) {
                    continue;
                }
                wake.store(0, std::memory_order_seq_cst);
                sleeping.store(true, std::memory_order_seq_cst);
                if (head.load(std::memory_order_seq_cst) == h && !stop.stop_requested()) {
                    wake.wait(0, std::memory_order_seq_cst);
                }
                sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
            const Item item = items[at % items.size()];
            if (item.kind == ItemKind::End) {
                // Results of packet seq go where packet seq - results.size() was: stage B must
                // have released that one.
                while (item.extra - released.load(std::memory_order_acquire) >= results.size()) {
                    _mm_pause();
                }
                results[item.extra % results.size()] = {packet_first, ring_end, counts};
                counts = {};
                // bbport (U2 chapter 1 train flash, 2026-11-02): the ring is write-combined
                // memory (video memory through the BAR). Its stores wait in the core's
                // write-combining buffers until something drains them, and the release store
                // below does not: stage B recorded, and the GPU read, constants whose first
                // bytes still held the ring's previous contents. The fence drains them before
                // the packet is published. BB_RING_SFENCE=0: as before.
                static const bool sfence = [] {
                    const char* env = std::getenv("BB_RING_SFENCE");
                    return !(env && env[0] == '0');
                }();
                if (sfence) {
                    _mm_sfence();
                }
                // The next packet's bindings start where they cannot wrap.
                if (out % bindings.size() + MaxPerPacket > bindings.size()) {
                    out += bindings.size() - out % bindings.size();
                }
                packet_first = out;
                ++at;
                done.store(at, std::memory_order_release);
                packets_done.store(item.extra, std::memory_order_release);
                continue;
            }
            const u8* source = item.kind == ItemKind::Bytes ? bytes.data() + item.address
                                                             : nullptr;
            const VAddr address = item.kind == ItemKind::Bytes ? 0 : item.address;
            if (reached_fn && item.kind == ItemKind::Candidate && item.extra) {
                // BB_COLLECT_DIFF=1: the candidate's bytes before and after the wait; a
                // difference is a write stage B made while running an earlier packet.
                static const bool diff = [] {
                    const char* env = std::getenv("BB_COLLECT_DIFF");
                    return env && env[0] == '1';
                }();
                if (diff && peek_fn && item.size <= 65536) {
                    peek_before.resize(item.size);
                    peek_fn(collect_context, item.address, peek_before.data(), item.size);
                }
                const int gpu_before =
                    diff && gpu_mod_fn ? gpu_mod_fn(collect_context, item.address, item.size) : 0;
                while (!reached_fn(collect_context, item.extra)) {
                    _mm_pause();
                }
                if (diff && gpu_mod_fn) {
                    const int gpu_after = gpu_mod_fn(collect_context, item.address, item.size);
                    static u32 flips = 0;
                    if (gpu_after != gpu_before && ++flips <= 4000) {
                        std::printf("Collect diff: %#llx size %llu: GPU-written %d -> %d while "
                                    "stage B ran the packets before the draw\n",
                                    static_cast<unsigned long long>(item.address),
                                    static_cast<unsigned long long>(item.size), gpu_before,
                                    gpu_after);
                    }
                }
                if (diff && peek_fn && item.size <= 65536) {
                    peek_after.resize(item.size);
                    peek_fn(collect_context, item.address, peek_after.data(), item.size);
                    if (std::memcmp(peek_before.data(), peek_after.data(), item.size) != 0) {
                        u32 first = 0, count = 0;
                        for (u32 b = 0; b + 4 <= item.size; b += 4) {
                            if (std::memcmp(peek_before.data() + b, peek_after.data() + b, 4)) {
                                first = count++ ? first : b;
                            }
                        }
                        u32 was = 0, is = 0;
                        std::memcpy(&was, peek_before.data() + first, 4);
                        std::memcpy(&is, peek_after.data() + first, 4);
                        static u32 reports = 0;
                        if (++reports <= 4000) {
                            std::printf("Collect diff: %#llx size %llu: %u words written by stage B "
                                        "before the draw (first at +%u: %08x -> %08x)\n",
                                        static_cast<unsigned long long>(item.address),
                                        static_cast<unsigned long long>(item.size), count, first,
                                        was, is);
                        }
                    }
                }
            }
            u64 offset = 0;
            u64 size = item.size;
            if (item.stage < MaxStages &&
                collect_fn(collect_context, address, size, source, offset, ring_end)) {
                // The bindings ring holds those of unreleased packets: wait for room.
                while (out - FreeBefore(packet_first) >= bindings.size()) {
                    _mm_pause();
                }
                bindings[out % bindings.size()] = {item.index, u32(size), offset, address};
                ++out;
                ++counts[item.stage];
            }
            if (item.kind == ItemKind::Bytes) {
                bytes_done.store(item.extra, std::memory_order_release);
            }
            ++at;
            done.store(at, std::memory_order_release);
        }
    }

    /// Bindings before the first one of the oldest packet stage B has not released are free
    /// (with every finished packet released: those before the packet being collected).
    u64 FreeBefore(u64 packet_first) const {
        const u64 r = released.load(std::memory_order_acquire);
        const u64 d = packets_done.load(std::memory_order_relaxed);
        return r < d ? results[(r + 1) % results.size()].first : packet_first;
    }

    CollectFn collect_fn = nullptr;
    ReachedFn reached_fn = nullptr;
    PeekFn peek_fn = nullptr;
    GpuModFn gpu_mod_fn = nullptr;
    std::vector<u8> peek_before, peek_after;
    void* collect_context = nullptr;
    std::array<Item, 65536> items{};
    std::array<u8, Bytes> bytes{};
    std::array<Binding, 262144> bindings{};
    std::array<Result, 16384> results{};
    u64 items_in_packet = 0; ///< stage A
    u64 packets_queued = 0;  ///< stage A
    u64 bytes_head = 0;      ///< stage A
    u64 done_seen = 0;       ///< stage A: `done` as last read (only rises)
    alignas(64) std::atomic<u64> head{0};
    alignas(64) std::atomic<u64> done{0};
    alignas(64) std::atomic<u64> packets_done{0};
    alignas(64) std::atomic<u64> released{0};
    alignas(64) std::atomic<u64> bytes_done{0};
    alignas(64) std::atomic<bool> sleeping{false};
    std::atomic<u32> wake{0};
    std::jthread worker;
};

} // namespace Vulkan
