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
