//===-- MoeMCInstLower.cpp - Convert Moe MachineInstr to an MCInst -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains code to lower Moe MachineInstrs to their corresponding
// MCInst records.
//
//===----------------------------------------------------------------------===//

#include "MoeMCInstLower.h"
#include "MoeShortForms.h"
#include "MCTargetDesc/MoeMCTargetDesc.h"
#include <cstdlib>
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

MCSymbol *
MoeMCInstLower::GetGlobalAddressSymbol(const MachineOperand &MO) const {
  return Printer.getSymbol(MO.getGlobal());
}

MCSymbol *
MoeMCInstLower::GetConstantPoolIndexSymbol(const MachineOperand &MO) const {
  const DataLayout &DL = Printer.getDataLayout();
  SmallString<256> Name;
  raw_svector_ostream(Name) << DL.getPrivateGlobalPrefix() << "CPI"
                             << Printer.getFunctionNumber() << '_'
                             << MO.getIndex();
  return Ctx.getOrCreateSymbol(Name);
}

MCOperand MoeMCInstLower::LowerSymbolOperand(const MachineOperand &MO,
                                              MCSymbol *Sym) const {
  const MCExpr *Expr = MCSymbolRefExpr::create(Sym, Ctx);
  if (MO.getOffset())
    Expr = MCBinaryExpr::createAdd(
        Expr, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);
  return MCOperand::createExpr(Expr);
}

namespace {

// The rules for which instructions get a short form live in MoeShortForms.h,
// shared with MoeShortBranches - which has to predict the size of everything
// here in order to work out which branches reach, and would mispredict rather
// than fail to build if the two ever disagreed.

// The PC-relative twin of an instruction whose trailing operand is an absolute
// address, or -1 if there isn't one.
//
// These five are every way compiled code names an address: a branch to one of
// this function's own blocks (JMP/JCC), a call to another function (JMPabs),
// and a read or write of a constant-pool word (LOADabs/STOREabs). In
// position-independent code all five become displacements from IA, which is
// what keeps a PIE's relocations out of .text - see the JMPpc/LOADpc defs in
// MoeInstrInfo.td.
int pcRelativeOpcode(unsigned Opc) {
  switch (Opc) {
  case Moe::JMP:       return Moe::JMPpc;
  case Moe::JCC:       return Moe::JCCpc;
  case Moe::JMPabs:    return Moe::JMPabs_pc;
  case Moe::LOADabs:   return Moe::LOADpc;
  case Moe::STOREabs:  return Moe::STOREpc;
  default:             return -1;
  }
}

} // end anonymous namespace

void MoeMCInstLower::Lower(const MachineInstr *MI, MCInst &OutMI) const {
  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    switch (MO.getType()) {
    default:
      MI->print(errs());
      llvm_unreachable("unknown operand type");
    case MachineOperand::MO_Register:
      if (MO.isImplicit())
        continue;
      MCOp = MCOperand::createReg(MO.getReg());
      break;
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::createImm(MO.getImm());
      break;
    case MachineOperand::MO_MachineBasicBlock:
      MCOp = MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), Ctx));
      break;
    case MachineOperand::MO_MCSymbol:
      MCOp = LowerSymbolOperand(MO, MO.getMCSymbol());
      break;
    case MachineOperand::MO_GlobalAddress:
      MCOp = LowerSymbolOperand(MO, GetGlobalAddressSymbol(MO));
      break;
    case MachineOperand::MO_ExternalSymbol:
      // A libcall's callee (e.g. __addsf3, referenced by name rather than a
      // GlobalValue* - see LowerCall's matching ExternalSymbolSDNode case
      // and the Milestone 8 plan).
      MCOp = LowerSymbolOperand(
          MO, Printer.GetExternalSymbolSymbol(MO.getSymbolName()));
      break;
    case MachineOperand::MO_ConstantPoolIndex:
      MCOp = LowerSymbolOperand(MO, GetConstantPoolIndexSymbol(MO));
      break;
    case MachineOperand::MO_RegisterMask:
      continue;
    }

    OutMI.addOperand(MCOp);
  }

  // Pick the short addressing form when the offset fits, which is the whole of
  // candidate S9's register-indirect half (isa-evaluation.md). This is done
  // here rather than during instruction selection because the choice is not a
  // semantic one: the two forms compute the same address and differ only in
  // encoding, and the offset is not necessarily a constant until frame-index
  // elimination has run - which it has by now, and has not during ISel.
  //
  // A symbolic offset is left long. It cannot be range-checked here, and there
  // is no relocation that writes a shifted 12-bit field, so the long form is
  // the only one that can be emitted honestly.
  //
  // MOE_NO_SHORT_FORM turns the rewrite off. It exists because the only honest
  // way to price an encoding is to build the same source both ways and run
  // both, and the alternative - comparing against a measurement taken before
  // some other commit - compares two things that differ in more than one way.
  // It found something no static model would have: the short form takes 18% off
  // the kernel's text and 2.5% off its instruction T-states, and *costs* 3.4% of
  // a boot on the machine as built, because the denser layout aliases worse in a
  // 32-entry direct-mapped TLB. See isa-evaluation.md's S9 finding.
  // Position-independent code reaches every address relative to IA instead of
  // naming it. Done here, at the last possible moment, for the same reason the
  // short-form rewrite is: the two forms compute the same address and cost the
  // same, so the choice carries no information any earlier pass needs. Nothing
  // about register allocation, scheduling or block layout changes.
  //
  // The gate is the function's own relocation model rather than a subtarget
  // feature, so a PIC and a non-PIC translation unit can be compiled by the
  // same compiler and linked together - which is what building a libc both
  // ways requires.
  if (Printer.TM.isPositionIndependent()) {
    int PcRelOpc = pcRelativeOpcode(OutMI.getOpcode());
    if (PcRelOpc >= 0)
      OutMI.setOpcode(PcRelOpc);
  }

  int ShortOpc = Moe::shortRegisterIndirectOpcode(OutMI.getOpcode());
  if (ShortOpc >= 0 && !Moe::shortFormsDisabled()) {
    int Base = Moe::memBaseOperand(OutMI.getOpcode());
    if (Base >= 0 && Base + 1 < (int)OutMI.getNumOperands()) {
      const MCOperand &Offset = OutMI.getOperand(Base + 1);
      if (Offset.isImm() && Moe::fitsShortForm(Offset.getImm()))
        OutMI.setOpcode(ShortOpc);
    }
  }
}
