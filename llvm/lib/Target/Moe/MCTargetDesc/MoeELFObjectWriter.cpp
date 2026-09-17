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
#include "llvm/ADT/Twine.h"
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
    // An instruction's trailing operand word is either a flat 32-bit address or
    // a 28-bit displacement in that word's upper bits, which is what
    // position-independent code uses for every one of them - see
    // MoeFixupKinds.h.
    //
    // A *data* word is the generic FK_Data_4, and IsPCRel distinguishes the two
    // things it can mean. `.long sym` is the address; `.long sym - .` is the
    // distance to it, which the generic ELF writer has already turned into a
    // PC-relative relocation with the subtrahend folded into the addend by the
    // time this is asked. Ignoring IsPCRel here - which this did - answers
    // R_MOE_32 for both, so `.long sym - .` assembled to the plain address of
    // sym, silently, with no diagnostic anywhere in the pipeline.
    switch (Fixup.getKind()) {
    case FK_Data_4:
      return IsPCRel ? ELF::R_MOE_PCREL32 : ELF::R_MOE_32;
    case Moe::fixup_moe_pcrel28:
      return ELF::R_MOE_PCREL28;
    case Moe::fixup_moe_pcrel12:
      // A short form's twelve bits reach +-2 KiB, which is a distance inside
      // one function, so MC resolves every one of these at layout and none
      // should ever arrive here. One that did would be a reference across
      // sections that no relocation can express - there is no R_MOE for a
      // shifted 12-bit field, and inventing one would mean a linker that
      // silently truncates. Reported rather than emitted.
      reportError(Fixup.getLoc(),
                  Twine("a short PC-relative branch cannot reach outside its "
                        "own section, and there is no relocation for one that "
                        "tries"));
      return ELF::R_MOE_NONE;
    default:
      llvm_unreachable("Invalid fixup kind");
    }
  }
};
} // end anonymous namespace

std::unique_ptr<MCObjectTargetWriter> llvm::createMoeELFObjectWriter() {
  return std::make_unique<MoeELFObjectWriter>();
}
