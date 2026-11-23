// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vulkan::Motion {

struct VertexRange {
    uint32_t first{}, count{};
    bool operator==(const VertexRange&) const = default;
};

// Reserve only the vertices referenced by this draw, not the whole shared vertex buffer.
// Primitive restart markers are not vertex shader invocations. Reject signed overflow.
template <class Index, size_t Extent>
VertexRange IndexedRange(std::span<const Index, Extent> indices, int32_t base_vertex, bool restart) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto index : indices) {
        if (restart && index == std::numeric_limits<Index>::max()) {
            continue;
        }
        lo = std::min(lo, uint32_t(index));
        hi = std::max(hi, uint32_t(index));
    }
    if (lo > hi || int64_t(lo) + base_vertex < 0 ||
        int64_t(hi) + base_vertex > INT32_MAX) {
        return {};
    }
    return {uint32_t(int64_t(lo) + base_vertex), hi - lo + 1};
}

// Vertex shader constant buffers (stride 16) of Bloodborne's G-buffer shaders: the 864-byte
// scene constants (camera), 416-byte model constants and palettes of 3x4 bone matrices
// (48 bytes per bone). Characters have 656+ bytes; weapons and props 2-8 bones (96-384).
enum class BufferRole { Other, Skeleton, SmallSkeleton };
inline BufferRole ClassifyBuffer(uint32_t size) {
    if (size == 864 || size > 16384) {
        return BufferRole::Other;
    }
    if (size >= 640) {
        return BufferRole::Skeleton;
    }
    return size >= 96 && size % 48 == 0 ? BufferRole::SmallSkeleton : BufferRole::Other;
}

// Scanning and hashing the index list of every stored draw each frame took ~10% of the GPU
// thread. Static meshes keep their index buffers, so a result is reused and fully verified
// again every `Revalidate` frames. A stale result only costs camera-quality vectors: the
// shader bounds every history access by the stored range.
class IndexRangeCache {
public:
    static constexpr uint64_t Revalidate = 32, Unused = 600;
    struct Key {
        uint64_t address{};
        uint32_t count{}, index_size{};
        int32_t base_vertex{};
        bool restart{};
        bool operator==(const Key&) const = default;
    };
    struct Result {
        VertexRange range;
        uint64_t topology{};
    };
    struct Stats {
        uint64_t hits{}, scans{}, stale{};
    } stats;

    template <class Scan>
    Result Get(const Key& key, uint64_t frame, Scan&& scan) {
        auto& entry = Find(key);
        entry.last_use = frame;
        if (entry.valid && frame - entry.verified < Revalidate) {
            ++stats.hits;
            return entry.result;
        }
        ++stats.scans;
        const Result fresh = scan();
        if (entry.valid && (fresh.topology != entry.result.topology ||
                            !(fresh.range == entry.result.range))) {
            ++stats.stale; // the cached result was wrong for up to Revalidate frames
        }
        entry.result = fresh;
        entry.verified = frame;
        entry.valid = true;
        return entry.result;
    }
    void Trim(uint64_t frame) {
        Rebuild(slots.size(), [frame](const Entry& e) { return frame - e.last_use <= Unused; });
    }
    size_t Size() const { return used; }

private:
    // Open addressing (linear probing) in one array: the per-draw lookup of a node-based map
    // missed the cache (~90 ns a draw on the GPU thread). Entries leave only by Trim/Grow, which
    // rebuild the table.
    struct Entry {
        Key key;
        Result result;
        uint64_t verified{}, last_use{};
        bool valid{};
        bool occupied{};
    };
    Entry& Find(const Key& key) {
        if ((used + 1) * 2 > slots.size()) {
            Rebuild(slots.size() * 2, [](const Entry&) { return true; });
        }
        const size_t mask = slots.size() - 1;
        for (size_t i = KeyHash{}(key) & mask;; i = (i + 1) & mask) {
            Entry& entry = slots[i];
            if (!entry.occupied) {
                entry = {};
                entry.key = key;
                entry.occupied = true;
                ++used;
                return entry;
            }
            if (entry.key == key) {
                return entry;
            }
        }
    }
    template <class Keep>
    void Rebuild(size_t size, Keep&& keep) {
        std::vector<Entry> old(size);
        old.swap(slots);
        used = 0;
        const size_t mask = slots.size() - 1;
        for (const auto& entry : old) {
            if (!entry.occupied || !keep(entry)) {
                continue;
            }
            size_t i = KeyHash{}(entry.key) & mask;
            while (slots[i].occupied) {
                i = (i + 1) & mask;
            }
            slots[i] = entry;
            ++used;
        }
    }
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = k.address * 0x9e3779b97f4a7c15ULL;
            h ^= (uint64_t(k.count) << 32 | uint32_t(k.base_vertex)) + (h << 6) + (h >> 2);
            h ^= uint64_t(k.index_size << 1 | uint32_t(k.restart)) + (h << 6) + (h >> 2);
            return size_t(h ^ (h >> 29));
        }
    };
    std::vector<Entry> slots = std::vector<Entry>(64);
    size_t used = 0;
};

