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
#include "MoeInstrInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Constants.h"
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

  // GP7 is the frame pointer, but only in the functions that need one - see
  // MoeFrameLowering::hasFPImpl. Everywhere else it stays a general
  // callee-saved register.
  if (MF.getSubtarget().getFrameLowering()->hasFP(MF))
    Reserved.set(Moe::GP7);

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

  // Frame indices are relative to SP, or to the frame pointer in a function
  // that has one - and the offset is the same either way, because the frame
  // pointer is set to exactly the SP the prologue finishes with. The object's
  // recorded offset plus the total (post-prologue) frame size is that offset. This formula itself needs no fixup for
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

  if (MI.getOpcode() == Moe::FIADDR) {
    // A bare stack-object ADDRESS used as an ordinary value (see the
    // Milestone 7 plan's "FrameIndex-as-value" fix) - unlike every other
    // FI-bearing instruction (LOADrr/STORErr/spill code), whose (reg, imm)
    // operand pair can just be rewritten in place because the consuming
    // instruction already dereferences memory at (SP + Offset), FIADDR's
    // *result* must equal SP + Offset itself, and Moe has no register+
    // immediate ADD (see MoeFrameLowering.cpp's header comment) - so this
    // needs the same real MOVE/LOADabs/ADD sequence
    // MoeFrameLowering::adjustStackPointer already established for "compute
    // SP adjusted by an arbitrary offset with no fresh virtual registers
    // available" (GP4/GP5 are excluded from register allocation entirely -
    // see getReservedRegs above - so nothing RegAllocFast ever assigned can
    // be live in them here, the same invariant adjustStackPointer/emitCall's
    // own hand-built GP4/GP5 sequences already rely on).
    //
    // Always BuildMI-and-erase, even for Offset == 0 (rather than mutating
    // the pseudo in place for that case), to avoid any risk of operand-index
    // confusion from changing a 2-operand pseudo's MCInstrDesc mid-flight.
    MachineBasicBlock &MBB = *MI.getParent();
    const MoeInstrInfo &TII =
        *static_cast<const MoeInstrInfo *>(MF.getSubtarget().getInstrInfo());
    DebugLoc DL = MI.getDebugLoc();
    Register DstReg = MI.getOperand(0).getReg();

    Register FrameReg = getFrameRegister(MF);
    if (Offset == 0) {
      BuildMI(MBB, II, DL, TII.get(Moe::MOVE), DstReg).addReg(FrameReg);
    } else {
      Constant *CV = ConstantInt::get(
          Type::getInt32Ty(MF.getFunction().getContext()), (uint64_t)Offset);
      unsigned CPI = MF.getConstantPool()->getConstantPoolIndex(CV, Align(4));

      BuildMI(MBB, II, DL, TII.get(Moe::MOVE), Moe::GP4).addReg(FrameReg);
      BuildMI(MBB, II, DL, TII.get(Moe::LOADabs), Moe::GP5)
          .addConstantPoolIndex(CPI);
      BuildMI(MBB, II, DL, TII.get(Moe::ADD), DstReg)
          .addReg(Moe::GP4)
          .addReg(Moe::GP5);
    }
    MI.eraseFromParent();
    return false;
  }

  MI.getOperand(FIOperandNum).ChangeToRegister(getFrameRegister(MF), false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
  return false;
}

Register MoeRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  // GP7 in a function with a variable-sized stack object, SP in every other -
  // see MoeFrameLowering::hasFPImpl.
  //
  // A function that only realigns the stack also has a frame pointer, but it
  // is not the base locals are addressed from: there GP7 records where SP was
  // *before* the frame was allocated and aligned, purely so the epilogue can
  // put it back. SP is the base, and it is a fixed one, because a realigning
  // function is not allowed a variable-sized object (MoeFrameLowering's
  // prologue rejects the combination).
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  bool FPIsBase = MFI.hasVarSizedObjects() || MFI.isFrameAddressTaken();
  return FPIsBase ? Moe::GP7 : Moe::SP;
}
