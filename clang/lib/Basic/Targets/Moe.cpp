//===--- Moe.cpp - Implement Moe target feature support --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements Moe TargetInfo objects.
//
//===----------------------------------------------------------------------===//

#include "Moe.h"
#include "clang/Basic/MacroBuilder.h"

using namespace clang;
using namespace clang::targets;

const char *const MoeTargetInfo::GCCRegNames[] = {
    "gp0", "gp1", "gp2", "gp3", "gp4", "gp5", "gp6", "gp7",
    "ia",  "sp",  "tt",  "f",   "im",  "ifs", "ra",  "rf"
};

ArrayRef<const char *> MoeTargetInfo::getGCCRegNames() const {
  return llvm::ArrayRef(GCCRegNames);
}

void MoeTargetInfo::getTargetDefines(const LangOptions &Opts,
                                      MacroBuilder &Builder) const {
  Builder.defineMacro("MOE");
  Builder.defineMacro("__MOE__");
}
