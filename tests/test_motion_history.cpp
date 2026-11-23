// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks: no game, Vulkan device or window required.
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <tuple>
#include <vector>
#include <cstring>
#include <initializer_list>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"
#include "gpu/shadps4/video_core/renderer_vulkan/motion_still.h"

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

// A few SPIR-V instructions put together by hand, for PositionInputs (only what it reads: no valid module).
struct Spv {
    std::vector<uint32_t> words{0x07230203u, 0x00010600u, 0, 0, 0};
    uint32_t next = 1;
    uint32_t Id() {
        return next++;
    }
    void Op(uint32_t op, std::initializer_list<uint32_t> operands) {
        words.push_back(uint32_t(operands.size() + 1) << 16 | op);
        words.insert(words.end(), operands);
    }
    /// An instruction with a result: its id.
    uint32_t Value(uint32_t op, std::initializer_list<uint32_t> operands) {
        const uint32_t id = Id();
        words.push_back(uint32_t(operands.size() + 3) << 16 | op);
        words.push_back(0); // the type: not read
        words.push_back(id);
        words.insert(words.end(), operands);
        return id;
    }
    void Name(uint32_t id, const char* name) {
        const size_t length = std::strlen(name) / 4 + 1;
        std::vector<uint32_t> text(length, 0);
        std::memcpy(text.data(), name, std::strlen(name));
        words.push_back(uint32_t(length + 2) << 16 | 5);
        words.push_back(id);
        words.insert(words.end(), text.begin(), text.end());
    }
    uint32_t Constant(uint32_t value) {
        return Value(43, {value});
    }
    uint32_t Variable(uint32_t storage) {
        return Value(59, {storage});
    }
    std::vector<uint32_t> Done() {
        words[3] = next;
        return words;
    }
};

// The shape of Uncharted's vertex shaders: constants from the flat data, a pointer word loaded and never used, a
// branch on a flat word, a stream read at an index from a user-data register, another output from another word.
struct VertexShader : Spv {
    uint32_t flat = Variable(12), position = Variable(3), other = Variable(3), push = Variable(9),
             stream = Variable(12), texture = Variable(0);
    uint32_t c[16];
    VertexShader() {
        Name(flat, "srt_flatbuf");
        Op(71, {position, 11, 0}); // BuiltIn Position
        for (uint32_t i = 0; i < 16; ++i) {
            c[i] = Constant(i);
        }
        Op(54, {0, Id(), 0, 0}); // OpFunction
        Op(248, {Id()});
    }
    uint32_t Flat(uint32_t word) {
        return Value(61, {Value(65, {flat, c[0], c[word]})});
    }
    std::vector<uint32_t> End() {
        Op(253, {});
        Op(56, {});
        return Done();
    }
};

std::vector<uint16_t> PositionWords(const std::vector<uint32_t>& code, bool expect_valid = true) {
    const auto inputs = PositionInputs::FromSpirv(code, "srt_flatbuf", 4, 4);
    assert(inputs.valid == expect_valid);
    return inputs.words;
}

void PositionInputsTest() {
    {
        VertexShader v;
        const uint32_t object = v.Flat(5);
        v.Flat(7); // a pointer word: read, used by nothing
        const uint32_t condition = v.Value(171, {v.Flat(9), v.c[0]});
        const uint32_t then_label = v.Id(), merge_label = v.Id();
        v.Op(247, {merge_label, 0});
        v.Op(250, {condition, then_label, merge_label});
        v.Op(248, {then_label});
        const uint32_t waved = v.Value(128, {object, v.Flat(11)});
        v.Op(249, {merge_label});
        v.Op(248, {merge_label});
        const uint32_t phi = v.Value(245, {waved, then_label, object, 0});
        const uint32_t reg = v.Value(61, {v.Value(65, {v.push, v.c[4], v.c[3]})}); // user-data register 3
        const uint32_t vertex = v.Value(61, {v.Value(65, {v.stream, v.c[0], reg})});
        v.Op(62, {v.position, v.Value(128, {phi, vertex})});
        v.Op(62, {v.other, v.Flat(13)});
        // The other push members (viewport, buffer offsets) are not user data.
        v.Op(62, {v.position, v.Value(61, {v.Value(65, {v.push, v.c[2]})})});
        assert((PositionWords(v.End()) == std::vector<uint16_t>{3, 5, 9, 11}));
    }
    {
        // Through a variable: every store to it counts; a store through a chain into the position too.
        VertexShader v;
        const uint32_t local = v.Variable(7);
        v.Op(62, {local, v.Flat(5)});
        v.Op(62, {local, v.Flat(6)});
        v.Op(62, {v.Value(65, {v.position, v.c[1]}), v.Value(61, {local})});
        v.Op(62, {v.other, v.Flat(13)});
        assert((PositionWords(v.End()) == std::vector<uint16_t>{5, 6}));
    }
    {
        // The position read back and stored again (the emitter's viewport and motion code): the first store counts.
        VertexShader v;
        v.Op(62, {v.position, v.Flat(5)});
        v.Op(62, {v.position, v.Value(128, {v.Value(61, {v.position}), v.c[1]})});
        assert((PositionWords(v.End()) == std::vector<uint16_t>{5}));
    }
    {
        VertexShader v; // a texture
        v.Op(62, {v.position, v.Value(61, {v.texture})});
        PositionWords(v.End(), false);
    }
    {
        VertexShader v; // a read through a device address
        v.Op(62, {v.position, v.Value(61, {v.Value(120, {v.Flat(5)})})});
        PositionWords(v.End(), false);
    }
    {
        VertexShader v; // a flat word at a computed index
        v.Op(62, {v.position, v.Value(61, {v.Value(65, {v.flat, v.c[0], v.Flat(5)})})});
        PositionWords(v.End(), false);
    }
    {
        VertexShader v; // a call
        v.Op(62, {v.position, v.Value(57, {v.Id()})});
        PositionWords(v.End(), false);
    }
    {
        VertexShader v; // no position stored
        v.Op(62, {v.other, v.Flat(5)});
        PositionWords(v.End(), false);
    }
    assert(!PositionInputs::FromSpirv(std::vector<uint32_t>{1, 2, 3}, "srt_flatbuf", 4, 4).valid);
}

