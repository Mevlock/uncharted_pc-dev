// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (object motion, still draws): what a vertex shader's position follows from, read from its SPIR-V.
//
// Uncharted's vertex shaders take their constants (object matrix, view, projection, wind) from the flattened user
// data, next to pointers and descriptors that change every frame without changing anything drawn, and a wind block
// with a clock that only matters to what waves. Which flat words reach the position is a property of the shader's
// code and, where the code branches on flat words alone (wind off), of the draw's flat data: PositionCode slices the
// module backwards from every store to the Position built-in, through the blocks this draw's flat data lets run.
//
// The slice takes the safe side everywhere. A branch whose condition is not decided by flat words and constants
// alone runs both ways; every condition of a branch that runs is an input of the position (so a decided branch's
// flat words are in the key: when they change, the draw is another); a variable read depends on every store to it in
// a block that runs; and anything it cannot follow (a function call, a read through a computed pointer or a device
// address, a texture, a flat word at a computed index) makes the result invalid, so the draw is never taken for
// still. No Vulkan or shader recompiler types: the regression test includes this header alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Vulkan::Motion {

struct PositionInputs {
    bool valid = false;          ///< `words` is all of the flat data the position reads for this draw
    std::vector<uint16_t> words; ///< flat word indices, ascending (user-data registers read from the push block
                                 ///< count as the flat words they are copied to, 0..15)
    const char* why = "";        ///< valid == false: the reason, for the log
};

class PositionCode {
public:
    /// `flat_name`: the flat buffer's variable name; `ud_member`, `ud_members`: the push block's members holding
    /// the user-data registers (four words each). Never null; Valid() false with Why() when the module's shape is
    /// not understood.
    static std::unique_ptr<PositionCode> Parse(std::span<const uint32_t> code, std::string_view flat_name,
                                               uint32_t ud_member, uint32_t ud_members) {
        auto result = std::unique_ptr<PositionCode>(new PositionCode);
        result->code.assign(code.begin(), code.end());
        result->ud_member = ud_member;
        result->ud_members = ud_members;
        if (const char* error = result->Scan(flat_name)) {
            result->why = error;
        }
        return result;
    }
    bool Valid() const {
        return !why;
    }
    const char* Why() const {
        return why ? why : "";
    }

    /// Which way the branches go that `flat` decides: two bits a conditional branch that runs, in block order (1:
    /// its first target, 2: its second, 3: both). Draws with the same value run the same blocks and so read the same
    /// words. NoConfig: not understood, or more blocks than the walk holds.
    static constexpr uint64_t NoConfig = ~0ull;
    /// The flat words the walk's decisions read, with their values: another draw whose flat data has the same
    /// values there walks the same way (the walk reads nothing else), so it has the same Config without walking.
    struct Guards {
        static constexpr uint32_t Max = 8;
        uint32_t count = 0; ///< Max + 1: more than fit (no match without walking)
        uint16_t word[Max]{};
        uint32_t value[Max]{};
        bool Match(std::span<const uint32_t> flat) const {
            if (count > Max) {
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (word[i] >= flat.size() || flat[word[i]] != value[i]) {
                    return false;
                }
            }
            return true;
        }
    };
    uint64_t Config(std::span<const uint32_t> flat, Guards* guards = nullptr) const {
        Walk walk;
        walk.guards = guards;
        return !why && Run(flat, walk) ? walk.config : NoConfig;
    }

