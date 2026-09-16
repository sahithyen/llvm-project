//===-- MoeAsmPrinter.cpp - Moe LLVM assembly writer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal
// representation of machine-dependent LLVM code to the Moe assembly
// language.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MoeInstPrinter.h"
#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "MoeConstantPoolValue.h"
#include "MoeMCInstLower.h"
#include "MoeTargetMachine.h"
#include "TargetInfo/MoeTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {
class MoeAsmPrinter : public AsmPrinter {
public:
  MoeAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "Moe Assembly Printer"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void emitInstruction(const MachineInstr *MI) override;
  void emitMachineConstantPoolValue(MachineConstantPoolValue *MCPV) override;

  // Inline asm (Milestone 15): AsmPrinter::PrintAsmOperand's default
  // (no-ExtraCode) implementation unconditionally fails ("Targets should
  // override this" - see its own comment) - every target that supports
  // inline asm needs this, modeled directly on MSP430AsmPrinter's
  // identically-shaped override. Register operands only, matching
  // MoeTargetLowering::getRegForInlineAsmConstraint's "r"-only scope.
  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &O) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &O) override;
  void emitBasicBlockStart(const MachineBasicBlock &MBB) override;
  void printOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O);

  static char ID;

private:
  // The target-specific constant-pool entries this function actually reads.
  // See runOnMachineFunction for why it has to be computed at all.
  SmallPtrSet<const MachineConstantPoolValue *, 8> ReferencedCPVs;
};
} // end anonymous namespace

bool MoeAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                     const char *ExtraCode, raw_ostream &O) {
  if (ExtraCode && ExtraCode[0])
    return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, O);
  printOperand(MI, OpNo, O);
  return false;
}

// The printing half of the "m" constraint - see
// MoeDAGToDAGISel::SelectInlineAsmMemoryOperand, which produces the (base
// register, offset) pair this prints. The syntax has to be exactly what
// MoeInstPrinter::printMemOperand emits and what MoeAsmParser accepts, since
// the text goes straight back through the integrated assembler.
bool MoeAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                                           const char *ExtraCode,
                                           raw_ostream &O) {
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.
  const MachineOperand &Base = MI->getOperand(OpNo);
  const MachineOperand &Disp = MI->getOperand(OpNo + 1);
  if (!Base.isReg() || !Disp.isImm())
    return true;
  O << "[" << MoeInstPrinter::getRegisterName(Base.getReg()) << "+"
    << Disp.getImm() << "]";
  return false;
}

void MoeAsmPrinter::printOperand(const MachineInstr *MI, unsigned OpNo,
                                  raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    O << MoeInstPrinter::getRegisterName(MO.getReg());
    break;
  case MachineOperand::MO_Immediate:
    O << MO.getImm();
    break;
  default:
    llvm_unreachable("unexpected inline asm operand kind");
  }
}

// A call's continuation block is reachable at run time and unreachable in the
// CFG, and those two facts have to be reconciled here.
//
// Moe has no call instruction, so MoeISelLowering::emitCall builds one: it puts
// the continuation block's address in a constant pool entry, pushes it, and
// jumps. The callee's return sequence pops that address and jumps to it - so
// the block genuinely is entered, just not by any edge a machine pass can see.
// emitCall marks it address-taken, which stops the passes deleting it, and
// marks its label must-be-emitted.
//
// That is not quite enough. Nothing branches to the block, so the passes are
// entitled to prune the fall-through edge into it - and once they do,
// AsmPrinter::shouldEmitLabelForBasicBlock declines to emit the label on the
// grounds that the block has no predecessors, BEFORE it ever consults
// hasLabelMustBeEmitted. The constant pool is then left referencing a symbol
// that is never defined, and the assembler says so: "Undefined temporary symbol
// .LBBn_m". Found building lib/math/int_log.c, at -O1 and above; the existing
// -O1/-O2 regression tests did not hit it because their calls sit in blocks
// whose successor edge survives.
//
// The condition below is the exact complement of the one generic case that
// declines: no predecessors. Spelling it out rather than calling
// shouldEmitLabelForBasicBlock - which is private - means it has to stay in
// step with that function by hand, so it is written as the narrowest thing
// that closes the gap: every other reason the generic code has for emitting a
// label is excluded here, and so the label is emitted exactly once either way.
void MoeAsmPrinter::emitBasicBlockStart(const MachineBasicBlock &MBB) {
  bool GenericWouldEmit =
      MBB.isBeginSection() || MF->getTarget().Options.BBAddrMap ||
      !MBB.pred_empty();

  if (MBB.hasLabelMustBeEmitted() && !MBB.isEntryBlock() && !GenericWouldEmit)
    OutStreamer->emitLabel(MBB.getSymbol());

  AsmPrinter::emitBasicBlockStart(MBB);
}

