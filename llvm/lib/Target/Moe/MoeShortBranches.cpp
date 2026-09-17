//===-- MoeShortBranches.cpp - Pick the short form for branches that reach ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A branch to one of this function's own blocks can be four bytes instead of
// eight: Addressing mode 3 with IA as the base register is a PC-relative
// reference with a twelve-bit signed displacement, and the whole instruction is
// then one aligned word - one memory access and three T-states rather than two
// and four. This is the Moe ISA evaluation's N1, and it costs no opcode, no
// addressing mode and no hardware: all three already exist.
//
// **Why this is a MachineFunction pass rather than a case in MoeMCInstLower,
// where the short LOAD/STORE forms are chosen.** That rewrite asks whether a
// constant fits twelve bits, and by the time it runs the offset *is* a
// constant. A branch's displacement is a symbol difference that nothing knows
// until the section is laid out, and MC cannot choose the form for us either:
// relaxation grows instructions, and the long form is the bigger one, so the
// only direction that would help is the one relaxation cannot express - and the
// long form's trailing word is emitted by MoeELFStreamer rather than by the
// code emitter, so a relaxed instruction would lose it.
//
// So this pass predicts the layout. It walks the function computing each
// instruction's size at its own offset, rewrites the branches whose target it
// can prove is in range, and repeats - because shortening one branch pulls
// every later block closer and can bring another into range. It only ever
// shrinks, so it terminates.
//
// **Predicting a layout is a model, and models are wrong.** Two things keep
// that from turning into a branch to the wrong address:
//
//  - Every size this pass computes is an upper bound on the real one, with one
//    exception, and the exception is handled separately. Where the pass cannot
//    know a size exactly it takes the largest it could be, and stops believing
//    it knows the alignment of anything afterwards, charging every later
//    trailing-operand instruction its worst case. Overestimating sizes can only
//    overestimate a distance, in either direction, so that alone can only make
//    the pass decline a branch it could have shortened.
//
//    The exception is inline assembly, whose size is a *guess* rather than a
//    bound - `.space 8192` is one statement and eight thousand bytes. So no
//    branch is shortened whose span contains any: a guess outside the span
//    shifts the branch and its target together and is covered by the bound
//    above, while a guess inside it is the one thing that could make a real
//    displacement larger than the predicted one.
//  - MoeAsmBackend range-checks fixup_moe_pcrel12 against the real displacement
//    and reports an error if it does not fit. A mistake here is a build that
//    fails loudly, not a program that jumps somewhere plausible.
//
//===----------------------------------------------------------------------===//

#include "Moe.h"
#include "MoeShortForms.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "moe-short-branches"

STATISTIC(NumShortened, "Number of branches given the short PC-relative form");

namespace {

/// How many bytes a short form occupies: the opcode halfword and its own
/// operand halfword, with no padding between them and no alignment of its own.
constexpr unsigned ShortFormSize = 4;
/// And a long one, at a word-aligned address: opcode, two bytes of padding,
/// then the operand word. Two bytes less at a halfword-aligned address, where
/// the operand word is already aligned without padding.
constexpr unsigned LongFormSize = 8;
/// Everything else is a bare opcode halfword.
constexpr unsigned OpcodeSize = 2;

/// The three opcodes that carry a trailing operand, in every form that is not
/// already short. A short one is sized by its own def (Size = 4).
bool hasLongTrailingOperand(unsigned Opc) {
  switch (Opc) {
  case Moe::JMP:
  case Moe::JCC:
  case Moe::JMPabs:
  case Moe::JMPpc:
  case Moe::JCCpc:
  case Moe::JMPabs_pc:
  case Moe::LOADabs:
  case Moe::STOREabs:
  case Moe::LOADabs_A:
  case Moe::STOREabs_A:
  case Moe::LOADpc:
  case Moe::STOREpc:
  case Moe::LOADrr:
  case Moe::STORErr:
  case Moe::LOADrr_A:
  case Moe::STORErr_A:
  case Moe::LOADrr_B:
  case Moe::LOADrr_H:
  case Moe::STORErr_B:
  case Moe::STORErr_H:
    return true;
  default:
    return false;
  }
}

bool isShortForm(unsigned Opc) {
  switch (Opc) {
  case Moe::LOADrr_S:
  case Moe::STORErr_S:
  case Moe::LOADrr_AS:
  case Moe::STORErr_AS:
  case Moe::LOADrr_BS:
  case Moe::LOADrr_HS:
  case Moe::STORErr_BS:
  case Moe::STORErr_HS:
  case Moe::JMPpc_S:
  case Moe::JCCpc_S:
    return true;
  default:
    return false;
  }
}

class MoeShortBranches : public MachineFunctionPass {
public:
  static char ID;
  MoeShortBranches() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return "Moe short PC-relative branches";
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

private:
  /// The block offsets this iteration computed, indexed by block number, and
  /// whether they are exact rather than upper bounds.
  SmallVector<unsigned, 16> BlockOffset;
  bool OffsetsExact = true;
  /// Where this function's inline assembly sits, by offset, in order. A branch
  /// whose span crosses one of these keeps the long form however close its
  /// target looks - see this file's header.
  SmallVector<unsigned, 4> UnsizedAt;

  const MCAsmInfo *MAI = nullptr;