    /// The position's flat words for a draw with this flat data (the same for every draw with its Config).
    PositionInputs Inputs(std::span<const uint32_t> flat) const {
        PositionInputs result;
        if (why) {
            result.why = why;
            return result;
        }
        Walk walk;
        if (!Run(flat, walk)) {
            result.why = "too many blocks";
            return result;
        }
        Slice slice{*this, walk};
        if (const char* error = slice.Run()) {
            result.why = error;
            return result;
        }
        result.valid = true;
        result.words = std::move(slice.words);
        return result;
    }

private:
    enum : uint32_t {
        OpName = 5, OpExtInst = 12, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpFunction = 54,
        OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63,
        OpCopyMemorySized = 64, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
        OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeExtract = 81, OpCompositeInsert = 82, OpBitcast = 124,
        OpLogicalEqual = 164, OpLogicalNotEqual = 165, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168,
        OpIEqual = 170, OpINotEqual = 171, OpFOrdEqual = 180, OpFUnordGreaterThanEqual = 191, OpAtomicLoad = 227,
        OpPhi = 245, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251,
        DecorationBuiltIn = 11, BuiltInPosition = 0,
        StorageUniformConstant = 0, StorageInput = 1, StorageUniform = 2, StorageOutput = 3, StoragePrivate = 6,
        StorageFunction = 7, StoragePushConstant = 9, StorageBuffer = 12,
        None = 0xffffffffu,
    };
    static constexpr uint32_t MaxBranches = 31, MaxBlocks = 64;

    struct Block {
        uint32_t label{};
        uint32_t condition = None;       ///< a conditional branch's condition, a switch's selector
        uint32_t targets[2]{None, None}; ///< block indices: the branch's target, or the conditional's two
        bool conditional = false;
        bool all = false;                ///< a switch: every target in `more` is taken
        std::vector<uint32_t> more;
    };
    struct Store {
        uint32_t block, value;
    };
    /// The blocks a draw's flat data lets run.
    struct Walk {
        uint64_t config = 0;
        uint8_t live[MaxBlocks]{};
        uint8_t taken[MaxBlocks]{}; ///< per block with a conditional branch that runs: 1, 2 or 3 (as in Config)
        Guards* guards = nullptr;   ///< filled with the flat words the decisions read
    };

    PositionCode() = default;

    /// Instructions of a function body without a result (type, id) pair.
    static bool HasResult(uint32_t op) {
        switch (op) {
        case 0: case 8: case 62: case 63: case 64: case 99: case 218: case 219: case 220: case 221: case 224:
        case 225: case 228: case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253:
        case 254: case 255: case 317: case 319:
            return false;
        default:
            return true;
        }
    }
    static std::string_view Literal(const uint32_t* words, uint32_t count) {
        const char* text = reinterpret_cast<const char*>(words);
        size_t length = 0;
        while (length < size_t(count) * 4 && text[length]) {
            ++length;
        }
        return {text, length};
    }
    bool IsChain(uint32_t id) const {
        if (id >= def.size() || !def[id]) {
            return false;
        }
        const uint32_t op = code[def[id]] & 0xffffu;
        return op == OpAccessChain || op == OpInBoundsAccessChain;
    }
    /// The variable a pointer leads into (through access chains), or None.
    uint32_t Root(uint32_t pointer) const {
        for (int depth = 0; depth < 8 && pointer < def.size(); ++depth) {
            if (storage[pointer] != None) {
                return pointer;
            }
            if (!IsChain(pointer)) {
                return None;
            }
            pointer = code[def[pointer] + 3];
        }
        return None;
    }
    /// The flat word a pointer reads (a chain into the flat buffer with constant indices), or None.
    uint32_t FlatWord(uint32_t pointer) const {
        if (!IsChain(pointer)) {
            return None;
        }
        const uint32_t* w = &code[def[pointer]];
        if (w[3] != flat_var || (w[0] >> 16) != 6 || w[5] >= constant.size()) {
            return None;
        }
        return constant[w[5]];
    }

