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
#include "MoeMCInstLower.h"
#include "MoeTargetMachine.h"
#include "TargetInfo/MoeTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
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

  static char ID;
};
} // end anonymous namespace

void MoeAsmPrinter::emitInstruction(const MachineInstr *MI) {
  MoeMCInstLower MCInstLowering(OutContext, *this);

  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

char MoeAsmPrinter::ID = 0;

INITIALIZE_PASS(MoeAsmPrinter, "moe-asm-printer", "Moe Assembly Printer",
                false, false)

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMoeAsmPrinter() {
  RegisterAsmPrinter<MoeAsmPrinter> X(getTheMoeTarget());
}
