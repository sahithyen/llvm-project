//===-- MoeMCCodeEmitter.cpp - Convert Moe code to machine code ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MoeMCCodeEmitter class.
//
// Every instruction's 16-bit opcode halfword is produced by the
// TableGen-generated getBinaryCodeForInstr (from the `Inst` bit layouts in
// MoeInstrFormats.td). JUMP/LOAD/STORE's trailing operand word is NOT part
// of `Inst` at all (no operand referencing it has a `let Inst{...} = ...`
// assignment - see 'Addressing mode' in the Encoding chapter), so it's
// hand-emitted here instead of through the generated tables, including the
// alignment-padding rule: the trailing word must land on the next
// word-aligned address after the instruction, which requires tracking each
// function's running byte offset from its (guaranteed, see
// MoeISelLowering's setMinFunctionAlignment) word-aligned start.
//
//===----------------------------------------------------------------------===//

#include "Moe.h"
#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/EndianStream.h"

#define DEBUG_TYPE "mccodeemitter"

using namespace llvm;

namespace {
class MoeMCCodeEmitter : public MCCodeEmitter {
  MCContext &Ctx;
  MCInstrInfo const &MCII;

  // Bytes emitted since the current function's (word-aligned) start - see
  // MoeAsmPrinter::emitFunctionBodyStart, which resets this via
  // resetMoeCodeEmitterOffset. Only its value mod 4 matters; kept as a full
  // count for debuggability.
  mutable uint64_t CurrentFunctionOffset = 0;

  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  unsigned getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  void emitTrailingWord(const MCInst &MI, SmallVectorImpl<char> &CB,
                        SmallVectorImpl<MCFixup> &Fixups) const;

public:
  MoeMCCodeEmitter(MCContext &ctx, MCInstrInfo const &MCII)
      : Ctx(ctx), MCII(MCII) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  void resetFunctionOffset() const { CurrentFunctionOffset = 0; }
};
} // end anonymous namespace

void MoeMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                          SmallVectorImpl<char> &CB,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {
  uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
  support::endian::write(CB, (uint16_t)Bits, llvm::endianness::little);
  CurrentFunctionOffset += 2;

  emitTrailingWord(MI, CB, Fixups);
}

void MoeMCCodeEmitter::emitTrailingWord(const MCInst &MI,
                                        SmallVectorImpl<char> &CB,
                                        SmallVectorImpl<MCFixup> &Fixups) const {
  // Which operand holds the trailing word's addressing info, and its
  // shape: a single symbol-valued operand (Absolute mode - moeaddr/
  // jmptarget/calltarget), or a (base register, offset immediate) pair
  // (Register-indirect mode - moemem, no relocation, just a packed value).
  int SymOperand = -1;
  int MemBaseOperand = -1;

  switch (MI.getOpcode()) {
  case Moe::JMP:
  case Moe::JCC:
  case Moe::JMPabs:
    SymOperand = 0;
    break;
  case Moe::LOADabs:
  case Moe::STOREabs:
    SymOperand = 1;
    break;
  case Moe::LOADrr:
  case Moe::STORErr:
    MemBaseOperand = 1;
    break;
  default:
    return; // No trailing operand for this instruction.
  }

  // Bytes appended by this call so far (relative to this encodeInstruction
  // invocation, which is what MCFixup offsets must be relative to - the
  // streamer adds this fragment's own base offset automatically).
  uint32_t LocalOffset = 2;

  if ((CurrentFunctionOffset % 4) != 0) {
    support::endian::write(CB, (uint16_t)0, llvm::endianness::little);
    LocalOffset += 2;
    CurrentFunctionOffset += 2;
  }

  if (SymOperand >= 0) {
    const MCOperand &MO = MI.getOperand(SymOperand);
    assert(MO.isExpr() &&
           "Absolute-mode trailing operand must be a symbol expression");
    Fixups.push_back(MCFixup::create(LocalOffset, MO.getExpr(), FK_Data_4));
    support::endian::write(CB, (uint32_t)0, llvm::endianness::little);
  } else {
    const MCOperand &Base = MI.getOperand(MemBaseOperand);
    const MCOperand &Offset = MI.getOperand(MemBaseOperand + 1);
    assert(Base.isReg() && Offset.isImm() &&
           "Register-indirect trailing operand must be (reg, imm)");
    uint32_t BaseEnc = Ctx.getRegisterInfo()->getEncodingValue(Base.getReg());
    // Matches emulator/src/cpu.rs's resolve_trailing_operand exactly: base
    // register select in bits 3-0, 28-bit signed offset in bits 31-4.
    uint32_t Packed = (static_cast<uint32_t>(Offset.getImm()) << 4) |
                       (BaseEnc & 0xF);
    support::endian::write(CB, Packed, llvm::endianness::little);
  }

  CurrentFunctionOffset += 4;
}

unsigned MoeMCCodeEmitter::getMachineOpValue(const MCInst &MI,
                                              const MCOperand &MO,
                                              SmallVectorImpl<MCFixup> &Fixups,
                                              const MCSubtargetInfo &STI) const {
  if (MO.isReg())
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());

  // Every operand actually assigned into `Inst` (see MoeInstrFormats.td) is
  // either a register or a plain immediate - no Inst-bound field is ever a
  // symbol (those all live in the hand-emitted trailing word above).
  assert(MO.isImm() && "Expected register or immediate for an Inst-bound field");
  return static_cast<unsigned>(MO.getImm());
}

MCCodeEmitter *llvm::createMoeMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx) {
  return new MoeMCCodeEmitter(Ctx, MCII);
}

void llvm::resetMoeCodeEmitterOffset(MCCodeEmitter &MCE) {
  static_cast<MoeMCCodeEmitter &>(MCE).resetFunctionOffset();
}

#include "MoeGenMCCodeEmitter.inc"
