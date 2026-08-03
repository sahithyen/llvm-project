//===-- MoeELFObjectWriter.cpp - Moe ELF Writer --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

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
    // Every relocatable reference Milestone 2 emits (moeaddr/jmptarget/
    // calltarget's trailing word) is a flat 32-bit absolute address - see
    // the Milestone 2 plan.
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return ELF::R_MOE_32;
    default:
      llvm_unreachable("Invalid fixup kind");
    }
  }
};
} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter> llvm::createMoeELFObjectWriter() {
  return std::make_unique<MoeELFObjectWriter>();
}
