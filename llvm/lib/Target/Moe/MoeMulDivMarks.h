//===-- MoeMulDivMarks.h - Pricing markers for candidate L8 -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MOE_MULDIV_MARKERS brackets every software multiply and divide with a marker
// instruction that `moe-emu --profile` counts between, so that candidate L8
// (hardware multiply/divide) can be priced from what the corpus actually runs.
//
// It exists because the thing being priced has no address to attribute: a
// 32-bit MUL/UDIV/SDIV is expanded *inline* into a loop (emitMul, emitDivRem,
// emitSDivRem), so a symbol-attributed profile cannot see it at all. L8's
// first bound, 7.87%, was three kernel helper functions and nothing else - no
// inline loop anywhere and no userspace at all.
//
// A marker is `MOVE r -> r` with a code in bits 8-11, where r holds the
// multiplier or divisor at a begin marker, so the profile can read the value
// each operation actually ran with. Bits 8-11 are reserved (see 'Reserved
// bits' in the Encoding chapter), so it is an architectural no-op, and the
// compiler otherwise always emits them as 0b1111, so no ordinary instruction is
// ever mistaken for one. It is not free: each is
// a MOVE's T-states, which the profile counts separately so that they can be
// taken back out. Off by default, and it must stay off in anything that is
// not a pricing build.
//
// The codes are the emulator's too (emulator/src/muldiv_marks.rs's KIND_NAMES),
// and nothing makes the two agree except that a disagreement reads as zero
// operations of some kind in a build that plainly has them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOEMULDIVMARKS_H
#define LLVM_LIB_TARGET_MOE_MOEMULDIVMARKS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include <cstdlib>

namespace llvm {
namespace MoeMulDivMark {

enum Code : unsigned {
  Mul32Begin = 1,
  Mul32End = 2,
  UDivRem32Begin = 3,
  UDivRem32End = 4,
  SDivRem32Begin = 5,
  SDivRem32End = 6,
  Mul64Begin = 7,
  Mul64End = 8,
  Div64Begin = 9,
  Div64End = 10,
  // The same 32-bit operations with a compile-time constant second operand:
  // what software could do better without hardware, and so what hardware
  // should not be credited with.
  Mul32KBegin = 11,
  Mul32KEnd = 12,
  UDivRem32KBegin = 13,
  UDivRem32KEnd = 14,
  // 15 is not a code: every ordinary MOVE carries 0b1111 in these bits.
};

inline bool enabled() {
  static const bool On = std::getenv("MOE_MULDIV_MARKERS") != nullptr;
  return On;
}

/// The begin code for a whole function that *is* a 64-bit multiply or divide
/// helper, or 0. Bracketed at its prologue and every epilogue rather than by
/// the operations inside it, because a hardware 32-bit instruction replaces
/// only part of what such a function does and the whole of it is what a
/// wider design would remove. The inline 32-bit loops inside are counted too,
/// nested; the profile reports both.
inline unsigned helperBegin(StringRef Name) {
  return StringSwitch<unsigned>(Name)
      // runtime/i64.ll, and Linux's own (lib/math/div64.c)
      .Case("__muldi3", Mul64Begin)
      .Cases("__udivdi3", "__umoddi3", "__divdi3", "__moddi3", Div64Begin)
      .Case("udivmod64", Div64Begin)
      .Cases("div_s64_rem", "div64_u64_rem", "div64_u64", "div64_s64",
             Div64Begin)
      .Cases("iter_div_u64_rem", "mul_u64_u64_div_u64", Div64Begin)
      .Default(0);
}

} // namespace MoeMulDivMark
} // namespace llvm

#endif
