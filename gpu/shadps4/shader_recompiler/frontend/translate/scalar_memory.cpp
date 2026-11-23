// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_exposure_fix.h"
#include "bbport_focus_fix.h"
#include "shader_recompiler/frontend/translate/translate.h"

namespace Shader::Gcn {

static constexpr u32 SQ_SRC_LITERAL = 0xFF;

void Translator::EmitScalarMemory(const GcnInst& inst) {
    switch (inst.opcode) {
        // SMRD
    case Opcode::S_LOAD_DWORD:
        return S_LOAD_DWORD(1, inst);
    case Opcode::S_LOAD_DWORDX2:
        return S_LOAD_DWORD(2, inst);
    case Opcode::S_LOAD_DWORDX4:
        return S_LOAD_DWORD(4, inst);
    case Opcode::S_LOAD_DWORDX8:
        return S_LOAD_DWORD(8, inst);
    case Opcode::S_LOAD_DWORDX16:
        return S_LOAD_DWORD(16, inst);
    case Opcode::S_BUFFER_LOAD_DWORD:
        return S_BUFFER_LOAD_DWORD(1, inst);
    case Opcode::S_BUFFER_LOAD_DWORDX2:
        return S_BUFFER_LOAD_DWORD(2, inst);
    case Opcode::S_BUFFER_LOAD_DWORDX4:
        return S_BUFFER_LOAD_DWORD(4, inst);
    case Opcode::S_BUFFER_LOAD_DWORDX8:
        return S_BUFFER_LOAD_DWORD(8, inst);
    case Opcode::S_BUFFER_LOAD_DWORDX16:
        return S_BUFFER_LOAD_DWORD(16, inst);
    case Opcode::S_MEMTIME:
        return S_MEMTIME(inst);
    default:
        LogMissingOpcode(inst);
    }
}

// SMRD

void Translator::S_LOAD_DWORD(int num_dwords, const GcnInst& inst) {
    const auto& smrd = inst.control.smrd;
    const IR::ScalarReg sbase{inst.src[0].code * 2};
    const IR::U32 dword_offset = [&] -> IR::U32 {
        if (smrd.imm) {
            return ir.Imm32(smrd.offset);
        }
        if (smrd.offset == SQ_SRC_LITERAL) {
            return ir.Imm32(inst.src[1].code);
        }
        return ir.ShiftRightLogical(ir.GetScalarReg(IR::ScalarReg(smrd.offset)), ir.Imm32(2));
    }();
    const IR::Value base =
        ir.CompositeConstruct(ir.GetScalarReg(sbase), ir.GetScalarReg(sbase + 1));
    IR::ScalarReg dst_reg{inst.dst[0].code};
    for (u32 i = 0; i < num_dwords; i++) {
        IR::U32 index = ir.IAdd(dword_offset, ir.Imm32(i));
        ir.SetScalarReg(dst_reg + i, ir.ReadConst(base, index));
    }
}

void Translator::S_BUFFER_LOAD_DWORD(int num_dwords, const GcnInst& inst) {
    const auto& smrd = inst.control.smrd;
    const IR::ScalarReg sbase{inst.src[0].code * 2};
    const IR::U32 dword_offset = [&] -> IR::U32 {
        if (smrd.imm) {
            return ir.Imm32(smrd.offset);
        }
        if (smrd.offset == SQ_SRC_LITERAL) {
            return ir.Imm32(inst.src[1].code);
        }
        return ir.ShiftRightLogical(ir.GetScalarReg(IR::ScalarReg(smrd.offset)), ir.Imm32(2));
    }();

    IR::BufferInstInfo buffer_info{};
    buffer_info.pc.Assign(pc);

    const IR::Value vsharp =
        ir.CompositeConstruct(ir.GetScalarReg(sbase), ir.GetScalarReg(sbase + 1),
                              ir.GetScalarReg(sbase + 2), ir.GetScalarReg(sbase + 3));
    IR::ScalarReg dst_reg{inst.dst[0].code};
    for (u32 i = 0; i < num_dwords; i++) {
        const IR::U32 index = ir.IAdd(dword_offset, ir.Imm32(i));
        IR::U32 value = ir.ReadConstBuffer(vsharp, index, buffer_info);
        // bbport: the autofocus's screen centre, for the render size (bbport_focus_fix.h).
        if (smrd.imm && smrd.offset + i < 2 && BbFocusFix::FocusCentreShader(info.pgm_hash)) {
            const u32 axis = smrd.offset + i;
            const IR::F32 inverse_size = ir.BitCast<IR::F32>(
                ir.ReadConstBuffer(vsharp, ir.Imm32(6 + axis), buffer_info));
            const IR::F32 centre = ir.FPDiv(
                ir.BitCast<IR::F32>(value),
                ir.FPMul(inverse_size, ir.Imm32(axis == 0 ? 1920.0f : 1080.0f)));
            value = ir.BitCast<IR::U32>(centre);
        }
        // bbport: the exposure metering at render sizes above 2048 wide (bbport_exposure_fix.h).
        if (smrd.imm && BbExposureFix::ExposureShader(info.pgm_hash)) {
            const u32 at = smrd.offset + i;
            const bool rows = static_cast<u32>(info.pgm_hash) == 0xaec539f0u;
            const IR::F32 widest = ir.Imm32(1.0f / 1024.0f);
            if (rows && at == 8) {
                // The column step: the 1024 taps span the whole row.
                value = ir.BitCast<IR::U32>(IR::F32{ir.FPMax(ir.BitCast<IR::F32>(value), widest)});
            } else if (!rows && at == 10) {
                // The divisor: the samples really summed (1024 a row instead of the half width).
                // Guarded: the factor only applies for a positive step. A step of 0 (the original shader never
                // reads it) would give infinity, and an infinity or NaN in the exposure history would stay
                // there; zero, negative and NaN steps leave the divisor as it was (factor 1). The factor is 1
                // for the shader's own sizes and about 2 at 8K wide, so it is capped far above that.
                const IR::F32 step = ir.BitCast<IR::F32>(ir.ReadConstBuffer(vsharp, ir.Imm32(8), buffer_info));
                const IR::F32 capped = IR::F32{ir.FPMin(
                    IR::F32{ir.FPMax(ir.Imm32(1.0f), IR::F32{ir.FPDiv(widest, step)})}, ir.Imm32(64.0f))};
                const IR::F32 more{ir.Select(ir.FPGreaterThan(step, ir.Imm32(0.0f)), capped, ir.Imm32(1.0f))};
                value = ir.BitCast<IR::U32>(IR::F32{ir.FPMul(ir.BitCast<IR::F32>(value), more)});
            }
        }
        ir.SetScalarReg(dst_reg + i, value);
    }
}

void Translator::S_MEMTIME(const GcnInst& inst) {
    SetDst64(inst.dst[0], ir.Memtime());
}

} // namespace Shader::Gcn