// Open-addressing map for the per-frame tables of History: entries are keyed per draw and
// the tables are cleared every frame. std::unordered_map allocated a node per insert (four
// per draw) and freed them all at the frame end; here clear() only starts a new generation.
template <class Key, class Value, class Hash>
class FlatMap {
public:
    FlatMap() : slots(64) {}

    Value& operator[](const Key& key) {
        return Insert(key, Hash{}(key)).first->value;
    }
    /// The same with the key's hash (Hash{}(key)) already known.
    Value& At(const Key& key, size_t hash) {
        return Insert(key, hash).first->value;
    }
    /// Inserts `value` when `key` is absent (like std::unordered_map::emplace).
    void emplace(const Key& key, const Value& value) {
        emplace(key, Hash{}(key), value);
    }
    void emplace(const Key& key, size_t hash, const Value& value) {
        const auto [slot, inserted] = Insert(key, hash);
        if (inserted) {
            slot->value = value;
        }
    }
    const Value* find(const Key& key) const {
        return find(key, Hash{}(key));
    }
    const Value* find(const Key& key, size_t hash) const {
        const size_t mask = slots.size() - 1;
        for (size_t i = Start(hash) & mask;; i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.generation != generation) {
                return nullptr;
            }
            if (slot.hash == hash && slot.key == key) {
                return &slot.value;
            }
        }
    }
    void clear() {
        size = 0;
        if (++generation == 0) { // wrapped: stale slots could look current
            for (auto& slot : slots) {
                slot.generation = 0;
            }
            generation = 1;
        }
    }
    void swap(FlatMap& other) noexcept {
        slots.swap(other.slots);
        std::swap(size, other.size);
        std::swap(generation, other.generation);
    }

private:
    struct Slot {
        Key key{};
        Value value{};
        uint32_t generation = 0;
        size_t hash = 0; ///< Hash{}(key): compared before the key
    };
    static size_t Start(size_t hash) {
        const uint64_t h = uint64_t(hash) * 0x9e3779b97f4a7c15ULL;
        return size_t(h ^ (h >> 29));
    }
    std::pair<Slot*, bool> Insert(const Key& key, size_t hash) {
        if ((size + 1) * 2 > slots.size()) {
            Grow();
        }
        const size_t mask = slots.size() - 1;
        for (size_t i = Start(hash) & mask;; i = (i + 1) & mask) {
            Slot& slot = slots[i];
            if (slot.generation != generation) {
                slot = {key, Value{}, generation, hash};
                ++size;
                return {&slot, true};
            }
            if (slot.hash == hash && slot.key == key) {
                return {&slot, false};
            }
        }
    }
    void Grow() {
        std::vector<Slot> old(slots.size() * 2);
        old.swap(slots);
        const uint32_t live = generation;
        generation = 1;
        size = 0;
        for (const auto& slot : old) {
            if (slot.generation == live) {
                *Insert(slot.key, slot.hash).first = {slot.key, slot.value, generation, slot.hash};
            }
        }
    }

    std::vector<Slot> slots;
    size_t size = 0;
    uint32_t generation = 1;
};