  unsigned sizeOf(const MachineInstr &MI, unsigned Offset);
  void computeOffsets(MachineFunction &MF);
};

char MoeShortBranches::ID = 0;

/// What the assembler will make of one MachineInstr, in bytes, if it is placed
/// at `Offset` from the start of a (4-aligned) function. Never smaller than the
/// truth - see this file's header for why that direction is the load-bearing
/// one.
unsigned MoeShortBranches::sizeOf(const MachineInstr &MI, unsigned Offset) {
  if (MI.isMetaInstruction())
    return 0; // labels, KILL, IMPLICIT_DEF, CFI, debug values: no bytes

  if (MI.isInlineAsm()) {
    // The same estimate AsmPrinter makes, and for the same reason it is only an
    // estimate: the text is not parsed until it is emitted. Statements are
    // separated by newlines or by MCAsmInfo's separator (a backtick here, since
    // ';' is the comment character), and each is charged the longest
    // instruction this target has. After this, nothing downstream is known to
    // sit where this pass thinks it does.
    OffsetsExact = false;
    UnsizedAt.push_back(Offset);
    const char *Asm = MI.getOperand(0).getSymbolName();
    unsigned Statements = 1;
    const char Separator = MAI ? MAI->getSeparatorString()[0] : ';';
    for (const char *P = Asm; *P; ++P)
      if (*P == '\n' || *P == Separator)
        ++Statements;
    return Statements * (MAI ? MAI->getMaxInstLength() : LongFormSize);
  }

  unsigned Opc = MI.getOpcode();
  if (isShortForm(Opc))
    return ShortFormSize;

  if (hasLongTrailingOperand(Opc)) {
    // MoeMCInstLower will shorten a register-indirect LOAD/STORE whose offset
    // is a constant that fits - which is most of what -O0 emits, so guessing
    // here rather than asking the same predicate would put every later block
    // in the wrong place. See MoeShortForms.h.
    int ShortOpc = Moe::shortRegisterIndirectOpcode(Opc);
    if (ShortOpc >= 0 && !Moe::shortFormsDisabled()) {
      int Base = Moe::memBaseOperand(Opc);
      if (Base >= 0 && Base + 1 < (int)MI.getNumOperands()) {
        const MachineOperand &Off = MI.getOperand(Base + 1);
        if (Off.isImm() && Moe::fitsShortForm(Off.getImm()))
          return ShortFormSize;
      }
    }
    // A long form's operand word goes at the next word-aligned address, so the
    // instruction is six bytes when it starts two bytes into a word and eight
    // when it starts on one. Six is only claimable when the offset is known
    // exactly; otherwise the worst case, which is also what keeps this an
    // upper bound.
    if (OffsetsExact && (Offset % 4) == 2)
      return LongFormSize - 2;
    return LongFormSize;
  }

  return OpcodeSize;
}

void MoeShortBranches::computeOffsets(MachineFunction &MF) {
  BlockOffset.assign(MF.getNumBlockIDs(), 0);
  OffsetsExact = true;
  UnsizedAt.clear();
  unsigned Offset = 0;
  for (const MachineBasicBlock &MBB : MF) {
    // alignTo is monotonic, so aligning an upper bound leaves an upper bound.
    Offset = alignTo(Offset, MBB.getAlignment());
    BlockOffset[MBB.getNumber()] = Offset;
    for (const MachineInstr &MI : MBB)
      Offset += sizeOf(MI, Offset);
  }
}

bool MoeShortBranches::runOnMachineFunction(MachineFunction &MF) {
  if (Moe::shortBranchesDisabled())
    return false;

  MAI = MF.getTarget().getMCAsmInfo();

  bool Changed = false;
  // Shortening only ever moves blocks closer together, so each pass can only
  // add candidates and the loop is bounded by the number of branches. The cap
  // is belt and braces: a fixed point is normally reached in two or three.
  for (unsigned Round = 0; Round != 8; ++Round) {
    computeOffsets(MF);
    bool ShortenedThisRound = false;

    for (MachineBasicBlock &MBB : MF) {
      unsigned Offset = BlockOffset[MBB.getNumber()];
      for (MachineInstr &MI : MBB) {
        unsigned Size = sizeOf(MI, Offset);
        int ShortOpc = Moe::shortBranchOpcode(MI.getOpcode());
        if (ShortOpc >= 0 && MI.getOperand(0).isMBB()) {
          // The displacement the hardware adds to IA, and IA stands at the end
          // of the operand halfword - so it is measured from four bytes past
          // the branch's own address, which is what the short form would
          // occupy. MoeELFStreamer's -2 addend puts the same number in the
          // fixup, from the operand's own address.
          const MachineBasicBlock *Target = MI.getOperand(0).getMBB();
          unsigned TargetOffset = BlockOffset[Target->getNumber()];
          int64_t Displacement =
              (int64_t)TargetOffset - ((int64_t)Offset + ShortFormSize);
          unsigned Lo = std::min(Offset, TargetOffset);
          unsigned Hi = std::max(Offset, TargetOffset);
          bool CrossesGuess = llvm::any_of(UnsizedAt, [&](unsigned At) {
            return At >= Lo && At < Hi;
          });
          if (Moe::fitsShortForm(Displacement) && !CrossesGuess) {
            LLVM_DEBUG(dbgs() << "moe-short-branches: " << MI
                              << "  displacement " << Displacement << "\n");
            MI.setDesc(MF.getSubtarget().getInstrInfo()->get(ShortOpc));
            ++NumShortened;
            Changed = ShortenedThisRound = true;
            Size = ShortFormSize;
          }
        }
        Offset += Size;
      }
    }

    if (!ShortenedThisRound)
      break;
  }

  return Changed;
}

} // end anonymous namespace

INITIALIZE_PASS(MoeShortBranches, DEBUG_TYPE,
                "Moe short PC-relative branches", false, false)

FunctionPass *llvm::createMoeShortBranchesPass() {
  return new MoeShortBranches();
}
