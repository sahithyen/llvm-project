//===-- MoeFixupKinds.h - Moe-specific fixup entries ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Moe has exactly one target-specific fixup, and it exists for exactly one
// reason: position-independent code.
//
// Every relocatable reference this target emitted before it was a flat 32-bit
// absolute address (R_MOE_32), written into a trailing operand word whole. That
// is fine for a program linked at a fixed address and impossible for one that
// is not - and since JUMP/LOAD/STORE keep their operand *inside .text*, "fix it
// up at load time" would mean writable, relocated text.
//
// The way out is already in the ISA. A trailing operand in Register-indirect
// mode is a base register plus a signed offset, the base register may be IA,
// and IA at the time the operand resolves is the address just past it - so
// `[IA + (sym - .)]` is an ordinary PC-relative reference and needs no new
// addressing mode, no new opcode, and no extra instruction. What it does need
// is a relocation that writes a *shifted* field rather than a whole word,
// because the low four bits of that operand belong to the base register select.
// That is this fixup.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MCTARGETDESC_MOEFIXUPKINDS_H
#define LLVM_LIB_TARGET_MOE_MCTARGETDESC_MOEFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace Moe {

enum Fixups {
  /// A 28-bit signed PC-relative displacement in bits 31-4 of a
  /// Register-indirect trailing operand word whose base register select (bits
  /// 3-0) is IA. The value written is `S + A - P`, where P is the address of
  /// the operand word itself; the emitter supplies `A = -4` so that the result
  /// is measured from the *end* of the word, which is where the hardware's IA
  /// stands when the operand resolves (see resolve_trailing_operand in
  /// emulator/src/cpu.rs).
  fixup_moe_pcrel28 = FirstTargetFixupKind,

  /// The same field in a *short* form's halfword operand: a 12-bit signed
  /// PC-relative displacement in bits 15-4, over the same four bits of base
  /// register select. The addend is -2 rather than -4, because a short form's
  /// operand is two bytes and IA stands at its end.
  ///
  /// There is deliberately no relocation behind this one. A 12-bit field
  /// reaches +-2 KiB, which is a distance only within one function, so MC
  /// resolves every one of these during layout; anything that survived to the
  /// object file would be a reference the linker has no way to write. The ELF
  /// writer says so rather than inventing a relocation number.
  fixup_moe_pcrel12,

  fixup_moe_NumTargetFixupKinds
};

} // end namespace Moe
} // end namespace llvm

#endif
