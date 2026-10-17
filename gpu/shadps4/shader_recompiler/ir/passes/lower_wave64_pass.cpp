// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsDivergentCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    const IR::Inst* const condition_inst = condition.Inst();
    return IR::BreadthFirstSearch(condition_inst,
                                  [](const IR::Inst* inst) -> std::optional<bool> {
                                      switch (inst->GetOpcode()) {
                                      case IR::Opcode::LaneId:
                                          return true;
                                      case IR::Opcode::GetAttributeU32:
                                          if (inst->Arg(0).Attribute() ==
                                              IR::Attribute::LocalInvocationId) {
                                              return true;
                                          }
                                          break;
                                      default:
                                          break;
                                      }
                                      return std::nullopt;
                                  })
        .value_or(false);
}

static std::vector<IR::Block*> FindUniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0 && loops.empty()) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

// bbport: GCN bounds-checks LDS accesses against the workgroup's allocation (LDS_SIZE): reads
// beyond it return 0 and writes are dropped. Vulkan shared memory has no such check, so a shader
// that relies on it (U3's 1024-thread cloth solver sums lds[i] + lds[i + 512] with only 512 dwords
// allocated) reads the wave64 ballot scratch and whatever lies past the array instead of zeros.
// Out-of-range accesses are sent to a sink dword after the allocation, and loads and atomics
// from out of range yield 0. BB_LDS_BOUNDS=0 turns this off.
void SharedMemoryBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_LDS_BOUNDS");
        return !env || std::atoi(env) != 0;
    }();
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    if (!enabled || program.info.hw_stage != HwStage::Compute || size < sizeof(u64)) {
        return;
    }
    const u32 sink = Common::AlignUp(size, sizeof(u64));
    bool used_sink = false;
    for (IR::Block* const block : program.blocks) {
        for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
            IR::Inst& inst = *it;
            u32 bytes = 0;
            bool returns_value = true;
            switch (inst.GetOpcode()) {
            case IR::Opcode::LoadSharedU16:
                bytes = 2;
                break;
            case IR::Opcode::WriteSharedU16:
                bytes = 2;
                returns_value = false;
                break;
            case IR::Opcode::LoadSharedU32:
            case IR::Opcode::SharedAtomicIAdd32:
            case IR::Opcode::SharedAtomicISub32:
            case IR::Opcode::SharedAtomicSMin32:
            case IR::Opcode::SharedAtomicUMin32:
            case IR::Opcode::SharedAtomicSMax32:
            case IR::Opcode::SharedAtomicUMax32:
            case IR::Opcode::SharedAtomicInc32:
            case IR::Opcode::SharedAtomicDec32:
            case IR::Opcode::SharedAtomicAnd32:
            case IR::Opcode::SharedAtomicOr32:
            case IR::Opcode::SharedAtomicXor32:
            case IR::Opcode::SharedAtomicCmpSwap32:
                bytes = 4;
                break;
            case IR::Opcode::WriteSharedU32:
                bytes = 4;
                returns_value = false;
                break;
            case IR::Opcode::LoadSharedU64:
            case IR::Opcode::SharedAtomicIAdd64:
            case IR::Opcode::SharedAtomicISub64:
            case IR::Opcode::SharedAtomicSMin64:
            case IR::Opcode::SharedAtomicUMin64:
            case IR::Opcode::SharedAtomicSMax64:
            case IR::Opcode::SharedAtomicUMax64:
            case IR::Opcode::SharedAtomicInc64:
            case IR::Opcode::SharedAtomicDec64:
            case IR::Opcode::SharedAtomicAnd64:
            case IR::Opcode::SharedAtomicOr64:
            case IR::Opcode::SharedAtomicXor64:
            case IR::Opcode::SharedAtomicCmpSwap64:
                bytes = 8;
                break;
            case IR::Opcode::WriteSharedU64:
                bytes = 8;
                returns_value = false;
                break;
            default:
                continue;
            }
            if (inst.Flags<bool>()) { // GDS
                continue;
            }
            const IR::Value address = inst.Arg(0);
            if (address.IsImmediate() && address.U32() + bytes <= size) {
                continue;
            }
            IR::IREmitter ir{*block, it};
            const IR::U1 in_range =
                ir.ILessThanEqual(IR::U32{address}, ir.Imm32(size - bytes), false);
            inst.SetArg(0, ir.Select(in_range, address, ir.Imm32(sink)));
            used_sink = true;
            if (!returns_value || bytes == 2) {
                continue;
            }
            // The zero for out-of-range reads, after the access.
            IR::IREmitter after{*block, std::next(it)};
            const IR::Value zero = bytes == 8 ? IR::Value{ir.Imm64(u64{0})} : IR::Value{ir.Imm32(0)};
            const IR::Value result = after.Select(in_range, IR::Value{&inst}, zero);
            const std::vector<IR::Use> uses(inst.Uses().begin(), inst.Uses().end());
            for (const IR::Use& use : uses) {
                if (use.user != result.Inst()) {
                    use.user->SetArg(use.operand, result);
                }
            }
            it = IR::Block::InstructionList::s_iterator_to(*result.Inst());
        }
    }
    if (used_sink) {
        program.info.shared_memory_scratch_size = sink + sizeof(u64) - size;
    }
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        return;
    }

    std::vector<IR::Inst*> worklist;
    const auto uniform_blocks = FindUniformBlocks(program);
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                LOG_WARNING(Render_Recompiler, "{} instruction in non uniform control flow",
                            inst.GetOpcode());
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::Ballot) {
                const auto is_unpack = [](const IR::Use& use) {
                    return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
                };
                if (std::ranges::any_of(inst.Uses(), is_unpack)) {
                    push_worklist(inst);
                }
            } else if (inst.GetOpcode() == IR::Opcode::GetAttributeU32 &&
                       inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
                worklist.push_back(&inst);
            }
        }
    }
    if (worklist.empty()) {
        return;
    }

    // After any scratch an earlier pass reserved (SharedMemoryBoundsPass's sink).
    const u32 scratch_base =
        Common::AlignUp(runtime_info.hw.cs.shared_memory_size +
                            program.info.shared_memory_scratch_size,
                        sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            const IR::U64 mask = IR::U64{ir.LoadShared(64, false, offset)};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            inst->ReplaceUsesWithAndRemove(
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(1).U32()));
        }
    }
}

} // namespace Shader::Optimization
