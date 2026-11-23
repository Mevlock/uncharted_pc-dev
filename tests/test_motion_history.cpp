// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks: no game, Vulkan device or window required.
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <tuple>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"

using namespace Vulkan::Motion;

namespace {
// The pairing History had before the in-order walk (0.3.6.68: the k-th of identical draws of a frame takes the
// k-th's store of the previous frame, through a table keyed by draw and occurrence), kept as the reference: the
// in-order History must give every draw exactly the same allocation.
class ReferenceHistory {
public:
    explicit ReferenceHistory(uint32_t capacity) : capacity{capacity} {}
    void NextFrame() {
        previous.swap(current);
        current.clear();
        seen.clear();
        ++frame;
        used = 0;
    }
    History::Allocation Prepare(const Draw& d) {
        const auto draw = Tie(d);
        const uint32_t occurrence = seen[draw]++;
        const uint64_t count = uint64_t(d.vertices.count) * d.instances;
        if (!count || uint64_t(d.vertices.first) + d.vertices.count - 1 > INT32_MAX ||
            uint64_t(d.first_instance) + d.instances > UINT32_MAX) {
            return {};
        }
        History::Allocation result{0, 0, d.vertices.count, d.instances, d.vertices.first, d.first_instance};
        if (const auto it = previous.find({draw, occurrence}); it != previous.end()) {
            result.load = it->second;
        }
        if (count <= capacity - used) {
            result.store = 1 + uint32_t(frame & 1) * capacity + used;
            used += uint32_t(count);
        }
        current[{draw, occurrence}] = result.store;
        return result;
    }

private:
    using Key = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static Key Tie(const Draw& d) {
        return {d.shader, d.geometry, d.indices, d.topology, d.index_count, d.instances, d.first_instance,
                d.vertices.first, d.vertices.count};
    }
    uint32_t capacity{}, used{};
    uint64_t frame{};
    std::map<std::pair<Key, uint32_t>, uint32_t> previous, current;
    std::map<Key, uint32_t> seen;
};

bool Same(const History::Allocation& a, const History::Allocation& b) {
    return a.store == b.store && a.load == b.load && a.vertices == b.vertices && a.instances == b.instances &&
           a.first_vertex == b.first_vertex && a.first_instance == b.first_instance;
}

// Frames like a game's: a scene of draws (many identical ones: instanced props) drawn again each frame with some
// culled, some uncovered, runs moved, new ones, a few invalid, the capacity running out at times, and now and then
// a skipped frame (NextFrame twice).
void RandomEquivalence(uint32_t seed) {
    std::mt19937 rng{seed};
    const auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
    std::vector<Draw> scene;
    for (uint32_t i = 0; i < 400; ++i) {
        Draw d{.shader = pick(40), .geometry = pick(30), .indices = 0x1000ull * pick(50), .topology = pick(3),
               .index_count = 3 * (1 + pick(100)), .instances = 1 + pick(3), .first_instance = pick(4),
               .vertices = {pick(1000), 1 + pick(200)}};
        if (pick(50) == 0) {
            d.vertices.count = 0; // invalid
        }
        const uint32_t copies = pick(6) == 0 ? 1 + pick(5) : 1;
        for (uint32_t c = 0; c < copies; ++c) {
            scene.push_back(d);
        }
    }
    History history{110000};
    ReferenceHistory reference{110000};
    uint64_t loads = 0, draws = 0;
    for (uint32_t frame = 0; frame < 300; ++frame) {
        const uint32_t skips = pick(40) == 0 ? 2 : 1;
        for (uint32_t s = 0; s < skips; ++s) {
            history.NextFrame();
            reference.NextFrame();
        }
        // This frame's order: mostly the scene's, some draws culled, runs moved elsewhere, some swapped.
        std::vector<Draw> order;
        for (const auto& d : scene) {
            if (pick(10) != 0) {
                order.push_back(d);
            }
        }
        for (uint32_t m = pick(4); m-- > 0 && order.size() > 20;) {
            const uint32_t at = pick(uint32_t(order.size()) - 10), len = 1 + pick(10);
            std::vector<Draw> run(order.begin() + at, order.begin() + at + len);
            order.erase(order.begin() + at, order.begin() + at + len);
            order.insert(order.begin() + pick(uint32_t(order.size())), run.begin(), run.end());
        }
        for (uint32_t m = pick(6); m-- > 0;) {
            std::swap(order[pick(uint32_t(order.size()))], order[pick(uint32_t(order.size()))]);
        }
        for (uint32_t n = pick(5); n-- > 0;) {
            Draw d = scene[pick(uint32_t(scene.size()))];
            d.shader += 1000 + pick(3);
            order.insert(order.begin() + pick(uint32_t(order.size())), d);
        }
        if (pick(8) == 0) { // a crowded frame: the capacity runs out
            for (uint32_t n = 0; n < 200; ++n) {
                order.push_back(scene[pick(uint32_t(scene.size()))]);
            }
        }
        for (const auto& d : order) {
            const auto got = history.Prepare(d);
            const auto want = reference.Prepare(d);
            if (!Same(got, want)) {
                std::printf("seed %u frame %u: store %u/%u load %u/%u\n", seed, frame, got.store, want.store,
                            got.load, want.load);
                assert(false);
            }
            loads += got.load != 0;
            ++draws;
        }
    }
    if (seed <= 3) {
        std::printf("seed %u: %llu of %llu draws with history (%llu capacity skips, %llu invalid)\n", seed,
                    (unsigned long long)loads, (unsigned long long)draws,
                    (unsigned long long)history.stats.exhausted, (unsigned long long)history.stats.invalid);
    }
    assert(history.stats.exhausted && history.stats.invalid); // both edge cases were exercised
    assert(loads > draws / 2); // most draws found their history (the test pairs, not only stores)
}
} // namespace

