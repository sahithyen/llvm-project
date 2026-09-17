//===-- MoeELFObjectWriter.cpp - Moe ELF Writer --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MoeFixupKinds.h"
#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
class MoeELFObjectWriter : public MCELFObjectTargetWriter {
public:
  MoeELFObjectWriter()
      : MCELFObjectTargetWriter(/*Is64Bit*/ false, ELF::ELFOSABI_STANDALONE,
                                 ELF::EM_MOE, /*HasRelocationAddend*/ true) {}

  ~MoeELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    // Two shapes of relocatable reference, one per relocation. An absolute
    // trailing operand (moeaddr/jmptarget/calltarget) is a flat 32-bit address;
    // a PC-relative one is a 28-bit displacement in the same word's upper bits,
    // which is what position-independent code uses for all of them (see
    // MoeFixupKinds.h).
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return ELF::R_MOE_32;
    case Moe::fixup_moe_pcrel28:
      return ELF::R_MOE_PCREL28;
    default:
      llvm_unreachable("Invalid fixup kind");
    }
  }
};
} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter> llvm::createMoeELFObjectWriter() {
  return std::make_unique<MoeELFObjectWriter>();
}
