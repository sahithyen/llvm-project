//===-- MoeMCTargetDesc.cpp - Moe Target Descriptions --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides Moe specific target descriptions.
//
//===----------------------------------------------------------------------===//

#include "MoeMCTargetDesc.h"
#include "MoeInstPrinter.h"
#include "MoeMCAsmInfo.h"
#include "TargetInfo/MoeTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "MoeGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "MoeGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "MoeGenRegisterInfo.inc"

static MCInstrInfo *createMoeMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitMoeMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createMoeMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  // IA is Moe's program-counter equivalent - the closest match for the
  // generic "return address register" DWARF/CFI parameter, which Milestone
  // 1 doesn't otherwise use (no CFI directives emitted yet).
  InitMoeMCRegisterInfo(X, Moe::IA);
  return X;
}

static MCAsmInfo *createMoeMCAsmInfo(const MCRegisterInfo &MRI,
                                     const Triple &TT,
                                     const MCTargetOptions &Options) {
  return new MoeMCAsmInfo(TT);
}

static MCSubtargetInfo *createMoeMCSubtargetInfo(const Triple &TT,
                                                  StringRef CPU,
                                                  StringRef FS) {
  return createMoeMCSubtargetInfoImpl(TT, CPU, /*TuneCPU*/ CPU, FS);
}

static MCInstPrinter *createMoeMCInstPrinter(const Triple &T,
                                              unsigned SyntaxVariant,
                                              const MCAsmInfo &MAI,
                                              const MCInstrInfo &MII,
                                              const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new MoeInstPrinter(MAI, MII, MRI);
  return nullptr;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMoeTargetMC() {
  Target &T = getTheMoeTarget();

  TargetRegistry::RegisterMCAsmInfo(T, createMoeMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createMoeMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createMoeMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createMoeMCSubtargetInfo);
  TargetRegistry::RegisterMCInstPrinter(T, createMoeMCInstPrinter);
}
