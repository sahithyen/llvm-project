//==-- Moe.h - Top-level interface for Moe representation --------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in
// the LLVM Moe backend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOE_H
#define LLVM_LIB_TARGET_MOE_MOE_H

#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
class FunctionPass;
class MoeTargetMachine;
class PassRegistry;

FunctionPass *createMoeISelDag(MoeTargetMachine &TM, CodeGenOptLevel OptLevel);

/// Picks the four-byte PC-relative form for every branch that reaches - the
/// ISA evaluation's N1. Pre-emit, because the decision needs the layout.
FunctionPass *createMoeShortBranchesPass();

void initializeMoeAsmPrinterPass(PassRegistry &);
void initializeMoeDAGToDAGISelLegacyPass(PassRegistry &);
void initializeMoeShortBranchesPass(PassRegistry &);

} // namespace llvm

#endif
