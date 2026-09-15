//===-- MoeELFStreamer.cpp - Moe ELF object output ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// JUMP/LOAD/STORE carry a trailing operand word that is not part of their
// 16-bit `Inst` encoding, and that word must land on the next word-aligned
// address after the 16-bit opcode halfword (see 'Addressing mode' in the
// Encoding chapter). Whether that costs two bytes of padding depends on the
// opcode halfword's own address, which is exactly the thing an MCCodeEmitter
// cannot know: encodeInstruction is handed a byte buffer, not a position, and
// the position genuinely isn't decided yet - an earlier `.align` in the same
// section is an MCAlignFragment whose size is only resolved during layout.
//
// MoeMCCodeEmitter used to approximate the position with a running byte
// counter reset at each function start. That is correct for compiler output,
// where a function body is an uninterrupted run of instructions, and wrong
// for anything else: any .byte/.long/.asciz, any .align, any .org, any
// section switch desynchronises the counter from reality and every
// subsequent trailing word in that section is misplaced. Confirmed
// concretely before this file existed - `.byte 1,2` followed by `load.w foo
// -> gp0` put the trailing word at offset 6 instead of 4, and dragged every
// later label out of alignment with it. Hand-written supervisor assembly
// (head.S, entry.S, and the paging test's in-line page tables) mixes code
// and data in exactly that way as a matter of course, so the approximation
// had to go rather than be patched.
//
// So the split is: MoeMCCodeEmitter emits the 16-bit opcode halfword and
// nothing else, and the trailing word is emitted here, as ordinary streamer
// output - an alignment directive followed by a 4-byte value. MC's own
// layout then computes the padding, correctly, in every one of the cases
// above, and the relocation for a symbolic trailing operand comes from
// emitValue's normal fixup path rather than a hand-built MCFixup.
//
// One consequence worth stating plainly: `llvm-mc -show-encoding` now prints
// only the opcode halfword for these instructions, because MCAsmStreamer
// asks the code emitter directly and never reaches this streamer. The
// trailing word is still there in the object file. Check encodings against
// the linked binary (llvm-objdump, or moe-emu --trace's disassembly of the
// actual bytes), which is the stronger check anyway - it is the artifact
// that runs.
//
//===----------------------------------------------------------------------===//

#include "MoeELFStreamer.h"
#include "MoeMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

// Which operand of a trailing-word instruction holds the addressing info,
// and in which of the two shapes: a single symbol-or-constant operand
// (Absolute mode - moeaddr/jmptarget/calltarget), or a (base register,
// offset) pair (Register-indirect mode - moemem).
struct TrailingOperand {
  int SymOperand = -1;
  int MemBaseOperand = -1;

  bool isAbsolute() const { return SymOperand >= 0; }
  bool exists() const { return SymOperand >= 0 || MemBaseOperand >= 0; }
};

TrailingOperand getTrailingOperand(const MCInst &MI) {
  TrailingOperand T;
  switch (MI.getOpcode()) {
  case Moe::JMP:
  case Moe::JCC:
  case Moe::JMPabs:
    T.SymOperand = 0;
    break;
  case Moe::LOADabs:
  case Moe::STOREabs:
  case Moe::LOADabs_A:
  case Moe::STOREabs_A:
    T.SymOperand = 1;
    break;
  case Moe::LOADrr:
  case Moe::STORErr:
  case Moe::LOADrr_A:
  case Moe::STORErr_A:
  // STORErr_B/H have the same (ins GPR:$reg, moemem:$addr) shape as
  // STORErr - no tied operand - so the base register is still operand 1.
  case Moe::STORErr_B:
  case Moe::STORErr_H:
    T.MemBaseOperand = 1;
    break;
  // LOADrr_B/H additionally have a tied $oldval input ((ins GPR:$oldval,
  // moemem:$addr)) ahead of the address, which remains a distinct MCInst
  // operand despite the tie (ties only constrain register allocation, they
  // don't collapse the operand at the MC level) - shifting the base
  // register to operand 2, not 1.
  case Moe::LOADrr_B:
  case Moe::LOADrr_H:
    T.MemBaseOperand = 2;
    break;
  default:
    break;
  }
  return T;
}

} // end anonymous namespace