inline uint32_t JitterPhases(uint32_t render_width, uint32_t output_width) {
    if (!render_width || !output_width) {
        return 8;
    }
    const double ratio = double(output_width) / render_width;
    return uint32_t(std::clamp(std::ceil(8.0 * ratio * ratio), 8.0, 256.0));
}

struct Draw {
    uint64_t shader{}, geometry{}, indices{}, topology{};
    uint32_t index_count{}, instances{}, first_instance{};
    VertexRange vertices;
    bool operator==(const Draw& rhs) const = default;
};

// CPU bookkeeping shared with the regression test. Draw identity includes topology and
// offsets: a reused index buffer or another submesh must never read uninitialised history.
class History {
public:
    explicit History(uint32_t capacity) : capacity{capacity} {}

    struct Allocation {
        uint32_t store{}, load{}, vertices{}, instances{}, first_vertex{}, first_instance{};
    };
    /// Allocate's result: the draw's store (in draw order) without its pairing; Pair adds the load.
    struct Stored {
        Allocation allocation;
        bool valid{}; ///< false: no vertices or out of range (nothing stored, nothing paired)
    };
    struct Stats {
        uint64_t draws{}, stored{}, loaded{}, unmatched{}, exhausted{}, invalid{}, still{};
        uint64_t in_order{}, searched{}, indexes{}; ///< paired at the walk's position, through the index; indexes built
    } stats;

    // A small skeleton (weapon, prop; most are static world pieces) only needs history
    // while its bone palette changes: with last frame's bones its motion is the camera's.
    // Checked before the index scan of Prepare, with a cheap identity. The first frame
    // of a change stores positions (nothing to load yet), the next one loads them.
    struct GateKey {
        uint64_t shader{}, geometry{}, indices{};
        uint32_t index_count{}, instances{}, first_instance{}, occurrence{};
        bool operator==(const GateKey&) const = default;
    };
    bool Moving(GateKey key, uint64_t palette) {
        key.occurrence = 0;
        key.occurrence = gate_occurrences[key]++;
        const auto* prev = gate_previous.find(key);
        const bool moving = prev && *prev != palette;
        gate_current.emplace(key, palette);
        stats.still += moving ? 0 : 1;
        return moving;
    }

    void NextFrame() {
        previous.swap(current);
        current.clear();
        cursor = 0;
        index_built = false;
        gate_previous.swap(gate_current);
        gate_current.clear();
        gate_occurrences.clear();
        ++frame;
        used = 0;
    }

