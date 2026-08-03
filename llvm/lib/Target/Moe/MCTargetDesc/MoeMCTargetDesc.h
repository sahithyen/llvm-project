//===-- MoeMCTargetDesc.h - Moe Target Descriptions ------------*- C++ -*-===//
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

#ifndef LLVM_LIB_TARGET_MOE_MCTARGETDESC_MOEMCTARGETDESC_H
#define LLVM_LIB_TARGET_MOE_MCTARGETDESC_MOEMCTARGETDESC_H

#include "llvm/Support/DataTypes.h"
#include <memory>

namespace llvm {
class Target;
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;

/// Creates a machine code emitter for Moe - see MoeMCCodeEmitter.cpp.
MCCodeEmitter *createMoeMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx);

/// Resets an MCCodeEmitter's per-function trailing-operand-alignment byte
/// counter - called from MoeAsmPrinter::emitFunctionBodyStart, since every
/// function is guaranteed word-aligned (see MoeISelLowering's
/// setMinFunctionAlignment) but the encoder can't otherwise tell where one
/// function's byte stream ends and the next begins.
void resetMoeCodeEmitterOffset(MCCodeEmitter &MCE);

MCAsmBackend *createMoeMCAsmBackend(const Target &T, const MCSubtargetInfo &STI,
                                     const MCRegisterInfo &MRI,
                                     const MCTargetOptions &Options);

std::unique_ptr<MCObjectTargetWriter> createMoeELFObjectWriter();

} // namespace llvm

// Defines symbolic names for Moe registers.
#define GET_REGINFO_ENUM
#include "MoeGenRegisterInfo.inc"

// Defines symbolic names for the Moe instructions.
#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "MoeGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "MoeGenSubtargetInfo.inc"

#endif