    // One pass: names, decorations, constants, variables, definitions, blocks, stores.
    const char* Scan(std::string_view flat_name) {
        if (code.size() < 5 || code[0] != 0x07230203u) {
            return "not SPIR-V";
        }
        const uint32_t bound = code[3];
        if (bound == 0 || bound > (1u << 22)) {
            return "id bound";
        }
        def.assign(bound, 0);
        def_block.assign(bound, None);
        constant.assign(bound, None);
        boolean.assign(bound, 2);
        storage.assign(bound, None);
        stores.assign(bound, {});
        std::vector<uint32_t> label_block(bound, None);
        struct Pending {
            uint32_t block, a, b;
            std::vector<uint32_t> more;
            bool is_switch;
        };
        std::vector<Pending> terminators;
        bool in_function = false;
        uint32_t functions = 0;
        for (size_t at = 5; at < code.size();) {
            const uint32_t count = code[at] >> 16, op = code[at] & 0xffffu;
            if (count == 0 || at + count > code.size()) {
                return "instruction length";
            }
            const uint32_t* w = &code[at];
            const uint32_t block = blocks.empty() ? uint32_t(None) : uint32_t(blocks.size() - 1);
            switch (op) {
            case OpName:
                if (count >= 3 && w[1] < bound && Literal(w + 2, count - 2) == flat_name) {
                    flat_var = w[1];
                }
                break;
            case OpDecorate:
                if (count >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition && w[1] < bound) {
                    position_var = w[1];
                }
                break;
            case OpMemberDecorate:
                if (count >= 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition) {
                    return "the position is a block member";
                }
                break;
            case OpConstant:
                if (count >= 4 && w[2] < bound) {
                    constant[w[2]] = w[3];
                }
                break;
            case OpConstantTrue:
            case OpConstantFalse:
                if (count >= 3 && w[2] < bound) {
                    boolean[w[2]] = op == OpConstantTrue;
                }
                break;
            case OpVariable:
                if (count >= 4 && w[2] < bound) {
                    storage[w[2]] = w[3];
                }
                break;
            case OpFunction:
                in_function = true;
                ++functions;
                break;
            case OpFunctionEnd:
                in_function = false;
                break;
            case OpFunctionCall:
            case OpCopyMemory:
            case OpCopyMemorySized:
                return "a call or memory copy";
            case OpLabel:
                if (in_function && count >= 2 && w[1] < bound) {
                    label_block[w[1]] = uint32_t(blocks.size());
                    blocks.push_back({});
                    blocks.back().label = w[1];
                }
                break;
            case OpStore:
                if (in_function && count >= 3 && block != None) {
                    // (A store through a pointer that is not a variable's, the motion twin's stores by device
                    // address, cannot change a variable the position reads.)
                    if (const uint32_t root = Root(w[1]); root != None) {
                        stores[root].push_back({block, w[2]});
                    }
                }
                break;
            case OpBranch:
                if (in_function && count >= 2 && block != None) {
                    terminators.push_back({block, w[1], None, {}, false});
                }
                break;
            case OpBranchConditional:
                if (in_function && count >= 4 && block != None) {
                    blocks[block].condition = w[1];
                    blocks[block].conditional = true;
                    terminators.push_back({block, w[2], w[3], {}, false});
                }
                break;
            case OpSwitch:
                if (in_function && count >= 3 && block != None) {
                    blocks[block].condition = w[1];
                    Pending pending{block, None, None, {w[2]}, true};
                    for (uint32_t i = 4; i < count; i += 2) {
                        pending.more.push_back(w[i]);
                    }
                    terminators.push_back(std::move(pending));
                }
                break;
            default:
                if (in_function && HasResult(op) && count >= 3 && w[2] < bound) {
                    def[w[2]] = uint32_t(at);
                    def_block[w[2]] = block;
                }
                break;
            }
            at += count;
        }
        if (functions != 1 || blocks.empty()) {
            return "not one function";
        }
        if (blocks.size() > MaxBlocks) {
            return "too many blocks";
        }
        if (!position_var) {
            return "no position output";
        }
        if (stores[position_var].empty()) {
            return "the position is not stored";
        }
        uint32_t conditionals = 0;
        for (auto& t : terminators) {
            Block& block = blocks[t.block];
            const auto target = [&](uint32_t label) { return label < bound ? label_block[label] : uint32_t(None); };
            if (t.is_switch) {
                block.all = true;
                for (const uint32_t label : t.more) {
                    if (target(label) == None) {
                        return "a branch to nowhere";
                    }
                    block.more.push_back(target(label));
                }
                continue;
            }
            block.targets[0] = target(t.a);
            if (block.targets[0] == None) {
                return "a branch to nowhere";
            }
            if (block.conditional) {
                block.targets[1] = target(t.b);
                if (block.targets[1] == None) {
                    return "a branch to nowhere";
                }
                ++conditionals;
            }
        }
        if (conditionals > MaxBranches) {
            return "too many branches";
        }
        return nullptr;
    }

