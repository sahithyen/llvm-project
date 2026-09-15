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

void MoeAsmPrinter::emitMachineConstantPoolValue(
    MachineConstantPoolValue *MCPV) {
  auto *CPV = static_cast<MoeConstantPoolValue *>(MCPV);
  const MCExpr *Expr = MCSymbolRefExpr::create(CPV->getSymbol(), OutContext);
  OutStreamer->emitValue(Expr, getDataLayout().getTypeAllocSize(CPV->getType()));
}

char MoeAsmPrinter::ID = 0;

INITIALIZE_PASS(MoeAsmPrinter, "moe-asm-printer", "Moe Assembly Printer",
                false, false)

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMoeAsmPrinter() {
  RegisterAsmPrinter<MoeAsmPrinter> X(getTheMoeTarget());
}
