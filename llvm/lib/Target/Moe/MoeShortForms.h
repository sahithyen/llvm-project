//===-- MoeShortForms.h - Which instructions take the short form -*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The short addressing forms are four bytes where the long ones are six or
// eight, and *two* passes now need to know which instructions get one:
// MoeMCInstLower, which does the rewriting, and MoeShortBranches, which has to
// predict the size of everything in a function in order to work out which
// branches reach. Two copies of that rule would be two copies of one fact, and
// the second one going stale would not fail to build - it would mispredict a
// displacement, which is the one kind of mistake this whole area is trying to
// avoid. So it lives here, once.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOESHORTFORMS_H
#define LLVM_LIB_TARGET_MOE_MOESHORTFORMS_H

#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "llvm/Support/MathExtras.h"
#include <cstdint>
#include <cstdlib>

namespace llvm {
namespace Moe {

/// The short twin of a register-indirect LOAD/STORE, or -1 if there isn't one.
///
/// The two differ only in Addressing mode - bit 8 of the opcode halfword - and
/// in whether the operand is a halfword after the opcode or a word at the next
/// aligned address. Every operand list is identical, so the rewrite is a change
/// of opcode and nothing else.
inline int shortRegisterIndirectOpcode(unsigned Opc) {
  switch (Opc) {
  case Moe::LOADrr:     return Moe::LOADrr_S;
  case Moe::STORErr:    return Moe::STORErr_S;
  case Moe::LOADrr_A:   return Moe::LOADrr_AS;
  case Moe::STORErr_A:  return Moe::STORErr_AS;
  case Moe::LOADrr_B:   return Moe::LOADrr_BS;
  case Moe::LOADrr_H:   return Moe::LOADrr_HS;
  case Moe::STORErr_B:  return Moe::STORErr_BS;
  case Moe::STORErr_H:  return Moe::STORErr_HS;
  default:              return -1;
  }
}

/// Which operand of a register-indirect LOAD/STORE is the base register; its
/// offset is the one after it. LOADrr_B/H carry a tied $oldval ahead of the
/// address, so their base is operand 2 while everything else's is operand 1.
inline int memBaseOperand(unsigned Opc) {
  switch (Opc) {
  case Moe::LOADrr_B:
  case Moe::LOADrr_H:
    return 2;
  case Moe::LOADrr:
  case Moe::STORErr:
  case Moe::LOADrr_A:
  case Moe::STORErr_A:
  case Moe::STORErr_B:
  case Moe::STORErr_H:
    return 1;
  default:
    return -1;
  }
}

/// Twelve signed bits, which is what a halfword operand has left once four go
/// to the base register - see 'Addressing mode' in the Encoding chapter. The
/// same field width, and so the same test, for a branch's displacement.
inline bool fitsShortForm(int64_t Value) { return isInt<12>(Value); }

/// MOE_NO_SHORT_FORM turns the register-indirect rewrite off, and
/// MOE_NO_SHORT_BRANCH the branch one. They exist because the only honest way
/// to price an encoding is to build the same source both ways and run both -
/// comparing against a measurement taken before some other commit compares two
/// things that differ in more than one way, which this evaluation has been
/// wrong about twice. Read once: a build does not change its mind halfway.
inline bool shortFormsDisabled() {
  static const bool Disabled = std::getenv("MOE_NO_SHORT_FORM") != nullptr;
  return Disabled;
}

inline bool shortBranchesDisabled() {
  static const bool Disabled = std::getenv("MOE_NO_SHORT_BRANCH") != nullptr;
  return Disabled;
}

/// The short PC-relative twin of a branch, or -1 if there isn't one. Both the
/// absolute forms and the long PC-relative ones map here: a short branch is
/// PC-relative by construction, since its twelve bits are a displacement.
inline int shortBranchOpcode(unsigned Opc) {
  switch (Opc) {
  case Moe::JMP:
  case Moe::JMPpc:   return Moe::JMPpc_S;
  case Moe::JCC:
  case Moe::JCCpc:   return Moe::JCCpc_S;
  default:           return -1;
  }
}

} // end namespace Moe
} // end namespace llvm

#endif