    /// A value that flat words and constants alone decide: its 32 bits (a boolean: 0 or 1). False: not decided.
    bool Value(uint32_t id, std::span<const uint32_t> flat, uint32_t& out, Guards* guards, int depth = 0) const {
        if (id >= def.size() || depth > 12) {
            return false;
        }
        if (!def[id]) {
            // (A constant 0xffffffff reads as undecided: the safe side.)
            if (constant[id] != None) {
                out = constant[id];
                return true;
            }
            if (boolean[id] != 2) {
                out = boolean[id];
                return true;
            }
            return false;
        }
        const uint32_t* w = &code[def[id]];
        const uint32_t count = w[0] >> 16, op = w[0] & 0xffffu;
        uint32_t a = 0, b = 0;
        switch (op) {
        case OpLoad: {
            const uint32_t word = FlatWord(w[3]);
            if (word == None || word >= flat.size()) {
                return false;
            }
            out = flat[word];
            if (guards) {
                // (A word read twice is noted twice: harmless.)
                if (guards->count < Guards::Max) {
                    guards->word[guards->count] = uint16_t(word);
                    guards->value[guards->count] = out;
                    ++guards->count;
                } else {
                    guards->count = Guards::Max + 1;
                }
            }
            return true;
        }
        case OpBitcast:
            return count == 4 && Value(w[3], flat, out, guards, depth + 1);
        case OpLogicalNot:
            if (count != 4 || !Value(w[3], flat, a, guards, depth + 1)) {
                return false;
            }
            out = !a;
            return true;
        case OpLogicalAnd:
        case OpLogicalOr: {
            if (count != 5) {
                return false;
            }
            const bool have_a = Value(w[3], flat, a, guards, depth + 1);
            const bool have_b = Value(w[4], flat, b, guards, depth + 1);
            const bool absorbing = op == OpLogicalOr; // true decides an or, false an and
            if ((have_a && (a != 0) == absorbing) || (have_b && (b != 0) == absorbing)) {
                out = absorbing;
                return true;
            }
            if (!have_a || !have_b) {
                return false;
            }
            out = !absorbing; // neither side is the absorbing value
            return true;
        }
        case OpLogicalEqual:
        case OpLogicalNotEqual:
        case OpIEqual:
        case OpINotEqual:
            if (count != 5 || !Value(w[3], flat, a, guards, depth + 1) ||
                !Value(w[4], flat, b, guards, depth + 1)) {
                return false;
            }
            out = (op == OpLogicalEqual || op == OpIEqual) ? a == b : a != b;
            return true;
        default:
            break;
        }
        if (op >= OpFOrdEqual && op <= OpFUnordGreaterThanEqual) {
            if (count != 5 || !Value(w[3], flat, a, guards, depth + 1) ||
                !Value(w[4], flat, b, guards, depth + 1)) {
                return false;
            }
            float x, y;
            std::memcpy(&x, &a, 4);
            std::memcpy(&y, &b, 4);
            // Pairs of ordered (false with a NaN) and unordered (true with one): equal, not equal, <, >, <=, >=.
            if (std::isnan(x) || std::isnan(y)) {
                out = ((op - OpFOrdEqual) & 1) != 0;
                return true;
            }
            switch ((op - OpFOrdEqual) / 2) {
            case 0: out = x == y; break;
            case 1: out = x != y; break;
            case 2: out = x < y; break;
            case 3: out = x > y; break;
            case 4: out = x <= y; break;
            default: out = x >= y; break;
            }
            return true;
        }
        return false;
    }