    // A draw is paired with the first draw of the previous frame that is the same and not paired yet: the k-th of
    // identical draws with the k-th. Draws come in nearly the same order every frame, so the previous frame's draws
    // are walked in order (one compare a draw); a draw out of that order (the camera culled or uncovered some) is
    // found through an index of the previous frame, built at the first such draw of a frame, and the walk goes on
    // behind it. (Two hash-table lookups a draw were ~70 ns of the GPU thread's ~110 ns on a motion draw.)
    //
    // Prepare is Allocate then Pair. They can also run apart (ObjectMotion defers the pairing of a draw whose store
    // was allocated to a batch before the submission): Allocate in draw order, Pair later for the same draws in the
    // same order, with no NextFrame between them. The store does not depend on the pairing, so the allocations are
    // the same either way.
    Allocation Prepare(const Draw& draw) {
        Stored stored = Allocate(draw);
        if (stored.valid) {
            Pair(draw, stored.allocation);
        }
        return stored.allocation;
    }
    /// The draw's store (positions of this frame), in draw order.
    Stored Allocate(const Draw& draw) {
        ++stats.draws;
        const uint64_t count = uint64_t(draw.vertices.count) * draw.instances;
        if (!count || uint64_t(draw.vertices.first) + draw.vertices.count - 1 > INT32_MAX ||
            uint64_t(draw.first_instance) + draw.instances > UINT32_MAX) {
            ++stats.invalid;
            return {};
        }
        Stored result{{0, 0, draw.vertices.count, draw.instances, draw.vertices.first, draw.first_instance}, true};
        if (count <= capacity - used) {
            result.allocation.store = 1 + uint32_t(frame & 1) * capacity + used;
            used += uint32_t(count);
            ++stats.stored;
        } else {
            ++stats.exhausted;
        }
        return result;
    }
    /// The load of a valid draw `Allocate` gave `result` for: its partner of the previous frame.
    void Pair(const Draw& draw, Allocation& result) {
        const uint64_t hash = OfDraw(draw);
        Entry* prev = nullptr;
        if (cursor < previous.size() && previous[cursor].hash == hash && !previous[cursor].paired &&
            previous[cursor].draw == draw) {
            prev = &previous[cursor++];
            ++stats.in_order;
            // Behind a draw found out of order, an earlier one of identical draws may still be unpaired: the
            // earliest goes first.
            if (index_built) {
                for (uint32_t e = prev->prev_same; e != NoEntry; e = previous[e].prev_same) {
                    if (!previous[e].paired && previous[e].draw == draw) {
                        prev = &previous[e];
                    }
                }
            }
        } else {
            prev = Find(hash, draw);
            ++stats.searched;
        }
        if (prev) {
            prev->paired = true;
        }
        if (prev && prev->store) {
            result.load = prev->store;
            ++stats.loaded;
        } else {
            ++stats.unmatched;
        }
        current.push_back({draw, hash, result.store, NoEntry, NoEntry, false});
    }

    uint32_t Used() const { return used; }

private:
    static constexpr uint32_t NoEntry = UINT32_MAX;
    struct Entry {
        Draw draw;
        uint64_t hash;
        uint32_t store;     ///< first position of its store (0: none)
        uint32_t next_same; ///< the next entry with the same hash (the index)
        uint32_t prev_same; ///< the one before it
        bool paired;
    };
    static uint64_t OfDraw(const Draw& d) {
        uint64_t h = 0;
        const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix(d.shader); mix(d.geometry); mix(d.indices); mix(d.topology);
        mix(d.index_count); mix(d.instances); mix(d.first_instance);
        mix(d.vertices.first); mix(d.vertices.count);
        return h ^ (h >> 29);
    }
    struct IndexSlot {
        uint64_t hash;
        uint32_t head; ///< first entry with this hash not known to be paired
        uint32_t generation;
    };
    Entry* Find(uint64_t hash, const Draw& draw) {
        if (previous.empty()) {
            return nullptr;
        }
        if (!index_built) {
            BuildIndex();
        }
        const size_t mask = index.size() - 1;
        for (size_t i = (hash * 0x9e3779b97f4a7c15ULL >> 32) & mask;; i = (i + 1) & mask) {
            IndexSlot& slot = index[i];
            if (slot.generation != generation) {
                return nullptr;
            }
            if (slot.hash != hash) {
                continue;
            }
            // Paired entries at the front of the chain are skipped for good.
            while (slot.head != NoEntry && previous[slot.head].paired) {
                slot.head = previous[slot.head].next_same;
            }
            for (uint32_t e = slot.head; e != NoEntry; e = previous[e].next_same) {
                if (!previous[e].paired && previous[e].draw == draw) {
                    cursor = size_t(e) + 1;
                    return &previous[e];
                }
            }
            return nullptr;
        }
    }
    void BuildIndex() {
        index_built = true;
        ++stats.indexes;
        size_t size = 64;
        while (size < previous.size() * 2) {
            size *= 2;
        }
        if (index.size() < size) {
            index.assign(size, IndexSlot{0, NoEntry, 0});
            generation = 0;
        }
        if (++generation == 0) {
            for (auto& slot : index) {
                slot.generation = 0;
            }
            generation = 1;
        }
        const size_t mask = index.size() - 1;
        // Backwards: each chain starts at its earliest entry.
        for (size_t e = previous.size(); e-- > 0;) {
            Entry& entry = previous[e];
            for (size_t i = (entry.hash * 0x9e3779b97f4a7c15ULL >> 32) & mask;; i = (i + 1) & mask) {
                IndexSlot& slot = index[i];
                if (slot.generation != generation) {
                    slot = {entry.hash, uint32_t(e), generation};
                    entry.next_same = NoEntry;
                    entry.prev_same = NoEntry;
                    break;
                }
                if (slot.hash == entry.hash) {
                    entry.next_same = slot.head;
                    entry.prev_same = NoEntry;
                    previous[slot.head].prev_same = uint32_t(e);
                    slot.head = uint32_t(e);
                    break;
                }
            }
        }
    }
    uint32_t capacity{}, used{};
    uint64_t frame{};
    std::vector<Entry> previous, current;
    size_t cursor = 0;
    bool index_built = false;
    std::vector<IndexSlot> index;
    uint32_t generation = 0;
    struct GateHash {
        size_t operator()(const GateKey& key) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(key.shader); mix(key.geometry); mix(key.indices); mix(key.index_count);
            mix(key.instances); mix(key.first_instance); mix(key.occurrence);
            return size_t(h);
        }
    };
    FlatMap<GateKey, uint64_t, GateHash> gate_previous, gate_current;
    FlatMap<GateKey, uint32_t, GateHash> gate_occurrences;
};

