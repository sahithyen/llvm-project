//===-- MoeInstPrinter.cpp - Convert Moe MCInst to assembly syntax --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This class prints a Moe MCInst to a .s file.
//
//===----------------------------------------------------------------------===//

#include "MoeInstPrinter.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/ErrorHandling.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

// Include the auto-generated portion of the assembly writer.
#define PRINT_ALIAS_INSTR
#include "MoeGenAsmWriter.inc"

void MoeInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg);
}

void MoeInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                StringRef Annot, const MCSubtargetInfo &STI,
                                raw_ostream &O) {
  if (!printAliasInstr(MI, Address, O))
    printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void MoeInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                   raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    O << getRegisterName(Op.getReg());
  } else if (Op.isImm()) {
    O << Op.getImm();
  } else {
    assert(Op.isExpr() && "unknown operand kind in printOperand");
    MAI.printExpr(O, *Op.getExpr());
  }
}

// moemem is (base register, offset immediate) - see 'Addressing mode' in
// the Encoding chapter's Register-indirect description.
void MoeInstPrinter::printMemOperand(const MCInst *MI, unsigned OpNo,
                                       raw_ostream &O) {
  const MCOperand &Base = MI->getOperand(OpNo);
  const MCOperand &Offset = MI->getOperand(OpNo + 1);
  O << "[" << getRegisterName(Base.getReg()) << "+" << Offset.getImm() << "]";
}

// Matches the numbering and naming in 'Condition selection' in the Encoding
// chapter, and emulator/src/disassembler.rs's condition_name(), for
// consistency across the project.
void MoeInstPrinter::printCCOperand(const MCInst *MI, unsigned OpNo,
                                     raw_ostream &O) {
  unsigned CC = MI->getOperand(OpNo).getImm();
  switch (CC) {
  default:
    llvm_unreachable("Unsupported condition code");
  case 0:
    O << "AL";
    break;
  case 1:
    O << "NV";
    break;
  case 2:
    O << "EQ";
    break;
  case 3:
    O << "NE";
    break;
  case 4:
    O << "CS";
    break;
  case 5:
    O << "CC";
    break;
  case 6:
    O << "MI";
    break;
  case 7:
    O << "PL";
    break;
  case 8:
    O << "VS";
    break;
  case 9:
    O << "VC";
    break;
  case 10:
    O << "HI";
    break;
  case 11:
    O << "LS";
    break;
  case 12:
    O << "GE";
    break;
  case 13:
    O << "LT";
    break;
  case 14:
    O << "GT";
    break;
  case 15:
    O << "LE";
    break;
  }
}
