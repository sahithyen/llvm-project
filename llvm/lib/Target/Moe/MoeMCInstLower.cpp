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
#include "MCTargetDesc/MoeMCTargetDesc.h"
#include <cstdlib>
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/DataLayout.h"
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

// The short twin of a register-indirect LOAD/STORE, or -1 if there isn't one.
//
// The two differ only in Addressing mode - bit 8 of the opcode halfword - and
// in whether the operand is a halfword after the opcode or a word at the next
// aligned address. Every operand list is identical, so the rewrite below is a
// change of opcode and nothing else.
int shortRegisterIndirectOpcode(unsigned Opc) {
  switch (Opc) {
  case Moe::LOADrr:     return Moe::LOADrr_S;
  case Moe::STORErr:    return Moe::STORErr_S;
  case Moe::LOADrr_A:   return Moe::LOADrr_AS;
  case Moe::STORErr_A:  return Moe::STORErr_AS;
  case Moe::LOADrr_B:   return Moe::LOADrr_BS;
  case Moe::LOADrr_H:   return Moe::LOADrr_HS;
  case Moe::STORErr_B:  return Moe::STORErr_BS;
  case Moe::STORErr_H:  return Moe::STORErr_HS;
  default:              return -1;
  }
}

// Which operand of a lowered register-indirect LOAD/STORE is the base
// register; its offset is the one after it. This mirrors
// MoeELFStreamer's own getTrailingOperand and for the same reason: LOADrr_B/H
// carry a tied $oldval ahead of the address, so their base is operand 2 while
// everything else's is operand 1.
int memBaseOperand(unsigned Opc) {
  switch (Opc) {
  case Moe::LOADrr_B:
  case Moe::LOADrr_H:
    return 2;
  case Moe::LOADrr:
  case Moe::STORErr:
  case Moe::LOADrr_A:
  case Moe::STORErr_A:
  case Moe::STORErr_B:
  case Moe::STORErr_H:
    return 1;
  default:
    return -1;
  }
}

// Twelve signed bits, which is what the halfword operand has left once four
// go to the base register - see 'Addressing mode' in the Encoding chapter.
bool offsetFitsShortForm(int64_t Offset) {
  return Offset >= -2048 && Offset <= 2047;
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
  int ShortOpc = shortRegisterIndirectOpcode(OutMI.getOpcode());
  static const bool Disabled = getenv("MOE_NO_SHORT_FORM") != nullptr;
  if (ShortOpc >= 0 && !Disabled) {
    int Base = memBaseOperand(OutMI.getOpcode());
    if (Base >= 0 && Base + 1 < (int)OutMI.getNumOperands()) {
      const MCOperand &Offset = OutMI.getOperand(Base + 1);
      if (Offset.isImm() && offsetFitsShortForm(Offset.getImm()))
        OutMI.setOpcode(ShortOpc);
    }
  }
}