void MoeELFStreamer::emitInstruction(const MCInst &Inst,
                                      const MCSubtargetInfo &STI) {
  // The 16-bit opcode halfword, through the ordinary code-emitter path.
  MCELFStreamer::emitInstruction(Inst, STI);

  TrailingOperand T = getTrailingOperand(Inst);
  if (!T.exists())
    return;

  // Fill with zeroes rather than any instruction pattern: this padding sits
  // between an instruction and its own operand word and is never reached by
  // control flow, which is the same reasoning MoeAsmBackend::writeNopData
  // already records for why Moe needs no NOP opcode (it has none - all 16
  // opcodes are real instructions).
  emitValueToAlignment(Align(4), /*Fill=*/0, /*FillLen=*/1,
                       /*MaxBytesToEmit=*/0);

  if (T.isAbsolute()) {
    const MCOperand &MO = Inst.getOperand(T.SymOperand);
    // Both shapes are legitimate input. A symbol (or any relocatable
    // expression) becomes an R_MOE_32 via emitValue's own fixup path; a bare
    // constant is just the address, which is how assembly reaches a fixed
    // location with no symbol attached to it - the trap vector at 0, the
    // reset vector, an MMIO register. The constant case used to abort the
    // assembler on an assertion rather than assemble or diagnose.
    if (MO.isImm())
      emitIntValue(static_cast<uint32_t>(MO.getImm()), 4);
    else if (MO.isExpr())
      emitValue(MO.getExpr(), 4, Inst.getLoc());
    else
      llvm_unreachable("Absolute-mode trailing operand must be imm or expr");
    return;
  }

  const MCOperand &Base = Inst.getOperand(T.MemBaseOperand);
  const MCOperand &Offset = Inst.getOperand(T.MemBaseOperand + 1);
  assert(Base.isReg() && "Register-indirect trailing operand needs a base reg");
  uint32_t BaseEnc = getContext().getRegisterInfo()->getEncodingValue(
      Base.getReg());

  // Matches emulator/src/cpu.rs's resolve_trailing_operand exactly: base
  // register select in bits 3-0, 28-bit signed offset in bits 31-4.
  if (Offset.isImm()) {
    emitIntValue((static_cast<uint32_t>(Offset.getImm()) << 4) | (BaseEnc & 0xF),
                 4);
    return;
  }

  // A symbolic offset (`load.w [gp1+SOME_CONSTANT] -> gp0`, with the constant
  // .set elsewhere) packs the same way, just as an expression MC folds during
  // layout. It has to fold to an absolute value: there is no relocation that
  // writes a shifted field, so a genuinely relocatable offset is an error
  // here rather than something to emit and hope about - and MC reports it as
  // one when the expression fails to evaluate.
  assert(Offset.isExpr() && "Register-indirect offset must be imm or expr");
  MCContext &Ctx = getContext();
  const MCExpr *Shifted = MCBinaryExpr::createShl(
      Offset.getExpr(), MCConstantExpr::create(4, Ctx), Ctx);
  const MCExpr *Packed = MCBinaryExpr::createOr(
      Shifted, MCConstantExpr::create(BaseEnc & 0xF, Ctx), Ctx);
  emitValue(Packed, 4, Inst.getLoc());
}

MCStreamer *llvm::createMoeELFStreamer(const Triple &T, MCContext &Ctx,
                                        std::unique_ptr<MCAsmBackend> &&MAB,
                                        std::unique_ptr<MCObjectWriter> &&MOW,
                                        std::unique_ptr<MCCodeEmitter> &&MCE) {
  return new MoeELFStreamer(Ctx, std::move(MAB), std::move(MOW),
                            std::move(MCE));
}
