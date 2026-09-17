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
#include "MoeFixupKinds.h"
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
  // A short form's operand is the halfword right after the opcode, with no
  // alignment and no padding, so the whole instruction is four bytes and can
  // be one aligned word - see 'Addressing mode' in the Encoding chapter.
  bool Short = false;
  // A PC-relative form: one symbolic operand, like the absolute forms, but the
  // word holds a displacement from itself in bits 31-4 and IA's base register
  // select in bits 3-0 rather than a whole address. See MoeFixupKinds.h. With
  // Short, the same thing in a halfword: twelve bits of displacement over the
  // same four of base register select.
  bool PcRel = false;

  bool isAbsolute() const { return SymOperand >= 0; }
  bool exists() const { return SymOperand >= 0 || MemBaseOperand >= 0; }
};

// IA's 'Register selection' value, per MoeRegisterInfo.td and 'Base register
// selection' in the Encoding chapter. Written out rather than looked up: the
// PC-relative forms have no base-register operand to read it from, because
// which register it is is the whole meaning of the addressing form.
constexpr uint32_t IARegSelect = 8;

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
  // The PC-relative forms, which position-independent code uses in place of
  // every one of the absolute forms above. Same operand positions - all that
  // changes is how the word they emit is to be read.
  case Moe::JMPpc:
  case Moe::JCCpc:
  case Moe::JMPabs_pc:
    T.SymOperand = 0;
    T.PcRel = true;
    break;
  case Moe::LOADpc:
  case Moe::STOREpc:
    T.SymOperand = 1;
    T.PcRel = true;
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
  // The short register-indirect twins, chosen by MoeMCInstLower when the
  // offset fits twelve signed bits. Same operand positions as the long forms
  // they came from; only the width and the placement of the emitted operand
  // differ.
  case Moe::LOADrr_S:
  case Moe::STORErr_S:
  case Moe::LOADrr_AS:
  case Moe::STORErr_AS:
  case Moe::STORErr_BS:
  case Moe::STORErr_HS:
    T.MemBaseOperand = 1;
    T.Short = true;
    break;
  case Moe::LOADrr_BS:
  case Moe::LOADrr_HS:
    T.MemBaseOperand = 2;
    T.Short = true;
    break;
  // The short PC-relative branches (N1). Both halves of the short form at
  // once: the operand is a halfword right after the opcode, *and* it holds a
  // displacement rather than a base register and a constant - so this is the
  // one shape with no base-register operand to read, like the long PC-relative
  // forms, and no alignment of its own, like the short register-indirect ones.
  case Moe::JMPpc_S:
  case Moe::JCCpc_S:
    T.SymOperand = 0;
    T.PcRel = true;
    T.Short = true;
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

  if (T.Short && T.PcRel) {
    // A short branch's own halfword. Same shape as the long PC-relative case
    // below - a fixup over bytes that already hold IA's register select - with
    // two differences: the field is twelve bits instead of twenty-eight, and
    // the addend is -2 rather than -4, because IA stands at the end of a
    // two-byte operand rather than a four-byte one.
    const MCOperand &MO = Inst.getOperand(T.SymOperand);
    assert(MO.isExpr() && "A short PC-relative branch names a block");
    MCContext &Ctx = getContext();
    const MCExpr *Expr = MCBinaryExpr::createSub(
        MO.getExpr(), MCConstantExpr::create(2, Ctx), Ctx);
    ensureHeadroom(2);
    getCurrentFragment()->addFixup(MCFixup::create(
        getCurFragSize(), Expr, Moe::fixup_moe_pcrel12, /*PCRel=*/true));
    const char Half[2] = {static_cast<char>(IARegSelect), 0};
    appendContents(Half);
    return;
  }

  if (T.Short) {
    // No alignment directive and no padding: the operand is the halfword that
    // immediately follows, which is what makes the pair one aligned word when
    // the opcode is at a word-aligned address.
    const MCOperand &Base = Inst.getOperand(T.MemBaseOperand);
    const MCOperand &Offset = Inst.getOperand(T.MemBaseOperand + 1);
    assert(Base.isReg() && "Short register-indirect needs a base register");
    assert(Offset.isImm() &&
           "Short register-indirect offset must be a constant - "
           "MoeMCInstLower only picks this form for one");
    uint32_t BaseEnc =
        getContext().getRegisterInfo()->getEncodingValue(Base.getReg());
    // Base register in bits 3-0 and a 12-bit signed offset in bits 15-4,
    // mirroring the long form's own layout - and matching
    // emulator/src/cpu.rs's resolve_trailing_operand.
    uint16_t Packed = static_cast<uint16_t>(
        (static_cast<uint32_t>(Offset.getImm()) << 4) | (BaseEnc & 0xF));
    emitIntValue(Packed, 2);
    return;
  }

  // Fill with zeroes rather than any instruction pattern: this padding sits
  // between an instruction and its own operand word and is never reached by
  // control flow, which is the same reasoning MoeAsmBackend::writeNopData
  // already records for why Moe needs no NOP opcode (it has none - all 16
  // opcodes are real instructions).
  emitValueToAlignment(Align(4), /*Fill=*/0, /*FillLen=*/1,
                       /*MaxBytesToEmit=*/0);

  if (T.PcRel) {
    const MCOperand &MO = Inst.getOperand(T.SymOperand);
    assert(MO.isExpr() && "A PC-relative trailing operand names a symbol - a "
                          "bare address has nothing to be relative to");
    MCContext &Ctx = getContext();

    // The displacement the hardware wants is measured from the *end* of this
    // word, because that is where IA stands by the time the operand resolves
    // (emulator/src/cpu.rs sets IA past the operand before computing the
    // effective address). The fixup itself is relative to the word's own
    // address, so the four-byte difference is carried as an addend - which
    // works out identically whether MC resolves the fixup itself or leaves a
    // relocation for the linker, since both add it in.
    const MCExpr *Expr = MCBinaryExpr::createSub(
        MO.getExpr(), MCConstantExpr::create(4, Ctx), Ctx);

    // Emitted by hand rather than through emitValue, because no generic path
    // can place a fixup of a target-specific kind over bytes that already hold
    // something - and these bytes do: the low nibble is IA's register select,
    // which the relocation deliberately leaves alone.
    ensureHeadroom(4);
    getCurrentFragment()->addFixup(MCFixup::create(
        getCurFragSize(), Expr, Moe::fixup_moe_pcrel28, /*PCRel=*/true));
    const char Word[4] = {static_cast<char>(IARegSelect), 0, 0, 0};
    appendContents(Word);
    return;
  }

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
