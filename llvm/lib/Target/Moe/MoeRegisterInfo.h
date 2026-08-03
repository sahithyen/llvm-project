//===-- MoeRegisterInfo.h - Moe Register Information Impl -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Moe implementation of the MRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOEREGISTERINFO_H
#define LLVM_LIB_TARGET_MOE_MOEREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "MoeGenRegisterInfo.inc"

namespace llvm {

class MoeRegisterInfo : public MoeGenRegisterInfo {
public:
  MoeRegisterInfo();

  const MCPhysReg *
  getCalleeSavedRegs(const MachineFunction *MF) const override;

  BitVector getReservedRegs(const MachineFunction &MF) const override;
  const TargetRegisterClass *
  getPointerRegClass(unsigned Kind = 0) const override;

  bool eliminateFrameIndex(MachineBasicBlock::iterator II, int SPAdj,
                            unsigned FIOperandNum,
                            RegScavenger *RS = nullptr) const override;

  Register getFrameRegister(const MachineFunction &MF) const override;
};

} // end namespace llvm

#endif