    /// The blocks that run with `flat`, and the configuration.
    bool Run(std::span<const uint32_t> flat, Walk& walk) const {
        walk.live[0] = 1;
        uint32_t stack[MaxBlocks];
        uint32_t depth = 0;
        stack[depth++] = 0;
        while (depth) {
            const uint32_t at = stack[--depth];
            const Block& block = blocks[at];
            const auto go = [&](uint32_t target) {
                // (Each block is pushed once, when it first comes alive: the stack cannot overflow.)
                if (target != None && !walk.live[target]) {
                    walk.live[target] = 1;
                    stack[depth++] = target;
                }
            };
            if (block.all) {
                for (const uint32_t target : block.more) {
                    go(target);
                }
            } else if (block.conditional) {
                uint32_t value = 0;
                const uint8_t taken = Value(block.condition, flat, value, walk.guards) ? (value ? 1 : 2) : 3;
                walk.taken[at] = taken;
                if (taken & 1) {
                    go(block.targets[0]);
                }
                if (taken & 2) {
                    go(block.targets[1]);
                }
            } else {
                go(block.targets[0]);
            }
        }
        uint32_t shift = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (walk.taken[i]) {
                walk.config |= uint64_t(walk.taken[i]) << shift;
                shift += 2;
            }
        }
        return true;
    }

    /// The backward slice through the blocks of a walk.
    struct Slice {
        const PositionCode& p;
        const Walk& walk;
        std::vector<uint16_t> words{};
        std::vector<uint32_t> work{};
        std::vector<bool> seen{}, variable_seen{};
        const char* failed = nullptr;

        const char* Run() {
            seen.assign(p.def.size(), false);
            variable_seen.assign(p.def.size(), false);
            for (size_t i = 0; i < p.blocks.size(); ++i) {
                if (walk.live[i] && p.blocks[i].condition != None) {
                    work.push_back(p.blocks[i].condition);
                }
            }
            Variable(p.position_var);
            while (!work.empty() && !failed) {
                const uint32_t id = work.back();
                work.pop_back();
                if (id >= p.def.size() || seen[id]) {
                    continue;
                }
                seen[id] = true;
                if (p.def[id] && p.def_block[id] != None && walk.live[p.def_block[id]]) {
                    Visit(&p.code[p.def[id]], p.def_block[id]);
                }
            }
            std::sort(words.begin(), words.end());
            words.erase(std::unique(words.begin(), words.end()), words.end());
            return failed;
        }
        /// A read of variable `var`: it depends on every store to it in a block that runs.
        void Variable(uint32_t var) {
            if (variable_seen[var]) {
                return;
            }
            variable_seen[var] = true;
            for (const Store& store : p.stores[var]) {
                if (walk.live[store.block]) {
                    work.push_back(store.value);
                }
            }
        }
        /// Whether the branch from block `from` to block `to` is taken in this walk.
        bool Edge(uint32_t from, uint32_t to) const {
            if (!walk.live[from]) {
                return false;
            }
            const Block& block = p.blocks[from];
            if (block.all) {
                return std::find(block.more.begin(), block.more.end(), to) != block.more.end();
            }
            if (block.conditional) {
                return ((walk.taken[from] & 1) && block.targets[0] == to) ||
                       ((walk.taken[from] & 2) && block.targets[1] == to);
            }
            return block.targets[0] == to;
        }
        /// A value read through `pointer`.
        void Read(uint32_t pointer) {
            const uint32_t root = p.Root(pointer);
            if (root == None) {
                failed = "a read through a computed pointer";
                return;
            }
            // The indices of the chain(s) between the variable and the pointer, in order.
            uint32_t indices[16];
            uint32_t num = 0;
            for (uint32_t at = pointer; at != root;) {
                const uint32_t* w = &p.code[p.def[at]];
                const uint32_t count = w[0] >> 16;
                if (count < 4 || num + (count - 4) > 16) {
                    failed = "a long access chain";
                    return;
                }
                // (The walk goes from the pointer towards its variable: the indices seen so far go behind.)
                for (uint32_t i = num; i-- > 0;) {
                    indices[i + (count - 4)] = indices[i];
                }
                for (uint32_t i = 4; i < count; ++i) {
                    indices[i - 4] = w[i];
                }
                num += count - 4;
                at = w[3];
            }
            const auto constant = [this](uint32_t id) {
                return id < p.constant.size() ? p.constant[id] : uint32_t(None);
            };
            const uint32_t cls = p.storage[root];
            if (root == p.flat_var) {
                // srt_flatbuf { uint data[]; }: member 0, then the word.
                if (num != 2 || constant(indices[1]) == None) {
                    failed = "a flat word at a computed index";
                    return;
                }
                if (constant(indices[1]) > 0xffffu) {
                    failed = "a flat word index out of range";
                    return;
                }
                words.push_back(uint16_t(constant(indices[1])));
                return;
            }
            if (cls == StoragePushConstant) {
                if (num == 0 || constant(indices[0]) == None) {
                    failed = "the push block read whole";
                    return;
                }
                const uint32_t member = constant(indices[0]);
                if (member >= p.ud_member && member < p.ud_member + p.ud_members) {
                    const uint32_t first = (member - p.ud_member) * 4;
                    if (num >= 2 && constant(indices[1]) != None && constant(indices[1]) < 4) {
                        words.push_back(uint16_t(first + constant(indices[1])));
                    } else {
                        for (uint32_t i = 0; i < 4; ++i) {
                            words.push_back(uint16_t(first + i));
                        }
                    }
                }
                // (The other members follow from the registers and the buffers' addresses: the viewport, the
                // buffers' offsets.)
                return;
            }
            switch (cls) {
            case StorageUniformConstant:
                failed = "a texture read";
                return;
            case StorageInput:
                return; // the vertex and instance numbers: the draw's own
            case StorageUniform:
            case StorageBuffer:
                // A guest buffer (vertex stream, constants): its descriptor and memory are the caller's part;
                // where it is read may depend on flat words.
                for (uint32_t i = 0; i < num; ++i) {
                    work.push_back(indices[i]);
                }
                return;
            case StorageOutput:
            case StoragePrivate:
            case StorageFunction:
                for (uint32_t i = 0; i < num; ++i) {
                    work.push_back(indices[i]);
                }
                Variable(root);
                return;
            default:
                failed = "a read from another storage class";
                return;
            }
        }
        void Visit(const uint32_t* w, uint32_t block) {
            const uint32_t count = w[0] >> 16, op = w[0] & 0xffffu;
            switch (op) {
            case OpLoad:
            case OpAtomicLoad:
                Read(w[3]);
                break;
            case OpAccessChain:
            case OpInBoundsAccessChain:
                // A pointer used as a value (never by this emitter): not followed.
                failed = "a pointer as a value";
                break;
            case OpPhi:
                // Only what comes in over a branch that is taken.
                for (uint32_t i = 3; i + 1 < count; i += 2) {
                    bool taken = true; // a parent that is no label of this function: the safe side
                    for (uint32_t from = 0; from < p.blocks.size(); ++from) {
                        if (p.blocks[from].label == w[i + 1]) {
                            taken = Edge(from, block);
                            break;
                        }
                    }
                    if (taken) {
                        work.push_back(w[i]);
                    }
                }
                break;
            case OpCompositeExtract:
                work.push_back(w[3]);
                break;
            case OpCompositeInsert:
            case OpVectorShuffle:
                work.push_back(w[3]);
                work.push_back(w[4]);
                break;
            case OpExtInst:
                for (uint32_t i = 5; i < count; ++i) {
                    work.push_back(w[i]);
                }
                break;
            default:
                // Every operand an id (a literal among them only adds an id that is not needed).
                for (uint32_t i = 3; i < count; ++i) {
                    work.push_back(w[i]);
                }
                break;
            }
        }
    };

    std::vector<uint32_t> code;
    uint32_t ud_member = 0, ud_members = 0;
    uint32_t flat_var = 0, position_var = 0;
    const char* why = nullptr;
    std::vector<uint32_t> def;       ///< id -> its instruction in the function (0: defined outside)
    std::vector<uint32_t> def_block; ///< id -> the block of that instruction
    std::vector<uint32_t> constant;  ///< id -> the value of a 32-bit constant
    std::vector<uint8_t> boolean;    ///< id -> a boolean constant (2: none)
    std::vector<uint32_t> storage;   ///< id -> a variable's storage class
    std::vector<std::vector<Store>> stores; ///< variable -> the values stored into it
    std::vector<Block> blocks;
};

} // namespace Vulkan::Motion
