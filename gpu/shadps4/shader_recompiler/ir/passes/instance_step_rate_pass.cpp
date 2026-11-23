// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

/// bbport (exact variant key): the translator gives a vertex shader's input VGPRs v1 and v2 (the
/// instance ID over step rate 0 and 1) as placeholder attributes. After the SSA rewrite every read
/// of v1/v2 is a use of its placeholder, so the uses besides the prologue's own register write say
/// whether the program reads it (Info::vs_step_rate_reads, a property of the code alone, whatever
/// the step rate). Each placeholder then becomes what the translator gave before: the instance ID
/// divided by the step rate, or 0 for a step rate of 0, so the binary is unchanged.
void LowerInstanceStepRatePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    Info& info = program.info;
    if (info.sw_stage != SwStage::Vertex) {
        return;
    }
    for (IR::Block* const block : program.blocks) {
        for (auto it = block->Instructions().begin(); it != block->Instructions().end();) {
            IR::Inst& inst = *it;
            if (inst.GetOpcode() != IR::Opcode::GetAttributeU32) {
                ++it;
                continue;
            }
            const IR::Attribute attr = inst.Arg(0).Attribute();
            if (attr != IR::Attribute::InstanceIdStepRate0 &&
                attr != IR::Attribute::InstanceIdStepRate1) {
                ++it;
                continue;
            }
            const u32 k = attr == IR::Attribute::InstanceIdStepRate0 ? 0 : 1;
            const IR::VectorReg reg = k == 0 ? IR::VectorReg::V1 : IR::VectorReg::V2;
            bool read = false;
            for (const IR::Use& use : inst.Uses()) {
                const IR::Inst* user = use.user;
                const bool prologue_write = user->GetOpcode() == IR::Opcode::SetVectorRegister &&
                                            use.operand == 1 && user->Arg(0).VectorReg() == reg;
                read |= !prologue_write;
            }
            info.vs_step_rate_inputs |= u8(1u << k);
            if (read) {
                info.vs_step_rate_reads |= u8(1u << k);
            }
            const u32 step_rate =
                k == 0 ? runtime_info.sw.vs.step_rate_0 : runtime_info.sw.vs.step_rate_1;
            IR::IREmitter ir{*block, it};
            const IR::U32 value =
                step_rate != 0
                    ? ir.IDiv(ir.GetAttributeU32(IR::Attribute::InstanceId), ir.Imm32(step_rate))
                    : ir.Imm32(0);
            inst.ReplaceUsesWithAndRemove(value);
            it = block->Instructions().erase(it);
        }
    }
}

} // namespace Shader::Optimization
