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
      : Generic_ELF(D, Triple, Args) {
    // Where runtime/build.sh installs crt0.o/libmoe_f32.o, relative to
    // wherever this clang binary itself lives - not a hardcoded absolute
    // repo path. Standard ToolChain idiom (see e.g. DragonFly's
    // "getDriver().Dir + /../lib"); GetFilePath("crt0.o") searches this.
    getFilePaths().push_back(D.Dir + "/../lib/moe");
  }

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
// script, no --gc-sections. Milestone 10 Part B added real crt0/soft-float
// auto-linking (AddStartFiles/AddDefaultLibs, below) - everything else
// (linker script, stack-protector args, etc.) is still deliberately not
// productionized.
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("Moe::Linker", "ld.lld", TC) {}
  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;

private:
  // Adds runtime/build.sh's installed crt0.o, unless -nostartfiles/-nostdlib
  // suppress it (matching MSP430ToolChain's precedent).
  void AddStartFiles(const llvm::opt::ArgList &Args,
                      llvm::opt::ArgStringList &CmdArgs) const;
  // Adds the installed f32 soft-float runtime as a plain positional linker
  // input (not an -l/archive - only two flat .o files exist, no need for
  // ar/archive symbol-pulling machinery), unless -nostdlib/-nodefaultlibs
  // suppress it.
  void AddDefaultLibs(const llvm::opt::ArgList &Args,
                       llvm::opt::ArgStringList &CmdArgs) const;
};

} // end namespace moe
} // end namespace tools
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MOE_H