void CameraTest() {
    // Flat data of 80 words: the object matrix at 10..21, the view at 24..39, the projection at 44..59.
    std::vector<uint32_t> flat(80, 0);
    const auto put = [&flat](size_t at, std::initializer_list<float> values) {
        for (const float value : values) {
            std::memcpy(&flat[at++], &value, 4);
        }
    };
    put(10, {1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7});
    const float view[16] = {0.8f, 0, -0.6f, 12.5f, 0, 1, 0, -3.25f, 0.6f, 0, 0.8f, -140.0f, 0, 0, 0, 1};
    const float proj[16] = {1.3f, 0, 0.002f, 0, 0, 2.3f, -0.001f, 0, 0, 0, -1.0001f, -0.2f, 0, 0, -1, 0};
    std::memcpy(&flat[24], view, sizeof(view));
    std::memcpy(&flat[44], proj, sizeof(proj));
    float clip[16];
    for (int r = 0; r < 4; ++r) {
        for (int col = 0; col < 4; ++col) {
            double sum = 0; // the game's own product: another order and precision than ProductIs
            for (int k = 3; k >= 0; --k) {
                sum += double(proj[r * 4 + k]) * double(view[k * 4 + col]);
            }
            clip[r * 4 + col] = float(sum);
        }
    }
    std::vector<uint16_t> words;
    for (uint16_t w = 8; w < 62; ++w) {
        words.push_back(w);
    }
    uint16_t view_at = 0, proj_at = 0;
    assert(FindCamera(flat, words, clip, view_at, proj_at) && view_at == 24 && proj_at == 44);
    assert(ProductIs(&flat[24], &flat[44], clip, CameraTolerance));
    // Another camera (a turn of a hundredth), a truncated word list, a second copy of the pair: no camera.
    float turned[16];
    std::memcpy(turned, clip, sizeof(clip));
    turned[0] *= 1.01f;
    assert(!FindCamera(flat, words, turned, view_at, proj_at));
    assert(!FindCamera(flat, {words.begin(), words.begin() + 40}, clip, view_at, proj_at));
    StillProgram program;
    program.inputs.valid = true;
    program.inputs.words = words;
    assert(program.LearnCamera(flat, clip) && program.state == StillProgram::Ready);
    assert((program.key_runs == std::vector<WordRun>{{8, 16}, {40, 4}, {60, 2}}));
    auto doubled = flat;
    doubled.resize(160, 0);
    std::memcpy(&doubled[100], view, sizeof(view));
    std::memcpy(&doubled[120], proj, sizeof(proj));
    auto more = words;
    for (uint16_t w = 100; w < 136; ++w) {
        more.push_back(w);
    }
    assert(!FindCamera(doubled, more, clip, view_at, proj_at));
    assert((Runs({1, 2, 3, 7, 9, 10}) == std::vector<WordRun>{{1, 3}, {7, 1}, {9, 2}}));
}

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
    KeyedHistory keyed{110000}; // the same, as BB_MOTION_HISTORY_CHECK runs it in the game
    // As ObjectMotion runs it: Allocate at each draw, Pair for the draws with a store later in batches (at the
    // submissions: here at random points and before every NextFrame), a draw without a store paired at once after
    // the batch.
    History deferred{110000};
    struct Pending {
        Draw draw;
        History::Allocation allocation, want;
    };
    std::vector<Pending> pending;
    const auto flush = [&] {
        for (auto& p : pending) {
            deferred.Pair(p.draw, p.allocation);
            assert(Same(p.allocation, p.want));
        }
        pending.clear();
    };
    uint64_t loads = 0, draws = 0;
    for (uint32_t frame = 0; frame < 300; ++frame) {
        const uint32_t skips = pick(40) == 0 ? 2 : 1;
        for (uint32_t s = 0; s < skips; ++s) {
            flush();
            deferred.NextFrame();
            history.NextFrame();
            reference.NextFrame();
            keyed.NextFrame();
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
            assert(Same(keyed.Prepare(d), want));
            auto stored = deferred.Allocate(d);
            if (!stored.valid) {
                assert(Same(stored.allocation, want) && Same(History::Allocation{}, want));
            } else if (!stored.allocation.store) {
                flush();
                deferred.Pair(d, stored.allocation);
                assert(Same(stored.allocation, want));
            } else {
                pending.push_back({d, stored.allocation, want});
                if (pick(30) == 0) {
                    flush();
                }
            }
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
    flush();
    assert(deferred.stats.draws == history.stats.draws && deferred.stats.loaded == history.stats.loaded &&
           deferred.stats.exhausted == history.stats.exhausted && deferred.stats.invalid == history.stats.invalid);
    assert(history.stats.exhausted && history.stats.invalid); // both edge cases were exercised
    assert(loads > draws / 2); // most draws found their history (the test pairs, not only stores)
}
} // namespace

