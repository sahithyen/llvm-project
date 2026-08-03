//==- MoeFrameLowering.h - Define frame lowering for Moe --------*- C++ -*--==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOEFRAMELOWERING_H
#define LLVM_LIB_TARGET_MOE_MOEFRAMELOWERING_H

#include "Moe.h"
#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class MoeSubtarget;
class MoeInstrInfo;
class MoeRegisterInfo;

class MoeFrameLowering : public TargetFrameLowering {
protected:
  bool hasFPImpl(const MachineFunction &MF) const override;

public:
  explicit MoeFrameLowering(const MoeSubtarget &STI);

  const MoeSubtarget &STI;
  const MoeInstrInfo &TII;
  const MoeRegisterInfo *TRI;

  void emitPrologue(MachineFunction &MF,
                     MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF,
                     MachineBasicBlock &MBB) const override;

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator I) const override;

  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MI,
                                  ArrayRef<CalleeSavedInfo> CSI,
                                  const TargetRegisterInfo *TRI) const override;
  bool restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator MI,
                                    MutableArrayRef<CalleeSavedInfo> CSI,
                                    const TargetRegisterInfo *TRI) const override;

  bool hasReservedCallFrame(const MachineFunction &MF) const override;
};

} // namespace llvm

#endif
