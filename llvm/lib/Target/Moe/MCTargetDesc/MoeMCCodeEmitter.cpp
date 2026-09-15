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
// MoeInstrFormats.td), and that halfword is all this emitter produces.
//
// JUMP/LOAD/STORE's trailing operand word is NOT part of `Inst` at all (no
// operand referencing it has a `let Inst{...} = ...` assignment - see
// 'Addressing mode' in the Encoding chapter) and is not emitted here either:
// it must land on the next word-aligned address after the opcode halfword,
// and whether that costs padding depends on the halfword's own position,
// which an MCCodeEmitter is not told and which layout has not decided yet.
// MoeELFStreamer emits it instead, as an alignment directive plus a 4-byte
// value - see that file's comment for the concrete miscompile that motivated
// moving it there.
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

  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  unsigned getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

public:
  MoeMCCodeEmitter(MCContext &ctx, MCInstrInfo const &MCII)
      : Ctx(ctx), MCII(MCII) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;
};
} // end anonymous namespace

void MoeMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                          SmallVectorImpl<char> &CB,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {
  uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
  support::endian::write(CB, (uint16_t)Bits, llvm::endianness::little);
}

unsigned MoeMCCodeEmitter::getMachineOpValue(const MCInst &MI,
                                              const MCOperand &MO,
                                              SmallVectorImpl<MCFixup> &Fixups,
                                              const MCSubtargetInfo &STI) const {
  if (MO.isReg())
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());

  // Every operand actually assigned into `Inst` (see MoeInstrFormats.td) is
  // either a register or a plain immediate - no Inst-bound field is ever a
  // symbol (those all live in the trailing word, emitted by
  // MoeELFStreamer).
  assert(MO.isImm() && "Expected register or immediate for an Inst-bound field");
  return static_cast<unsigned>(MO.getImm());
}

MCCodeEmitter *llvm::createMoeMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx) {
  return new MoeMCCodeEmitter(Ctx, MCII);
}

#include "MoeGenMCCodeEmitter.inc"
