// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
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
    void Add(u32 stage, u32 index, VAddr address, u64 size) {
        Push({address, size, stage, index, ItemKind::Candidate, 0});
    }

    /// Stage A: bytes (flat user data) to copy into the ring as the binding `index`.
    void AddBytes(u32 stage, u32 index, const void* data, u64 size) {
        u64 at = bytes_head;
        if (at % Bytes + size > Bytes) {
            at += Bytes - at % Bytes; // keep it contiguous
        }
        while (at + size - bytes_done.load(std::memory_order_acquire) > Bytes) {
            _mm_pause();
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

    void Push(const Item& item) {
        const u64 at = head.load(std::memory_order_relaxed);
        while (at - done.load(std::memory_order_acquire) >= items.size()) {
            _mm_pause();
        }
        items[at % items.size()] = item;
        head.store(at + 1, std::memory_order_seq_cst);
        ++items_in_packet;
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
    void* collect_context = nullptr;
    std::array<Item, 65536> items{};
    std::array<u8, Bytes> bytes{};
    std::array<Binding, 262144> bindings{};
    std::array<Result, 16384> results{};
    u64 items_in_packet = 0; ///< stage A
    u64 packets_queued = 0;  ///< stage A
    u64 bytes_head = 0;      ///< stage A
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
