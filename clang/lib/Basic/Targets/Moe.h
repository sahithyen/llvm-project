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
    // No f64 soft-float runtime exists (llvm-tests/softfloat_f32.ll is
    // deliberately f32-only). Collapse double/long double onto float's
    // IEEE-single representation, matching AVR-GCC's own -mdouble=32
    // default for targets without hardware/software double-precision
    // support - and matching llvm/lib/TargetParser/TargetDataLayout.cpp's
    // pre-existing `case Triple::moe:` datalayout string, which already
    // bakes in f64:32. This is higher-value than it might look: an
    // unsuffixed float literal like `3.14` is `double` by default in C.
    DoubleWidth = 32;
    DoubleAlign = 32;
    DoubleFormat = &llvm::APFloat::IEEEsingle();
    LongDoubleWidth = 32;
    LongDoubleAlign = 32;
    LongDoubleFormat = &llvm::APFloat::IEEEsingle();

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