// No emitFunctionBodyStart override: the trailing-operand word's alignment
// padding used to be computed from a per-function byte counter reset here,
// and is now MC layout's job (see MoeELFStreamer.cpp).

void MoeAsmPrinter::emitInstruction(const MachineInstr *MI) {
  MoeMCInstLower MCInstLowering(OutContext, *this);

  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

// Which constant-pool entries this function still reads.
//
// A call's continuation label lives in a pool entry (emitCall), and a pool
// entry is never removed once created: nothing deletes a MachineConstantPool
// entry when the instruction that referenced it goes away, and for an
// ordinary pooled number that costs nothing but a dead word. For one of these
// it is fatal. If the call is deleted - because optimization proved its block
// unreachable, say - the continuation block goes with it, and the pool is
// left holding `.long .LBBn_m` for a label that is never emitted. The
// assembler then refuses the whole file with "Undefined temporary symbol".
//
// This was invisible at -O0, where nothing deletes a block, and at -O1/-O2 on
// everything in llvm-tests/ and the kernel, which never produced a call in a
// block that later became unreachable. Rust's `core` produces them constantly:
// a bounds check whose failure arm calls a panic function, in a copy of the
// function where the check has been proved to hold.
//
// So the pool is emitted against what the function still references. An entry
// nothing reads becomes a zero word - the pool's layout and every other
// entry's index are unchanged, which is what makes this safe to do this late.
bool MoeAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  ReferencedCPVs.clear();
  const std::vector<MachineConstantPoolEntry> &CPs =
      MF.getConstantPool()->getConstants();
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB)
      for (const MachineOperand &MO : MI.operands())
        if (MO.isCPI() && (unsigned)MO.getIndex() < CPs.size()) {
          const MachineConstantPoolEntry &E = CPs[MO.getIndex()];
          if (E.isMachineConstantPoolEntry())
            ReferencedCPVs.insert(E.Val.MachineCPVal);
        }
  // An entry that holds the address of another entry keeps that one alive
  // too: only the outer one is named by an instruction.
  bool Grew = true;
  while (Grew) {
    Grew = false;
    for (const MachineConstantPoolEntry &E : CPs) {
      if (!E.isMachineConstantPoolEntry())
        continue;
      auto *CPV = static_cast<MoeConstantPoolValue *>(E.Val.MachineCPVal);
      if (!ReferencedCPVs.count(CPV) || !CPV->isCPIRef())
        continue;
      const MachineConstantPoolEntry &Target = CPs[CPV->getCPIRef()];
      if (Target.isMachineConstantPoolEntry() &&
          ReferencedCPVs.insert(Target.Val.MachineCPVal).second)
        Grew = true;
    }
  }
  return AsmPrinter::runOnMachineFunction(MF);
}

void MoeAsmPrinter::emitMachineConstantPoolValue(
    MachineConstantPoolValue *MCPV) {
  auto *CPV = static_cast<MoeConstantPoolValue *>(MCPV);
  unsigned Size = getDataLayout().getTypeAllocSize(CPV->getType());

  // Dead, and possibly naming a label that no longer exists - see
  // runOnMachineFunction.
  if (!ReferencedCPVs.count(CPV)) {
    OutStreamer->AddComment("unreferenced");
    OutStreamer->emitIntValue(0, Size);
    return;
  }

  // Either a symbol, or the label of another pool entry - which only exists
  // here, at the point the AsmPrinter names it.
  const MCExpr *Expr = MCSymbolRefExpr::create(
      CPV->isCPIRef() ? GetCPISymbol(CPV->getCPIRef()) : CPV->getSymbol(),
      OutContext);
  OutStreamer->emitValue(Expr, Size);
}

char MoeAsmPrinter::ID = 0;

INITIALIZE_PASS(MoeAsmPrinter, "moe-asm-printer", "Moe Assembly Printer",
                false, false)

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMoeAsmPrinter() {
  RegisterAsmPrinter<MoeAsmPrinter> X(getTheMoeTarget());
}
