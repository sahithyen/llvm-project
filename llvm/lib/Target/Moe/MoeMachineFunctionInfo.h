//=== MoeMachineFunctionInfo.h - Moe machine function info -------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares Moe-specific per-machine-function information.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOEMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_MOE_MOEMACHINEFUNCTIONINFO_H

#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {

/// Moe target-specific information for each MachineFunction.
class MoeMachineFunctionInfo : public MachineFunctionInfo {
  virtual void anchor();

  /// Size, in bytes, of the callee-saved (GP6/GP7) portion of the stack
  /// frame - see MoeFrameLowering's prologue/epilogue and
  /// MoeRegisterInfo::eliminateFrameIndex.
  unsigned CalleeSavedFrameSize = 0;

  /// Frame index of the variadic-argument register-save area (Milestone 14)
  /// - see LowerFormalArguments's isVarArg block and LowerVASTART. -1 if
  /// this function isn't variadic or consumed all 4 argument registers with
  /// named parameters (nothing to save).
  int VarArgsFrameIndex = -1;

public:
  MoeMachineFunctionInfo() = default;
  MoeMachineFunctionInfo(const Function &F, const TargetSubtargetInfo *STI) {}

  MachineFunctionInfo *
  clone(BumpPtrAllocator &Allocator, MachineFunction &DestMF,
        const DenseMap<MachineBasicBlock *, MachineBasicBlock *> &Src2DstMBB)
      const override;

  unsigned getCalleeSavedFrameSize() const { return CalleeSavedFrameSize; }
  void setCalleeSavedFrameSize(unsigned Bytes) { CalleeSavedFrameSize = Bytes; }

  int getVarArgsFrameIndex() const { return VarArgsFrameIndex; }
  void setVarArgsFrameIndex(int FI) { VarArgsFrameIndex = FI; }
};

} // end namespace llvm

#endif