// With SPIR-V files as arguments (a local shader cache's binaries): each file's position inputs on a line, for a
// look at real shaders; no checks.
int ListPositionInputs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::vector<uint32_t> code;
        if (FILE* file = std::fopen(argv[i], "rb")) {
            uint32_t word = 0;
            while (std::fread(&word, 4, 1, file) == 1) {
                code.push_back(word);
            }
            std::fclose(file);
        }
        const auto inputs = PositionInputs::FromSpirv(code, "srt_flatbuf", 4, 4);
        std::printf("%s: ", argv[i]);
        if (!inputs.valid) {
            std::printf("no (%s)\n", inputs.why);
            continue;
        }
        std::printf("%zu words:", inputs.words.size());
        for (const auto& run : Runs(inputs.words)) {
            std::printf(run.count > 1 ? " %u-%u" : " %u", unsigned(run.first), unsigned(run.first + run.count - 1));
        }
        std::printf("\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        return ListPositionInputs(argc, argv);
    }
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

    // Still draws: a key counts when it was drawn in the frame before, however often and in whatever order.
    {
        StillDraws still;
        still.NextFrame();
        assert(!still.Seen(10) && !still.Seen(10) && !still.Seen(0)); // new this frame, twice the same answer
        still.NextFrame();
        assert(still.Seen(0) && still.Seen(10) && still.Seen(10) && !still.Seen(20));
        still.NextFrame();
        assert(still.Seen(20) && still.Seen(10));
        still.NextFrame(); // 20 moved away (another key), 10 not drawn
        assert(!still.Seen(30));
        still.NextFrame();
        assert(!still.Seen(20) && !still.Seen(10) && still.Seen(30)); // back after a frame away: not still
        still.NextFrame();
        assert(still.Seen(20) && still.Seen(10) && still.Seen(30));
        still.Forget(); // a cut
        still.NextFrame();
        assert(!still.Seen(20) && !still.Seen(10));
        // Against a plain model over many frames, with the table growing, trimming and shrinking.
        StillDraws table;
        std::map<uint64_t, uint32_t> last; // key -> the last frame drawn
        std::mt19937_64 rng{7};
        for (uint32_t frame = 1; frame <= 400; ++frame) {
            table.NextFrame();
            const uint32_t draws = frame % 50 < 25 ? 3000 : 40;
            std::map<uint64_t, bool> answers;
            for (uint32_t i = 0; i < draws; ++i) {
                // Most keys repeat from frame to frame; some change every frame.
                const uint64_t key = rng() % 4 ? (rng() % 2500) * 2 : rng() * 2;
                const auto it = last.find(key);
                const bool expected = answers.contains(key) ? answers[key]
                                                             : it != last.end() && it->second + 1 == frame;
                answers.emplace(key, expected);
                assert(table.Seen(key) == expected);
            }
            for (const auto& [key, answer] : answers) {
                last[key] = frame;
            }
            assert(table.Size() <= 2 * 3000 + 64 * 3000);
        }
    }

    PositionInputsTest();
    CameraTest();

    assert(JitterPhases(1920, 1920) == 8);
    assert(JitterPhases(1280, 1920) == 18);
    assert(JitterPhases(960, 1920) == 32);
    assert(JitterPhases(640, 1920) == 72);
    assert(JitterPhases(0, 1920) == 8);
    std::puts("Motion history: PASS (ranges, restart, offsets, matching, in-order = keyed pairing, capacity, gating, still draws, position inputs, camera place, index cache, jitter)");
}
