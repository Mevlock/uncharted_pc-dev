// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (object motion, still draws): what a vertex shader's position follows from, read from its SPIR-V.
//
// Uncharted's vertex shaders take their constants (object matrix, view, projection, wind) from the flattened user
// data, next to pointers and descriptors that change every frame without changing anything drawn. Which of its
// words reach the position is a property of the shader's code: PositionInputs slices the module backwards from
// every store to the Position built-in. The slice is flow-insensitive and takes the safe side everywhere: every
// branch condition and switch selector of the function is an input of the position, a variable read depends on
// every store to that variable, and anything it cannot follow (a function call, a read through a computed pointer
// or a device address, a texture, a flat word at a computed index) makes the result invalid, so the draw is never
// taken for still. No Vulkan or shader recompiler types: the regression test includes this header alone.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Vulkan::Motion {

struct PositionInputs {
    bool valid = false;          ///< the module was understood: `words` is all of the flat data the position reads
    std::vector<uint16_t> words; ///< flat word indices, ascending (user-data registers read from the push block
                                 ///< count as the flat words they are copied to, 0..15)
    const char* why = "";        ///< valid == false: the reason, for the log

    /// `flat_name`: the flat buffer's variable name; `ud_member`, `ud_members`: the push block's members holding
    /// the user-data registers (four words each).
    static PositionInputs FromSpirv(std::span<const uint32_t> code, std::string_view flat_name, uint32_t ud_member,
                                    uint32_t ud_members) {
        return Slicer{code, flat_name, ud_member, ud_members}.Run();
    }

private:
    class Slicer {
    public:
        Slicer(std::span<const uint32_t> code, std::string_view flat_name, uint32_t ud_member, uint32_t ud_members)
            : code{code}, flat_name{flat_name}, ud_member{ud_member}, ud_members{ud_members} {}

        PositionInputs Run() {
            if (code.size() < 5 || code[0] != 0x07230203u) {
                return Fail("not SPIR-V");
            }
            const uint32_t bound = code[3];
            if (bound == 0 || bound > (1u << 22)) {
                return Fail("id bound");
            }
            def.assign(bound, 0);
            constant.assign(bound, NoConstant);
            storage.assign(bound, NoStorage);
            stores.assign(bound, {});
            bool in_function = false;
            uint32_t functions = 0;
            std::vector<uint32_t> seeds;
            // Pass 1: names, decorations, constants, variables, definitions, stores, conditions.
            for (size_t at = 5; at < code.size();) {
                const uint32_t count = code[at] >> 16, op = code[at] & 0xffffu;
                if (count == 0 || at + count > code.size()) {
                    return Fail("instruction length");
                }
                const uint32_t* w = &code[at];
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
                        return Fail("the position is a block member");
                    }
                    break;
                case OpConstant:
                    if (count >= 4 && w[2] < bound) {
                        constant[w[2]] = w[3];
                    }
                    break;
                case OpVariable:
                    if (count >= 4 && w[2] < bound) {
                        storage[w[2]] = w[3];
                        if (in_function) {
                            def[w[2]] = uint32_t(at);
                        }
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
                    return Fail("a call or memory copy");
                case OpStore:
                    if (in_function && count >= 3) {
                        const uint32_t root = Root(w[1]);
                        if (root == Unknown) {
                            // A store through a pointer that is not a variable's (the motion twin's stores by
                            // device address): it cannot change a variable the position reads.
                            break;
                        }
                        stores[root].push_back(w[2]);
                    }
                    break;
                case OpBranchConditional:
                case OpSwitch:
                    if (in_function && count >= 2) {
                        seeds.push_back(w[1]);
                    }
                    break;
                default:
                    if (in_function && HasResult(op) && count >= 3 && w[2] < bound) {
                        def[w[2]] = uint32_t(at);
                    }
                    break;
                }
                at += count;
            }
            if (functions != 1) {
                return Fail("more than one function");
            }
            if (!position_var) {
                return Fail("no position output");
            }
            if (stores[position_var].empty()) {
                return Fail("the position is not stored");
            }
            // Pass 2: everything the stored positions and the branch conditions are computed from.
            seen.assign(bound, false);
            variable_seen.assign(bound, false);
            work = std::move(seeds);
            Variable(position_var);
            while (!work.empty() && !failed) {
                const uint32_t id = work.back();
                work.pop_back();
                if (id >= bound || seen[id]) {
                    continue;
                }
                seen[id] = true;
                if (def[id]) {
                    Visit(&code[def[id]]);
                }
            }
            if (failed) {
                return Fail(failed);
            }
            PositionInputs result;
            result.valid = true;
            std::sort(words.begin(), words.end());
            words.erase(std::unique(words.begin(), words.end()), words.end());
            result.words = std::move(words);
            return result;
        }

