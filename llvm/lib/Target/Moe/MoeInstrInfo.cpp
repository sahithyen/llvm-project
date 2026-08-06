//===-- MoeInstrInfo.cpp - Moe Instruction Information --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the Moe implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "MoeInstrInfo.h"
#include "Moe.h"
#include "MoeSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "MoeGenInstrInfo.inc"

void MoeInstrInfo::anchor() {}

MoeInstrInfo::MoeInstrInfo(const MoeSubtarget &STI)
    : MoeGenInstrInfo(STI, RI, Moe::ADJCALLSTACKDOWN, Moe::ADJCALLSTACKUP),
      RI() {}

bool MoeInstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  // RET must still look like a real `isReturn` terminator when
  // PrologEpilogInserter runs (it's what tells PEI this block needs an
  // epilogue) - so its POP/MOVE-to-IA expansion happens here, in the
  // standard post-RA pseudo-expansion pass, which runs after PEI, rather
  // than via a custom inserter during ISel (which would erase that
  // information before PEI ever sees it).
  if (MI.getOpcode() != Moe::RET)
    return false;

  MachineBasicBlock &MBB = *MI.getParent();
  DebugLoc DL = MI.getDebugLoc();

  BuildMI(MBB, MI, DL, get(Moe::POP), Moe::GP4);
  MachineInstrBuilder MIB =
      BuildMI(MBB, MI, DL, get(Moe::MOVE), Moe::IA)
          .addReg(Moe::GP4, RegState::Kill);
  // Preserve RET's implicit return-value register uses (e.g. $gp0) so they
  // stay live through to this final instruction instead of looking dead.
  for (unsigned i = 0, e = MI.getNumOperands(); i != e; ++i)
    MIB.add(MI.getOperand(i));

  MI.eraseFromParent();
  return true;
}

void MoeInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator I,
                                const DebugLoc &DL, Register DestReg,
                                Register SrcReg, bool KillSrc,
                                bool RenamableDest, bool RenamableSrc) const {
  assert(Moe::AllRegRegClass.contains(DestReg, SrcReg) &&
         "Impossible reg-to-reg copy");
  BuildMI(MBB, I, DL, get(Moe::MOVE), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

void MoeInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator MI,
                                        Register SrcReg, bool isKill,
                                        int FrameIndex,
                                        const TargetRegisterClass *RC,
                                        Register VReg,
                                        MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIndex),
      MachineMemOperand::MOStore, MFI.getObjectSize(FrameIndex),
      MFI.getObjectAlign(FrameIndex));

  assert(RC == &Moe::GPRRegClass && "Cannot store this register class to stack slot!");
  // STORErr's ins list is (GPR:$reg, moemem:$addr) - register operand
  // first, then the base/offset pair.
  BuildMI(MBB, MI, DL, get(Moe::STORErr))
      .addReg(SrcReg, getKillRegState(isKill))
      .addFrameIndex(FrameIndex)
      .addImm(0)
      .addMemOperand(MMO);
}

void MoeInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator MI,
                                         Register DestReg, int FrameIdx,
                                         const TargetRegisterClass *RC,
                                         Register VReg, unsigned SubReg,
                                         MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOLoad, MFI.getObjectSize(FrameIdx),
      MFI.getObjectAlign(FrameIdx));

  assert(RC == &Moe::GPRRegClass && "Cannot load this register class from stack slot!");
  BuildMI(MBB, MI, DL, get(Moe::LOADrr), DestReg)
      .addFrameIndex(FrameIdx)
      .addImm(0)
      .addMemOperand(MMO);
}

bool MoeInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Invalid Moe branch condition!");
  // Every one of Moe's 16 conditions is paired with its logical negation at
  // an adjacent encoding (AL/NV, EQ/NE, CS/CC, ...) - see 'Condition
  // selection' in the Encoding chapter - so reversing is just flipping the
  // low bit.
  Cond[0].setImm(Cond[0].getImm() ^ 1);
  return false;
}

bool MoeInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                  MachineBasicBlock *&TBB,
                                  MachineBasicBlock *&FBB,
                                  SmallVectorImpl<MachineOperand> &Cond,
                                  bool AllowModify) const {
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;

    if (!isUnpredicatedTerminator(*I))
      break;

    if (!I->isBranch())
      return true;

    if (I->getOpcode() == Moe::JMP) {
      if (!AllowModify) {
        TBB = I->getOperand(0).getMBB();
        continue;
      }

      MBB.erase(std::next(I), MBB.end());
      Cond.clear();
      FBB = nullptr;

      if (MBB.isLayoutSuccessor(I->getOperand(0).getMBB())) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }

      TBB = I->getOperand(0).getMBB();
      continue;
    }

    assert(I->getOpcode() == Moe::JCC && "Invalid conditional branch");
    int BranchCode = I->getOperand(1).getImm();

    if (Cond.empty()) {
      FBB = TBB;
      TBB = I->getOperand(0).getMBB();
      Cond.push_back(MachineOperand::CreateImm(BranchCode));
      continue;
    }

    assert(Cond.size() == 1);
    assert(TBB);

    if (TBB != I->getOperand(0).getMBB())
      return true;

    int OldBranchCode = Cond[0].getImm();
    if (OldBranchCode == BranchCode)
      continue;

    return true;
  }

  return false;
}

unsigned MoeInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                     int *BytesRemoved) const {
  assert(!BytesRemoved && "code size not handled");

  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;

  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != Moe::JMP && I->getOpcode() != Moe::JCC)
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }

  return Count;
}

unsigned MoeInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                     MachineBasicBlock *TBB,
                                     MachineBasicBlock *FBB,
                                     ArrayRef<MachineOperand> Cond,
                                     const DebugLoc &DL,
                                     int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 1 || Cond.size() == 0) &&
         "Moe branch conditions have one component!");
  assert(!BytesAdded && "code size not handled");

  if (Cond.empty()) {
    assert(!FBB && "Unconditional branch with multiple successors!");
    BuildMI(&MBB, DL, get(Moe::JMP)).addMBB(TBB);
    return 1;
  }

  unsigned Count = 0;
  BuildMI(&MBB, DL, get(Moe::JCC)).addMBB(TBB).addImm(Cond[0].getImm());
  ++Count;

  if (FBB) {
    BuildMI(&MBB, DL, get(Moe::JMP)).addMBB(FBB);
    ++Count;
  }
  return Count;
}