// The pairing of 0.3.6.68 (a table keyed by draw and occurrence: the k-th of identical draws of a frame takes the k-th's
// store of the previous frame), kept as the reference History must equal: BB_MOTION_HISTORY_CHECK=1 (diagnosis) runs
// it next to History on the real draws and counts allocations that differ; the motion-history test compares the two.
class KeyedHistory {
public:
    explicit KeyedHistory(uint32_t capacity) : capacity{capacity} {}

    void NextFrame() {
        previous.swap(current);
        current.clear();
        ++frame;
        used = 0;
    }

    History::Allocation Prepare(const Draw& draw) {
        const size_t draw_hash = Hash::OfDraw(draw);
        Key key{draw, 0};
        Slot& head = current.At(key, Hash::WithOccurrence(draw_hash, 0));
        key.occurrence = head.seen++;
        const size_t key_hash = Hash::WithOccurrence(draw_hash, key.occurrence);
        const uint64_t count = uint64_t(draw.vertices.count) * draw.instances;
        if (!count || uint64_t(draw.vertices.first) + draw.vertices.count - 1 > INT32_MAX ||
            uint64_t(draw.first_instance) + draw.instances > UINT32_MAX) {
            return {};
        }
        const auto* prev = previous.find(key, key_hash);
        History::Allocation result{0, 0, draw.vertices.count, draw.instances, draw.vertices.first,
                                   draw.first_instance};
        if (prev && prev->store) {
            result.load = prev->store;
        }
        if (count <= capacity - used) {
            result.store = 1 + uint32_t(frame & 1) * capacity + used;
            used += uint32_t(count);
            (key.occurrence ? current.At(key, key_hash) : head).store = result.store;
        }
        return result;
    }

private:
    struct Key {
        Draw draw;
        uint32_t occurrence;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        static size_t OfDraw(const Draw& d) {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(d.shader); mix(d.geometry); mix(d.indices); mix(d.topology);
            mix(d.index_count); mix(d.instances); mix(d.first_instance);
            mix(d.vertices.first); mix(d.vertices.count);
            return size_t(h);
        }
        static size_t WithOccurrence(size_t h, uint32_t occurrence) {
            return h ^ (occurrence + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        }
        size_t operator()(const Key& key) const {
            return WithOccurrence(OfDraw(key.draw), key.occurrence);
        }
    };
    struct Slot {
        uint32_t store{};
        uint32_t seen{};
    };
    uint32_t capacity{}, used{};
    uint64_t frame{};
    FlatMap<Key, Slot, Hash> previous, current;
};

} // namespace Vulkan::Motion
