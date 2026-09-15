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
#include "MoeConstantPoolValue.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/MC/MCSymbol.h"
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

// Expands the CALL/CALLreg pseudo into the real call sequence:
//
//   LOAD.W  [pool entry holding the continuation's label] -> GP4
//   PUSH.W  GP4
//   JUMP.AL <callee>            (or MOVE <callee> -> IA, indirect)
//
// This happens HERE, after register allocation, and that timing is the whole
// point rather than an implementation detail.
//
// Moe has no call instruction, so a call is three instructions with a
// mid-sequence PUSH - and everything between that PUSH and the jump sees a
// stack pointer four bytes lower than the frame offsets were computed against.
// While the sequence existed as three separate instructions during register
// allocation, the allocator was free to insert spill and reload code into the
// middle of it, and it did: at -O2 it placed a reload of a callee-saved
// register AFTER the jump, in the gap between the jump and the continuation
// block's label, where nothing ever executes.
//
// That is the same failure the mid-block EH_LABEL design produced at -O0 years
// earlier (see MoeISelLowering::emitCall), and making the continuation a real
// MachineBasicBlock fixed it only for RegAllocFast, whose reloadAtBegin puts
// reloads at the start of the successor. Greedy places them at the end of the
// predecessor, which for a block ending in a jump-that-is-not-a-terminator is
// dead code.
//
// Found booting Linux: parse_args' `args` pointer came back holding the
// callback's own address, because the reload restoring it sat after the jump.
// The fix is to stop offering the middle of a call as a place to put anything:
// as one MachineInstr, the allocator can only put code before the whole
// sequence (where SP is still where the frame offsets expect it) or at the
// start of the continuation block.
bool MoeInstrInfo::expandCall(MachineInstr &MI) const {
  MachineBasicBlock &MBB = *MI.getParent();
  MachineFunction &MF = *MBB.getParent();
  DebugLoc DL = MI.getDebugLoc();
  bool IsIndirect = MI.getOpcode() == Moe::CALLSEQreg;
  (void)MF;

  // Operand 1 is the constant-pool slot holding the continuation label's
  // address, put there by emitCall. LOADabs dereferences its trailing address
  // operand, so reading the label's address as a VALUE needs exactly this
  // indirection - the same one every other constant goes through.
  BuildMI(MBB, MI, DL, get(Moe::LOADabs), Moe::GP4).add(MI.getOperand(1));
  BuildMI(MBB, MI, DL, get(Moe::PUSH)).addReg(Moe::GP4, RegState::Kill);

  MachineInstrBuilder MIB =
      IsIndirect ? BuildMI(MBB, MI, DL, get(Moe::MOVE), Moe::IA)
                       .addReg(MI.getOperand(0).getReg())
                 : BuildMI(MBB, MI, DL, get(Moe::JMPabs)).add(MI.getOperand(0));

  // Carry over the implicit argument-register uses and clobbers so they stay
  // live to the jump rather than looking dead from here on.
  for (unsigned i = 2, e = MI.getNumOperands(); i != e; ++i)
    if (MI.getOperand(i).isReg() && MI.getOperand(i).isImplicit())
      MIB.add(MI.getOperand(i));

  MI.eraseFromParent();
  return true;
}

bool MoeInstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  if (MI.getOpcode() == Moe::CALLSEQ || MI.getOpcode() == Moe::CALLSEQreg)
    return expandCall(MI);

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
