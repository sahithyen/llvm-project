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

namespace llvm {
class Target;

// Milestone 1 is scoped to `llc -filetype=asm` (assembly text) only, so no
// MCCodeEmitter/MCAsmBackend/ELFObjectWriter exist yet - real binary
// encoding (needed for -filetype=obj) is deferred to a later milestone, see
// the Milestone 1 plan's MC-layer notes.

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