    private:
        enum : uint32_t {
            OpName = 5, OpExtInst = 12, OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56, OpFunctionCall = 57,
            OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65,
            OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79,
            OpCompositeExtract = 81, OpCompositeInsert = 82, OpAtomicLoad = 227, OpPhi = 245, OpBranchConditional = 250,
            OpSwitch = 251,
            DecorationBuiltIn = 11, BuiltInPosition = 0,
            StorageUniformConstant = 0, StorageInput = 1, StorageUniform = 2, StorageOutput = 3, StoragePrivate = 6,
            StorageFunction = 7, StoragePushConstant = 9, StorageBuffer = 12,
            NoConstant = 0xffffffffu, NoStorage = 0xffffffffu, Unknown = 0xffffffffu,
        };
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
        PositionInputs Fail(const char* why) {
            PositionInputs result;
            result.why = why;
            return result;
        }
        bool IsChain(uint32_t id) const {
            if (id >= def.size() || !def[id]) {
                return false;
            }
            const uint32_t op = code[def[id]] & 0xffffu;
            return op == OpAccessChain || op == OpInBoundsAccessChain;
        }
        /// The variable a pointer leads into (through access chains), or Unknown.
        uint32_t Root(uint32_t pointer) const {
            for (int depth = 0; depth < 8 && pointer < def.size(); ++depth) {
                if (storage[pointer] != NoStorage) {
                    return pointer;
                }
                if (!IsChain(pointer)) {
                    return Unknown;
                }
                pointer = code[def[pointer] + 3];
            }
            return Unknown;
        }
        /// A read of variable `var`: flow-insensitive, it depends on every store to it.
        void Variable(uint32_t var) {
            if (variable_seen[var]) {
                return;
            }
            variable_seen[var] = true;
            for (const uint32_t value : stores[var]) {
                work.push_back(value);
            }
        }
        /// A value read through `pointer`.
        void Read(uint32_t pointer) {
            const uint32_t root = Root(pointer);
            if (root == Unknown) {
                failed = "a read through a computed pointer";
                return;
            }
            // The indices of the chain(s) between the pointer and its variable, outermost last.
            uint32_t indices[16];
            uint32_t num = 0;
            for (uint32_t p = pointer; p != root;) {
                const uint32_t* w = &code[def[p]];
                const uint32_t count = w[0] >> 16;
                if (num + (count - 4) > 16) {
                    failed = "a long access chain";
                    return;
                }
                // (Inner chains come first in this walk: their indices go behind the outer ones'.)
                for (uint32_t i = num; i-- > 0;) {
                    indices[i + (count - 4)] = indices[i];
                }
                for (uint32_t i = 4; i < count; ++i) {
                    indices[i - 4] = w[i];
                }
                num += count - 4;
                p = w[3];
            }
            const uint32_t cls = storage[root];
            if (root == flat_var) {
                // srt_flatbuf { uint data[]; }: member 0, then the word.
                if (num != 2 || constant[indices[1]] == NoConstant) {
                    failed = "a flat word at a computed index";
                    return;
                }
                if (constant[indices[1]] > 0xffffu) {
                    failed = "a flat word index out of range";
                    return;
                }
                words.push_back(uint16_t(constant[indices[1]]));
                return;
            }
            if (cls == StoragePushConstant) {
                if (num == 0 || constant[indices[0]] == NoConstant) {
                    failed = "the push block read whole";
                    return;
                }
                const uint32_t member = constant[indices[0]];
                if (member >= ud_member && member < ud_member + ud_members) {
                    const uint32_t first = (member - ud_member) * 4;
                    if (num >= 2 && constant[indices[1]] != NoConstant && constant[indices[1]] < 4) {
                        words.push_back(uint16_t(first + constant[indices[1]]));
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
        void Visit(const uint32_t* w) {
            const uint32_t count = w[0] >> 16, op = w[0] & 0xffffu;
            switch (op) {
            case OpLoad:
            case OpAtomicLoad:
                Read(w[3]);
                break;
            case OpVariable:
                break;
            case OpAccessChain:
            case OpInBoundsAccessChain:
                // A pointer used as a value (never by this emitter): not followed.
                failed = "a pointer as a value";
                break;
            case OpPhi:
                for (uint32_t i = 3; i + 1 < count; i += 2) {
                    work.push_back(w[i]);
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

        std::span<const uint32_t> code;
        std::string_view flat_name;
        uint32_t ud_member, ud_members;
        uint32_t flat_var = 0, position_var = 0;
        std::vector<uint32_t> def;      ///< id -> its instruction in the function (0: defined outside)
        std::vector<uint32_t> constant; ///< id -> the value of a 32-bit constant
        std::vector<uint32_t> storage;  ///< id -> a variable's storage class
        std::vector<std::vector<uint32_t>> stores; ///< variable -> the values stored into it
        std::vector<bool> seen, variable_seen;
        std::vector<uint32_t> work;
        std::vector<uint16_t> words;
        const char* failed = nullptr;
    };
};

} // namespace Vulkan::Motion
