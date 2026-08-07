//===--- Moe.cpp - Moe Helpers for Tools ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Moe.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

void MoeToolChain::addClangTargetOptions(const ArgList &DriverArgs,
                                         ArgStringList &CC1Args,
                                         Action::OffloadKind) const {
  // No system headers exist for Moe yet - no libc, no sysroot.
  CC1Args.push_back("-nostdsysteminc");
}

Tool *MoeToolChain::buildLinker() const {
  return new tools::moe::Linker(*this);
}

void moe::Linker::AddStartFiles(const ArgList &Args,
                                 ArgStringList &CmdArgs) const {
  if (Args.hasArg(options::OPT_nostartfiles, options::OPT_nostdlib))
    return;
  CmdArgs.push_back(Args.MakeArgString(getToolChain().GetFilePath("crt0.o")));
}

void moe::Linker::AddDefaultLibs(const ArgList &Args,
                                  ArgStringList &CmdArgs) const {
  if (Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs))
    return;
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_f32.o")));
  // The i64 arithmetic runtime (Milestone 12) - __muldi3/__udivdi3/etc, see
  // runtime/i64.ll. Also depends on widemul, defined in libmoe_f32.o above
  // (ld.lld resolves cross-object references regardless of link-line order
  // here, both are always present together).
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_i64.o")));
  // memcpy/memset (Milestone 13) - clang implicitly emits calls to these for
  // things as ordinary as a large struct assignment, see runtime/memcpy.ll.
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_memcpy.o")));
  // The f64/double arithmetic runtime (Milestone 18) - __adddf3/__muldf3/etc,
  // see runtime/f64.ll. Also depends on widemul (libmoe_f32.o) and the
  // variable-shift libcalls (libmoe_i64.o), both always present alongside.
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_f64.o")));
  // Minimal libc (Milestone 19) - malloc/free/calloc (runtime/malloc.ll),
  // strlen/strcpy/strcmp/etc (runtime/string.ll), putchar/puts
  // (runtime/stdio.ll, depends on strlen). No real system libc exists for
  // this freestanding target - these are it.
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_malloc.o")));
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_string.o")));
  CmdArgs.push_back(
      Args.MakeArgString(getToolChain().GetFilePath("libmoe_stdio.o")));
}

void moe::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                const InputInfo &Output,
                                const InputInfoList &Inputs,
                                const ArgList &Args,
                                const char *LinkingOutput) const {
  const ToolChain &ToolChain = getToolChain();
  std::string Linker = ToolChain.GetProgramPath(getShortName());
  ArgStringList CmdArgs;

  // Milestone 16: an explicit linker script (runtime/moe.ld) replaces the
  // previous bare -Ttext=0x0 --image-base=0x0 (relying on ld.lld's default
  // script) - formalizes the same RAM-at-0x0 layout explicitly, and defines
  // __bss_start/_end directly instead of leaving them to whatever ld.lld's
  // default happens to do with no real SECTIONS description (see
  // Milestone 10 Part B's crt0 bss-anchor fix for the class of bug that
  // caused - this closes it more robustly, at the linker-script level
  // rather than only working around it from the runtime side). ENTRY(_start)
  // lives in the script itself, so no separate --entry flag is needed.
  CmdArgs.push_back("-T");
  CmdArgs.push_back(Args.MakeArgString(getToolChain().GetFilePath("moe.ld")));

  AddStartFiles(Args, CmdArgs);

  Args.AddAllArgs(CmdArgs, options::OPT_L);
  ToolChain.AddFilePathLibArgs(Args, CmdArgs);
  AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);

  AddDefaultLibs(Args, CmdArgs);

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  Args.AddAllArgs(CmdArgs, options::OPT_T);

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), Args.MakeArgString(Linker),
      CmdArgs, Inputs, Output));
}
