//===--- Moe.h - Declare Moe target feature support ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares Moe TargetInfo objects.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_MOE_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_MOE_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY MoeTargetInfo : public TargetInfo {
  static const char *const GCCRegNames[];

public:
  MoeTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    TLSSupported = false;

    // Every Moe register (GP0-7 and friends) is a 32-bit word - see
    // specification/src/registers.md.
    IntWidth = 32;
    IntAlign = 32;
    LongWidth = 32;
    LongAlign = 32;
    LongLongWidth = 64;  // C-standard-mandated minimum - see the known
    LongLongAlign = 32;  // i64-arithmetic gap note below.
    PointerWidth = 32;
    PointerAlign = 32;
    SuitableAlign = 32;

    FloatWidth = 32;
    FloatAlign = 32;
    // Milestone 18: real IEEE754 binary64 double, backed by runtime/f64.ll's soft-double
    // library (see that file's header for the full story). Before this, DoubleFormat was
    // IEEEsingle() - collapsing every C `double` onto float's representation, matching
    // AVR-GCC's own -mdouble=32 default - which silently miscompiled any code assuming real
    // double precision (an unsuffixed float literal like `3.14` is `double` by default in
    // C), not merely omitted it: it compiled cleanly and ran, just with ~7 significant
    // decimal digits and a +-38 exponent range instead of ~15-17 digits and +-308. 32-bit
    // alignment (not the natural 64) matches LongLongAlign above and
    // llvm/lib/TargetParser/TargetDataLayout.cpp's pre-existing `case Triple::moe:`
    // datalayout string's `f64:32` - no register here is wider than 32 bits regardless of a
    // value's own type width, so nothing wider than a word is ever natively aligned (see that
    // datalayout case's own comment). `long double` is collapsed onto `double` (both
    // IEEEdouble()) rather than given a distinct wider format, matching this project's
    // general preference for the simplest option that satisfies the C standard's
    // sizeof(long double) >= sizeof(double) requirement over a third, unused, precision tier.
    DoubleWidth = 64;
    DoubleAlign = 32;
    DoubleFormat = &llvm::APFloat::IEEEdouble();
    LongDoubleWidth = 64;
    LongDoubleAlign = 32;
    LongDoubleFormat = &llvm::APFloat::IEEEdouble();

    SizeType = UnsignedInt;
    PtrDiffType = SignedInt;
    IntPtrType = SignedInt;
    IntMaxType = SignedLongLong;
    // Moe's `int` is already the full 32-bit word (unlike MSP430's 16-bit
    // int, which needs SignedLong here) - no need to widen.
    SigAtomicType = SignedInt;
    WIntType = SignedInt;

    resetDataLayout();
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {}; // No target-specific builtins yet.
  }

  bool allowsLargerPreferedTypeAlignment() const override { return false; }

  bool hasFeature(StringRef Feature) const override {
    return Feature == "moe";
  }

  ArrayRef<const char *> getGCCRegNames() const override;

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    // Moe's registers already have their real names (gp0..rf, from
    // MoeRegisterInfo.td) - unlike MSP430's numbered r0-r3, there's no
    // separate named alias to bridge.
    return {};
  }

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &info) const override {
    // The Moe backend has no InlineAsm handling at all - fail constraint
    // parsing cleanly rather than pretending to support something that
    // isn't there.
    return false;
  }

  std::string_view getClobbers() const override { return ""; }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    // Simplest model; MoeCallingConv.td has no variadic-call handling at
    // all yet, so this is untested, not a validated design.
    return TargetInfo::CharPtrBuiltinVaList;
  }
};

} // namespace targets
} // namespace clang
#endif // LLVM_CLANG_LIB_BASIC_TARGETS_MOE_H
