//===--- Moe.h - Moe-specific Tool Helpers ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MOE_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MOE_H

#include "Gnu.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Option.h"

namespace clang {
namespace driver {
namespace toolchains {

// Moe has no installed cross-toolchain (no moe-elf-gcc, no multilib, no
// sysroot) to discover, unlike MSP430ToolChain/AVRToolChain - modeled on
// LanaiToolChain's thin Generic_ELF subclassing instead (no
// GCCInstallation.init() call), but with a real buildLinker() override
// (unlike Lanai, which has none), since Moe needs a specific ld.lld
// invocation, not the Generic_GCC default host-linker behavior.
class LLVM_LIBRARY_VISIBILITY MoeToolChain : public Generic_ELF {
public:
  MoeToolChain(const Driver &D, const llvm::Triple &Triple,
               const llvm::opt::ArgList &Args)
      : Generic_ELF(D, Triple, Args) {}

  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                             llvm::opt::ArgStringList &CC1Args,
                             Action::OffloadKind) const override;

  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return true; }

  UnwindLibType
  GetUnwindLibType(const llvm::opt::ArgList &Args) const override {
    return UNW_None;
  }

  // No support for finding a C++ standard library yet.
  void addLibCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override {}
  void addLibStdCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override {}

protected:
  Tool *buildLinker() const override;
};

} // end namespace toolchains

namespace tools {
namespace moe {

// Invokes ld.lld directly with the exact flags every llvm-tests/run-*.sh
// script already hardcodes - see the Milestone 9 plan for why: no linker
// script, no crt0/libc auto-linking, no --gc-sections. Deliberately does
// not productionize any of that; it just gives Clang the same manual
// invocation this project's own test scripts already prove works.
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("Moe::Linker", "ld.lld", TC) {}
  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // end namespace moe
} // end namespace tools
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MOE_H
