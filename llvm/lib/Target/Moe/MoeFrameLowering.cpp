//===-- MoeFrameLowering.cpp - Moe Frame Information ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Moe implementation of TargetFrameLowering class.
//
// Moe has no immediate-arithmetic instruction wider than INCREMENT's 6-bit
// (0-63) field, and ADD/SUB can only address GP0-7 (3-bit GP-only operand
// fields - see 'ADD'/'SUB' in the Instruction set chapter), never SP. So
// adjusting SP by an arbitrary, possibly-large byte count needs a small
// round trip through a GP scratch register: MOVE SP into GP4, materialize
// the byte count via the literal pool into GP5 (no load-immediate
// instruction exists either), SUB/ADD GP4 by GP5, then MOVE the result back
// into SP - see adjustStackPointer below.
//
//===----------------------------------------------------------------------===//

#include "MoeFrameLowering.h"
#include "MoeInstrInfo.h"
#include "MoeMachineFunctionInfo.h"
#include "MoeSubtarget.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/IR/Constants.h"

using namespace llvm;

MoeFrameLowering::MoeFrameLowering(const MoeSubtarget &STI)
    : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, Align(4), 0,
                           Align(4)),
      STI(STI), TII(*STI.getInstrInfo()), TRI(STI.getRegisterInfo()) {}

// A frame pointer only where one is unavoidable: a function with a
// variable-sized stack object moves SP by an amount nothing can recompute, so
// its locals need a base that does not move, and its epilogue needs somewhere
// to restore SP from. Everywhere else - which is almost everywhere - the frame
// stays SP-relative and GP7 stays allocatable. Six allocatable registers is
// already the tightest thing about this target (psabi.md); spending one of
// them by default would not be worth it for a feature most functions never
// use.
bool MoeFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return MFI.hasVarSizedObjects() || MFI.isFrameAddressTaken();
}

// GP7 is the frame pointer when there is one, so it has to be saved and
// restored like any other callee-saved register - and it is reserved for the
// whole function (MoeRegisterInfo::getReservedRegs), so the ordinary
// "was it modified?" scan would not notice it.
void MoeFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                            BitVector &SavedRegs,
                                            RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  if (hasFP(MF))
    SavedRegs.set(Moe::GP7);
}

bool MoeFrameLowering::hasReservedCallFrame(const MachineFunction &MF) const {
  return !MF.getFrameInfo().hasVarSizedObjects();
}

// Emits: MOVE SP -> GP4 / LOADabs [pool:Amount] -> GP5 / (ADD|SUB) GP4, GP5
// -> GP4 / MOVE GP4 -> SP. Delta > 0 grows the frame (SP -= Amount, SUB);
// Delta < 0 shrinks it back (SP += Amount, ADD).
static void adjustStackPointer(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MBBI,
                                const DebugLoc &DL, const MoeInstrInfo &TII,
                                MachineFunction &MF, int64_t Delta,
                                MachineInstr::MIFlag Flag) {
  if (Delta == 0)
    return;

  uint64_t Amount = Delta < 0 ? -Delta : Delta;
  Constant *CV = ConstantInt::get(
      Type::getInt32Ty(MF.getFunction().getContext()), Amount);
  unsigned CPI = MF.getConstantPool()->getConstantPoolIndex(CV, Align(4));

  BuildMI(MBB, MBBI, DL, TII.get(Moe::MOVE), Moe::GP4)
      .addReg(Moe::SP)
      .setMIFlag(Flag);
  BuildMI(MBB, MBBI, DL, TII.get(Moe::LOADabs), Moe::GP5)
      .addConstantPoolIndex(CPI)
      .setMIFlag(Flag);
  BuildMI(MBB, MBBI, DL, TII.get(Delta > 0 ? Moe::SUB : Moe::ADD), Moe::GP4)
      .addReg(Moe::GP4)
      .addReg(Moe::GP5)
      .setMIFlag(Flag);
  BuildMI(MBB, MBBI, DL, TII.get(Moe::MOVE), Moe::SP)
      .addReg(Moe::GP4)
      .setMIFlag(Flag);
}

