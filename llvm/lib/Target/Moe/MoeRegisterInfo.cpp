//===-- MoeRegisterInfo.cpp - Moe Register Information -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Moe implementation of the TargetRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#include "MoeRegisterInfo.h"
#include "MoeFrameLowering.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "moe-reg-info"

#define GET_REGINFO_TARGET_DESC
#include "MoeGenRegisterInfo.inc"

MoeRegisterInfo::MoeRegisterInfo() : MoeGenRegisterInfo(Moe::IA) {}

const MCPhysReg *
MoeRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  // GP6-GP7 are callee-saved - see the Milestone 1 plan's ABI section.
  static const MCPhysReg CalleeSavedRegs[] = {Moe::GP6, Moe::GP7, 0};
  return CalleeSavedRegs;
}

BitVector MoeRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());

  // IA and SP are hardware-special, never general-purpose allocatable.
  Reserved.set(Moe::IA);
  Reserved.set(Moe::SP);

  // Privileged registers - Milestone 1 generates no code that touches
  // these, but they're never allocatable in any case.
  Reserved.set(Moe::TT);
  Reserved.set(Moe::F);
  Reserved.set(Moe::IM);
  Reserved.set(Moe::IFS);
  Reserved.set(Moe::RA);
  Reserved.set(Moe::RF);

  // GP4-GP5 are caller-saved scratch, reserved for literal-pool/call-
  // sequence temporaries - see the Milestone 1 plan's ABI section. Kept out
  // of general allocation so instruction selection always has somewhere to
  // put these without fighting the register allocator.
  Reserved.set(Moe::GP4);
  Reserved.set(Moe::GP5);

  return Reserved;
}

const TargetRegisterClass *
MoeRegisterInfo::getPointerRegClass(unsigned Kind) const {
  return &Moe::GPRRegClass;
}

bool MoeRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                           int SPAdj, unsigned FIOperandNum,
                                           RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected");
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();
  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();

  // No frame pointer for Milestone 1 (see getFrameRegister below), so every
  // frame-index is SP-relative. MoeFrameLowering's prologue always finishes
  // adjusting SP before any frame-indexed access executes, so the object's
  // recorded offset plus the total (post-prologue) frame size is exactly
  // its offset from the current SP. This formula itself needs no fixup for
  // the return address, since it works purely in terms of whatever SPOffset
  // a frame index was created with - but incoming stack-argument fixed
  // objects (see LowerFormalArguments) do need a +4 baked into that
  // SPOffset: unlike hardware-CALL targets, Moe's return address is pushed
  // explicitly by the caller and stays on the stack until this function's
  // own epilogue POPs it, so it still occupies the first word at entry SP,
  // one word below where the caller's overflow arguments actually start.
  int Offset = MF.getFrameInfo().getObjectOffset(FrameIndex) +
               MF.getFrameInfo().getStackSize();
  Offset += MI.getOperand(FIOperandNum + 1).getImm();

  MI.getOperand(FIOperandNum).ChangeToRegister(Moe::SP, false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
  return false;
}

Register MoeRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  // No frame pointer for Milestone 1 - see the Milestone 1 plan's ABI
  // section.
  return Moe::SP;
}
