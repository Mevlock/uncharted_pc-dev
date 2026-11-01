// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// bbport (from BilalReloaded's port 0.8.0, U3 chapter 2 device lost): Naughty Dog's effect and
// crowd skinning compute shaders (cs 8f067540, 7d7e7206, 0c936dd6, all three games) run, per
// vertex, a loop whose trip count is read from buffer 0 (u32 count, u32 offset per vertex):
//     entry = buffer0[vertex]; for (i = 0; i < entry.count; ++i) { ...buffer4[entry.offset + i]... }
// A count that is not a small influence count (stale or garbage data) runs the loop billions of
// times: the GPU hangs and Windows resets it ("Device lost"). The loop's bound is clamped to
// MaxEffectLoopCount, far above any real count. BB_EFFECT_LOOP_CLAMP=0: off (the shaders as
// before; their cache names change with it, see vk_pipeline_serialization.cpp BinaryName).
constexpr u32 MaxEffectLoopCount = 4096;

bool EffectLoopClamp(u64 pgm_hash) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_EFFECT_LOOP_CLAMP");
        return !(env && env[0] == '0');
    }();
    return enabled &&
           (pgm_hash == 0x8f067540 || pgm_hash == 0x7d7e7206 || pgm_hash == 0x0c936dd6);
}

void EffectLoopClampPass(IR::Program& program) {
    const u64 hash = program.info.pgm_hash;
    if (!EffectLoopClamp(hash)) {
        return;
    }
    u32 patched = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::LoadBufferU32x2) {
                continue;
            }
            const IR::Value handle = inst.Arg(0);
            if (!handle.IsImmediate() || handle.U32() != 0) {
                continue;
            }
            const std::vector<IR::Use> loads(inst.Uses().begin(), inst.Uses().end());
            for (const IR::Use& load_use : loads) {
                IR::Inst* const count = load_use.user;
                if (count->GetOpcode() != IR::Opcode::CompositeExtractU32x2 ||
                    !count->Arg(1).IsImmediate() || count->Arg(1).U32() != 0) {
                    continue;
                }
                // Only the loop bound: `i < count`.
                std::vector<IR::Use> bounds;
                for (const IR::Use& use : count->Uses()) {
                    if (use.user->GetOpcode() == IR::Opcode::ULessThan32 && use.operand == 1) {
                        bounds.push_back(use);
                    }
                }
                if (bounds.empty()) {
                    continue;
                }
                IR::Block* const parent = count->GetParent();
                IR::IREmitter ir{*parent,
                                 std::next(IR::Block::InstructionList::s_iterator_to(*count))};
                const IR::U32 bound = ir.IMin(IR::U32{count}, ir.Imm32(MaxEffectLoopCount), false);
                for (const IR::Use& use : bounds) {
                    use.user->SetArg(use.operand, bound);
                }
                ++patched;
            }
        }
    }
    static u32 reports = 0;
    if (reports < 8) {
        ++reports;
        std::printf("GPU: effect shader %#llx: loop bound clamped to %u in %u place(s) "
                    "(BB_EFFECT_LOOP_CLAMP)\n",
                    static_cast<unsigned long long>(hash), MaxEffectLoopCount, patched);
        std::fflush(stdout);
    }
}

} // namespace Shader::Optimization