void MoeFrameLowering::emitPrologue(MachineFunction &MF,
                                    MachineBasicBlock &MBB) const {
  assert(&MF.front() == &MBB && "Shrink-wrapping not yet supported");
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MoeMachineFunctionInfo *MoeFI = MF.getInfo<MoeMachineFunctionInfo>();

  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  uint64_t StackSize = MFI.getStackSize();
  uint64_t NumBytes = StackSize - MoeFI->getCalleeSavedFrameSize();

  // Skip the callee-saved PUSH instructions already inserted by
  // spillCalleeSavedRegisters.
  while (MBBI != MBB.end() && MBBI->getFlag(MachineInstr::FrameSetup) &&
         MBBI->getOpcode() == Moe::PUSH)
    ++MBBI;

  if (MBBI != MBB.end())
    DL = MBBI->getDebugLoc();

  if (NumBytes)
    adjustStackPointer(MBB, MBBI, DL, TII, MF, NumBytes,
                        MachineInstr::FrameSetup);

  // The frame pointer is SP as it stands here, once and for all: every local
  // is at a fixed offset from this point, whatever a later variable-sized
  // allocation does to SP.
  if (hasFP(MF))
    BuildMI(MBB, MBBI, DL, TII.get(Moe::MOVE), Moe::GP7)
        .addReg(Moe::SP)
        .setMIFlag(MachineInstr::FrameSetup);
}

void MoeFrameLowering::emitEpilogue(MachineFunction &MF,
                                     MachineBasicBlock &MBB) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  MoeMachineFunctionInfo *MoeFI = MF.getInfo<MoeMachineFunctionInfo>();

  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();

  uint64_t StackSize = MFI.getStackSize();
  unsigned CSSize = MoeFI->getCalleeSavedFrameSize();
  uint64_t NumBytes = StackSize - CSSize;

  // Skip back over the callee-saved POP instructions (and the RET pseudo)
  // to insert the SP restore before them.
  MachineBasicBlock::iterator FirstCSPop = MBBI;
  while (MBBI != MBB.begin()) {
    MachineBasicBlock::iterator PI = std::prev(MBBI);
    unsigned Opc = PI->getOpcode();
    if (Opc != Moe::POP && !PI->isTerminator())
      break;
    if (Opc == Moe::POP && !PI->getFlag(MachineInstr::FrameDestroy))
      break;
    FirstCSPop = PI;
    --MBBI;
  }
  MBBI = FirstCSPop;
  DL = MBBI->getDebugLoc();

  // With a frame pointer, SP is wherever the last variable-sized allocation
  // left it and no fixed amount describes the way back, so it is put back to
  // the frame pointer first - and then the frame is unwound exactly as it
  // would be without one, because the frame pointer is the BOTTOM of the
  // locals, not the top: the callee-saved registers this epilogue is about to
  // pop are above them.
  if (hasFP(MF))
    BuildMI(MBB, MBBI, DL, TII.get(Moe::MOVE), Moe::SP)
        .addReg(Moe::GP7)
        .setMIFlag(MachineInstr::FrameDestroy);

  if (NumBytes)
    adjustStackPointer(MBB, MBBI, DL, TII, MF, -(int64_t)NumBytes,
                        MachineInstr::FrameDestroy);
}

bool MoeFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  if (CSI.empty())
    return false;

  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  MachineFunction &MF = *MBB.getParent();
  MoeMachineFunctionInfo *MoeFI = MF.getInfo<MoeMachineFunctionInfo>();
  MoeFI->setCalleeSavedFrameSize(CSI.size() * 4);

  for (const CalleeSavedInfo &I : CSI) {
    MCRegister Reg = I.getReg();
    MBB.addLiveIn(Reg);
    BuildMI(MBB, MI, DL, TII.get(Moe::PUSH))
        .addReg(Reg, RegState::Kill)
        .setMIFlag(MachineInstr::FrameSetup);
  }
  return true;
}

bool MoeFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  if (CSI.empty())
    return false;

  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  for (const CalleeSavedInfo &I : llvm::reverse(CSI))
    BuildMI(MBB, MI, DL, TII.get(Moe::POP), I.getReg())
        .setMIFlag(MachineInstr::FrameDestroy);

  return true;
}

MachineBasicBlock::iterator MoeFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  if (!hasReservedCallFrame(MF)) {
    MachineInstr &Old = *I;
    bool IsDestroy = Old.getOpcode() == TII.getCallFrameDestroyOpcode();
    uint64_t Amount = TII.getFrameSize(Old);
    if (Amount != 0) {
      Amount = alignTo(Amount, getStackAlign());
      if (IsDestroy)
        Amount -= TII.getFramePoppedByCallee(Old);
      if (Amount != 0)
        adjustStackPointer(MBB, I, Old.getDebugLoc(), TII, MF,
                            IsDestroy ? -(int64_t)Amount : (int64_t)Amount,
                            MachineInstr::NoFlags);
    }
  }
  return MBB.erase(I);
}
