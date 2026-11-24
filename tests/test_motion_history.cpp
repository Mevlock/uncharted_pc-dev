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
    uint32_t entry = 0;
    VertexShader() {
        Name(flat, "srt_flatbuf");
        Op(71, {position, 11, 0}); // BuiltIn Position
        for (uint32_t i = 0; i < 16; ++i) {
            c[i] = Constant(i);
        }
        Op(54, {0, Id(), 0, 0}); // OpFunction
        entry = Id();
        Op(248, {entry});
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

std::vector<uint16_t> PositionWords(const std::vector<uint32_t>& code, bool expect_valid = true,
                                    std::span<const uint32_t> flat = {}) {
    const auto inputs = PositionCode::Parse(code, "srt_flatbuf", 4, 4)->Inputs(flat);
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
        const auto code = v.End();
        // Without flat data the branch runs both ways.
        assert((PositionWords(code) == std::vector<uint16_t>{3, 5, 9, 11}));
        // With it, word 9 decides: zero skips the block that reads word 11 (the wind's clock for what does not
        // wave); word 9 itself stays an input.
        std::vector<uint32_t> flat(16, 0);
        assert((PositionWords(code, true, flat) == std::vector<uint16_t>{3, 5, 9}));
        const auto parsed = PositionCode::Parse(code, "srt_flatbuf", 4, 4);
        const uint64_t off = parsed->Config(flat);
        flat[11] = 77; // not a branch's word: the same configuration
        assert(parsed->Config(flat) == off);
        flat[9] = 1;
        assert(parsed->Config(flat) != off && parsed->Config(flat) != parsed->Config({}));
        assert((PositionWords(code, true, flat) == std::vector<uint16_t>{3, 5, 9, 11}));
    }
    {
        // Conditions of several comparisons: decided only when their flat words decide them.
        VertexShader v;
        const uint32_t less = v.Value(184, {v.Flat(8), v.c[1]});          // float(word 8) < float bits 1
        const uint32_t unequal = v.Value(182, {v.Flat(9), v.c[0]});       // word 9 != 0.0
        const uint32_t both = v.Value(168, {v.Value(167, {less, unequal})}); // !(less && unequal)
        const uint32_t then_label = v.Id(), merge_label = v.Id();
        v.Op(247, {merge_label, 0});
        v.Op(250, {both, then_label, merge_label});
        v.Op(248, {then_label});
        const uint32_t inner = v.Flat(11);
        v.Op(249, {merge_label});
        v.Op(248, {merge_label});
        v.Op(62, {v.position, v.Value(245, {inner, then_label, v.Flat(5), v.entry})});
        const auto code = v.End();
        std::vector<uint32_t> flat(16, 0);
        // word 9 == 0.0: the and is false whatever word 8 is, the block runs: the first value of the phi.
        assert((PositionWords(code, true, flat) == std::vector<uint16_t>{8, 9, 11}));
        flat[9] = 0x3f800000u; // 1.0, and word 8 (0.0) below the constant's 1.4e-45: the block is skipped
        assert((PositionWords(code, true, flat) == std::vector<uint16_t>{5, 8, 9}));
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
    assert(!PositionCode::Parse(std::vector<uint32_t>{1, 2, 3}, "srt_flatbuf", 4, 4)->Valid());
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
    std::vector<WordRun> runs;
    assert(WithoutCamera(words, 24, 44, runs) && (runs == std::vector<WordRun>{{8, 16}, {40, 4}, {60, 2}}));
    assert(!WithoutCamera({words.begin(), words.begin() + 40}, 24, 44, runs)); // a matrix not wholly read
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
        // (Without flat data: every branch both ways.)
        const auto inputs = PositionCode::Parse(code, "srt_flatbuf", 4, 4)->Inputs({});
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

// The pairing rule with the draws' places (motion_history.h, History), written out plainly: no walk, no bit tables,
// no indexes. A batch's draws, in order, each take the first unpaired draw of the previous frame that is the same
// and stands at the same place; then those left over, in order, each take the first unpaired one of their shape.
class PlainPairing {
public:
    explicit PlainPairing(uint32_t capacity) : capacity{capacity} {}
    void NextFrame() {
        previous.swap(current);
        current.clear();
        ++frame;
        used = 0;
    }
    /// The loads of a batch's draws (all valid, the capacity never runs out).
    std::vector<uint32_t> Batch(const std::vector<Draw>& draws) {
        std::vector<uint32_t> loads(draws.size(), 0);
        std::vector<size_t> rest;
        for (size_t i = 0; i < draws.size(); ++i) {
            const uint32_t count = draws[i].vertices.count * draws[i].instances;
            assert(count && count <= capacity - used);
            current.push_back({draws[i], 1 + uint32_t(frame & 1) * capacity + used, false});
            used += count;
            bool found = false;
            for (auto& prev : previous) {
                if (!prev.paired && prev.draw == draws[i]) {
                    prev.paired = found = true;
                    loads[i] = prev.store;
                    break;
                }
            }
            if (!found) {
                rest.push_back(i);
            }
        }
        for (const size_t i : rest) {
            for (auto& prev : previous) {
                if (!prev.paired && prev.draw.SameShape(draws[i])) {
                    prev.paired = true;
                    loads[i] = prev.store;
                    break;
                }
            }
        }
        return loads;
    }

private:
    struct Item {
        Draw draw;
        uint32_t store;
        bool paired;
    };
    uint32_t capacity{}, used{};
    uint64_t frame{};
    std::vector<Item> previous, current;
};

// Instances of a few meshes at their own places: most stand still, some move every frame, some move now and then,
// some have no known place; every frame the order changes (runs moved, draws swapped: the game sorts by distance),
// the camera culls some and uncovers others, new instances appear. History, run in batches as ObjectMotion runs it
// (PairExact for each draw, PairRest at the batch's end), must give every draw the load of the plain rule. With one
// batch a frame (`whole_frames`: the game submits once a frame) an instance that stood still over two frames it was
// drawn in must load its own store of the frame before, whatever the others did: the k-th of a mesh's draws took the
// k-th of the frame before, wherever each stood, before the place counted.
void RandomPlacement(uint32_t seed, uint32_t objects, bool whole_frames) {
    std::mt19937 rng{seed};
    const auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
    enum Kind : uint32_t { Stands, Moves, MovesAtTimes, NoPlace };
    struct Object {
        Draw draw{};
        Kind kind = Stands;
        uint32_t last_store = 0; ///< its store in the frame before (0: not drawn then)
        uint64_t last_placement = 0;
    };
    std::vector<Draw> meshes;
    for (uint32_t i = 0; i < 1 + objects / 8; ++i) {
        meshes.push_back({.shader = pick(40), .geometry = 1 + i, .indices = 0x1000ull * pick(50), .topology = 0,
                          .index_count = 3 * (1 + pick(100)), .instances = 1, .first_instance = 0,
                          .vertices = {0, 1 + pick(20)}});
    }
    uint64_t next_place = 1;
    const auto make = [&] {
        Object o;
        o.draw = meshes[pick(uint32_t(meshes.size()))];
        o.kind = pick(10) < 6 ? Stands : Kind(1 + pick(3));
        o.draw.placement = o.kind == NoPlace ? 0 : next_place++;
        return o;
    };
    std::vector<Object> scene;
    for (uint32_t i = 0; i < objects; ++i) {
        scene.push_back(make());
    }
    const uint32_t capacity = 1u << 22;
    History history{capacity};
    PlainPairing plain{capacity};
    uint64_t draws = 0, still_pairs = 0, loads = 0;
    for (uint32_t frame = 0; frame < 200; ++frame) {
        const bool skipped = pick(40) == 0;
        for (uint32_t s = 0; s < (skipped ? 2u : 1u); ++s) {
            history.NextFrame();
            plain.NextFrame();
        }
        if (skipped) {
            for (auto& o : scene) {
                o.last_store = 0;
            }
        }
        // The scene changes: objects move, a few leave for good, a few new ones come.
        for (auto& o : scene) {
            if (o.kind == Moves || (o.kind == MovesAtTimes && pick(3) == 0)) {
                o.draw.placement = next_place++;
            }
        }
        for (uint32_t n = pick(4); n-- > 0;) {
            scene[pick(uint32_t(scene.size()))] = make();
        }
        // This frame's order and the draws the camera leaves out.
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < scene.size(); ++i) {
            if (pick(10) != 0) {
                order.push_back(i);
            } else {
                scene[i].last_store = 0;
            }
        }
        for (uint32_t m = pick(4); m-- > 0 && order.size() > 20;) {
            const uint32_t at = pick(uint32_t(order.size()) - 10), len = 1 + pick(10);
            std::vector<uint32_t> run(order.begin() + at, order.begin() + at + len);
            order.erase(order.begin() + at, order.begin() + at + len);
            order.insert(order.begin() + pick(uint32_t(order.size())), run.begin(), run.end());
        }
        for (uint32_t m = pick(8); m-- > 0;) {
            std::swap(order[pick(uint32_t(order.size()))], order[pick(uint32_t(order.size()))]);
        }
        // In batches.
        size_t at = 0;
        while (at < order.size()) {
            size_t end = order.size();
            if (!whole_frames) {
                end = std::min(order.size(), at + 1 + pick(uint32_t(order.size())));
            }
            std::vector<Draw> batch;
            std::vector<History::Allocation> got;
            for (size_t i = at; i < end; ++i) {
                batch.push_back(scene[order[i]].draw);
                auto stored = history.Allocate(batch.back());
                assert(stored.valid && stored.allocation.store);
                got.push_back(stored.allocation);
            }
            for (size_t i = 0; i < batch.size(); ++i) {
                history.PairExact(batch[i], got[i], uint32_t(i));
            }
            history.PairRest([&](uint32_t tag, uint32_t load) { got[tag].load = load; });
            assert(history.Waiting() == 0);
            const auto want = plain.Batch(batch);
            for (size_t i = 0; i < batch.size(); ++i) {
                Object& o = scene[order[at + i]];
                if (got[i].load != want[i]) {
                    std::printf("placement seed %u frame %u draw %zu: load %u, the plain rule %u\n", seed, frame,
                                at + i, got[i].load, want[i]);
                    assert(false);
                }
                if (whole_frames && o.last_store && o.draw.placement && o.draw.placement == o.last_placement) {
                    assert(got[i].load == o.last_store); // it stood still: its own positions
                    ++still_pairs;
                }
                loads += got[i].load != 0;
                ++draws;
                o.last_store = got[i].store;
                o.last_placement = o.draw.placement;
            }
            at = end;
        }
    }
    if (seed <= 2) {
        const auto& s = history.stats;
        std::printf("placement seed %u (%u objects, %s): %llu of %llu draws with history; %llu still instances "
                    "checked against their own store; %llu in order, %llu searched (%llu indexes), %llu moved (%llu "
                    "in order, %llu indexes by shape, %llu from a draw that had stood), %llu found their partner taken\n",
                    seed, objects, whole_frames ? "one batch a frame" : "random batches", (unsigned long long)loads,
                    (unsigned long long)draws, (unsigned long long)still_pairs, (unsigned long long)s.in_order,
                    (unsigned long long)s.searched, (unsigned long long)s.indexes, (unsigned long long)s.moved,
                    (unsigned long long)s.moved_in_order, (unsigned long long)s.shape_indexes,
                    (unsigned long long)s.moved_from_still, (unsigned long long)s.taken);
    }
    assert(history.stats.loaded == loads);
    assert(!whole_frames || history.stats.taken == 0);
}

// Two instances of one mesh that change their order in the frame: the case of the market's two far objects
// (vectors 100-280 px long on things that stood still). By order alone each takes the other's positions.
void SwappedInstances() {
    const Draw mesh{.shader = 7, .geometry = 9, .indices = 0x4000, .topology = 0, .index_count = 300, .instances = 1,
                    .first_instance = 0, .vertices = {0, 100}};
    Draw left = mesh, right = mesh;
    left.placement = 0x1111;
    right.placement = 0x2222;
    History history{1u << 20};
    KeyedHistory keyed{1u << 20};
    history.NextFrame();
    keyed.NextFrame();
    const auto left0 = history.Prepare(left), right0 = history.Prepare(right);
    const auto keyed_left0 = keyed.Prepare(left), keyed_right0 = keyed.Prepare(right);
    history.NextFrame();
    keyed.NextFrame();
    // The right one first now.
    const auto right1 = history.Prepare(right), left1 = history.Prepare(left);
    assert(right1.load == right0.store && left1.load == left0.store);
    const auto keyed_right1 = keyed.Prepare(right), keyed_left1 = keyed.Prepare(left);
    assert(keyed_right1.load == keyed_left0.store && keyed_left1.load == keyed_right0.store); // 0.3.6.68: swapped
    history.NextFrame();
    // The left one culled, a third one uncovered before the right one: nobody takes another's positions.
    Draw third = mesh;
    third.placement = 0x3333;
    History::Allocation third2 = history.Allocate(third).allocation, right2 = history.Allocate(right).allocation;
    const bool third_final = history.PairExact(third, third2, 0);
    const bool right_final = history.PairExact(right, right2, 1);
    history.PairRest([&](uint32_t tag, uint32_t load) { (tag == 0 ? third2 : right2).load = load; });
    assert(!third_final && right_final && right2.load == right1.store);
    assert(third2.load == left1.store); // the one left over of its shape: the left one may have moved there
    history.NextFrame();
    // All three stand: each its own.
    const auto third3 = history.Prepare(third), right3 = history.Prepare(right);
    assert(third3.load == third2.store && right3.load == right2.store);
    history.NextFrame();
    // An instance that moves pairs with itself while the other stands, whatever the order.
    Draw moved = third;
    moved.placement = 0x4444;
    History::Allocation right4 = history.Allocate(right).allocation, moved4 = history.Allocate(moved).allocation;
    history.PairExact(right, right4, 0);
    history.PairExact(moved, moved4, 1);
    history.PairRest([&](uint32_t tag, uint32_t load) { (tag == 0 ? right4 : moved4).load = load; });
    assert(right4.load == right3.store && moved4.load == third3.store);
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
    // Instances of one mesh pair by where they stand (History's two passes against the plain rule).
    SwappedInstances();
    for (uint32_t seed = 1; seed <= 48; ++seed) {
        RandomPlacement(seed, seed % 4 == 0 ? 2500 : 300, seed % 2 == 1);
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
    std::puts("Motion history: PASS (ranges, restart, offsets, matching, in-order = keyed pairing, instances by place, capacity, gating, still draws, position inputs, camera place, index cache, jitter)");
}
