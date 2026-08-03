//===-- MoeTargetInfo.cpp - Moe Target Implementation --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/MoeTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
using namespace llvm;

Target &llvm::getTheMoeTarget() {
  static Target TheMoeTarget;
  return TheMoeTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMoeTargetInfo() {
  RegisterTarget<Triple::moe> X(getTheMoeTarget(), "moe",
                                "Moe [experimental]", "Moe");
}
