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
}

void moe::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                const InputInfo &Output,
                                const InputInfoList &Inputs,
                                const ArgList &Args,
                                const char *LinkingOutput) const {
  const ToolChain &ToolChain = getToolChain();
  std::string Linker = ToolChain.GetProgramPath(getShortName());
  ArgStringList CmdArgs;

  // Matches every llvm-tests/run-*.sh script's ld.lld invocation exactly -
  // see Moe.h's Linker comment for why nothing more (no linker script, no
  // --gc-sections) is added here.
  CmdArgs.push_back("--entry=_start");
  CmdArgs.push_back("-Ttext=0x0");
  CmdArgs.push_back("--image-base=0x0");

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
