//===-- MoeTargetMachine.h - Define TargetMachine for Moe -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the Moe specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOETARGETMACHINE_H
#define LLVM_LIB_TARGET_MOE_MOETARGETMACHINE_H

#include "MoeSubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include <optional>

namespace llvm {
class StringRef;

class MoeTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  MoeSubtarget Subtarget;

public:
  MoeTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                    StringRef FS, const TargetOptions &Options,
                    std::optional<Reloc::Model> RM,
                    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                    bool JIT);
  ~MoeTargetMachine() override;

  const MoeSubtarget *getSubtargetImpl(const Function &F) const override {
    return &Subtarget;
  }
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }

  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                            const TargetSubtargetInfo *STI) const override;
};

} // end namespace llvm

#endif
