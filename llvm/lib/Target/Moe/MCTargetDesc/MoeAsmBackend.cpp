//===-- MoeAsmBackend.cpp - Moe Assembler Backend ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
class MoeAsmBackend : public MCAsmBackend {
public:
  MoeAsmBackend() : MCAsmBackend(llvm::endianness::little) {}

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createMoeELFObjectWriter();
  }

  // Only the generic FK_Data_4 fixup kind is used for Milestone 2 (every
  // relocatable reference is a flat 32-bit absolute address - see the
  // Milestone 2 plan) - the base class's handling of it is already correct,
  // no target-specific fixup kinds/getFixupKindInfo override needed.

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

void MoeAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                const MCValue &Target, uint8_t *Data,
                                uint64_t Value, bool IsResolved) {
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);
  if (!Value)
    return;

  MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());
  unsigned NumBytes = alignTo(Info.TargetSize + Info.TargetOffset, 8) / 8;
  assert(Fixup.getOffset() + NumBytes <= F.getSize() && "Invalid fixup offset!");
  for (unsigned i = 0; i != NumBytes; ++i)
    Data[i] |= uint8_t((Value >> (i * 8)) & 0xff);
}

bool MoeAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                  const MCSubtargetInfo *STI) const {
  // No NOP opcode exists - all 16 opcodes are real instructions (see
  // CLAUDE.md's "no opcode sharing" note). Alignment/padding regions are
  // never reached by control flow (they only ever sit between an
  // instruction and its own trailing operand word, or before a function's
  // word-aligned start), so zero-fill is fine - see MoeInstrFormats.td's
  // matching note for the same reasoning applied at the encoder level.
  for (uint64_t i = 0; i != Count; ++i)
    OS.write('\0');
  return true;
}

} // end anonymous namespace

MCAsmBackend *llvm::createMoeMCAsmBackend(const Target &T,
                                           const MCSubtargetInfo &STI,
                                           const MCRegisterInfo &MRI,
                                           const MCTargetOptions &Options) {
  return new MoeAsmBackend();
}