int main() {
    const std::array<uint16_t, 5> indices{1000, 1007, 1002, 1007, UINT16_MAX};
    const auto range = IndexedRange(std::span(indices), -900, true);
    assert(range.first == 100 && range.count == 8);
    assert(IndexedRange(std::span(indices), -1001, true).count == 0);
    assert(IndexedRange(std::span(indices), 0, false).count == 64536);
    const std::array<uint32_t, 3> large{100000, 100003, UINT32_MAX};
    assert(IndexedRange(std::span(large), 7, true).count == 4);
    assert(IndexedRange(std::span(large), INT32_MAX, true).count == 0);
    const std::array<uint16_t, 1> restart{UINT16_MAX};
    assert(IndexedRange(std::span(restart), 0, true).count == 0);
    assert(IndexedRange(std::span<const uint16_t>{}, 0, false).count == 0);

    Draw draw{.shader = 1, .geometry = 2, .indices = 3, .topology = 4,
              .index_count = 5, .instances = 2, .first_instance = 100,
              .vertices = range};
    History history(32);
    history.NextFrame();
    const auto first = history.Prepare(draw);
    const auto duplicate = history.Prepare(draw);
    assert(first.store && !first.load && first.first_vertex == 100);
    assert(first.first_instance == 100 && first.instances == 2);
    assert(duplicate.store == first.store + 16); // shared geometry, different draw
    assert(history.Used() == 32); // firstInstance must NOT multiply the allocation
    assert(!history.Prepare(draw).store && history.stats.exhausted == 1);
    history.NextFrame();
    const auto next = history.Prepare(draw);
    assert(next.load == first.store && next.store != first.store);
    assert(history.Prepare(draw).load == duplicate.store);
    history.NextFrame(); // a missing frame invalidates the match
    history.NextFrame();
    assert(!history.Prepare(draw).load);

    // A changed index buffer may reference formerly unused holes inside the same min/max.
    // Do not accept that history merely because its addresses/counts stayed the same.
    history.NextFrame();
    auto changed = draw;
    changed.topology++;
    assert(!history.Prepare(changed).load);
    changed = draw;
    changed.first_instance++;
    assert(!history.Prepare(changed).load);
    history.NextFrame();
    changed = draw;
    changed.vertices.first++;
    assert(!history.Prepare(changed).load);

    // Out of order (culling): a draw found through the index, identical draws paired k-th with k-th.
    {
        History order(1024);
        Draw a = draw, b = draw, c = draw;
        b.shader = 11;
        c.shader = 12;
        order.NextFrame();
        const auto a1 = order.Prepare(a), b1 = order.Prepare(b), c1 = order.Prepare(c), a2 = order.Prepare(a);
        order.NextFrame();
        assert(order.Prepare(c).load == c1.store); // out of order: the index
        assert(order.Prepare(a).load == a1.store); // the first a, not the second behind c
        assert(order.Prepare(b).load == b1.store);
        assert(order.Prepare(a).load == a2.store);
        assert(!order.Prepare(a).load); // a third a: none left
        order.NextFrame();
        const auto c2 = order.Prepare(c);
        assert(c2.load);
        Draw d = draw;
        d.shader = 13;
        assert(!order.Prepare(d).load); // new: unmatched, the walk goes on
        assert(order.Prepare(a).load && order.Prepare(b).load && order.Prepare(a).load);
    }

    for (uint32_t seed = 1; seed <= 64; ++seed) {
        RandomEquivalence(seed);
    }

    History overflow(32);
    draw.vertices.first = 0;
    draw.vertices.count = INT32_MAX;
    draw.instances = UINT32_MAX;
    draw.first_instance = 0;
    assert(!overflow.Prepare(draw).store);
    assert(overflow.stats.exhausted == 1);
    draw.first_instance = 10;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 1);
    draw.first_instance = 0;
    draw.vertices.first = INT32_MAX;
    draw.vertices.count = 2;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 2);

    // Small skeletons: history only while the bone palette changes (weapon spinning).
    History gated(1024);
    const History::GateKey weapon{.shader = 7, .index_count = 3, .instances = 1};
    const auto frame = [&](uint64_t palette) {
        gated.NextFrame();
        return gated.Moving(weapon, palette);
    };
    assert(!gated.Moving(weapon, 1) && gated.stats.still == 1); // first sight
    assert(!frame(1));                    // same bones: camera motion
    assert(frame(2) && frame(3));         // spinning
    assert(!frame(3));                    // stopped
    gated.NextFrame();
    gated.NextFrame();
    assert(!gated.Moving(weapon, 4));     // after a skipped frame: no comparison
    assert(!gated.Moving(weapon, 5));     // second occurrence of the same draw
    assert(ClassifyBuffer(864) == BufferRole::Other && ClassifyBuffer(416) == BufferRole::Other);
    assert(ClassifyBuffer(96) == BufferRole::SmallSkeleton &&
           ClassifyBuffer(384) == BufferRole::SmallSkeleton);
    assert(ClassifyBuffer(656) == BufferRole::Skeleton && ClassifyBuffer(64) == BufferRole::Other);

    // Index ranges: reused between full scans, rescanned every Revalidate frames, trimmed.
    IndexRangeCache ranges;
    uint32_t scans = 0;
    const IndexRangeCache::Key mesh{.address = 0x1000, .count = 3, .index_size = 2};
    const auto scan = [&] {
        ++scans;
        return IndexRangeCache::Result{{4, 3}, 99};
    };
    assert(ranges.Get(mesh, 0, scan).topology == 99 && scans == 1);
    assert(ranges.Get(mesh, IndexRangeCache::Revalidate - 1, scan).range == (VertexRange{4, 3}));
    assert(scans == 1);
    ranges.Get(mesh, IndexRangeCache::Revalidate, scan);
    assert(scans == 2);
    ranges.Trim(IndexRangeCache::Revalidate + IndexRangeCache::Unused + 1);
    assert(ranges.Size() == 0);

    assert(JitterPhases(1920, 1920) == 8);
    assert(JitterPhases(1280, 1920) == 18);
    assert(JitterPhases(960, 1920) == 32);
    assert(JitterPhases(640, 1920) == 72);
    assert(JitterPhases(0, 1920) == 8);
    std::puts("Motion history: PASS (ranges, restart, offsets, matching, in-order = keyed pairing, capacity, gating, index cache, jitter)");
}
