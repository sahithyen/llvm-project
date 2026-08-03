//===-- MoeTargetMachine.cpp - Define TargetMachine for Moe --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level implementation for the Moe target.
//
//===----------------------------------------------------------------------===//

#include "MoeTargetMachine.h"
#include "Moe.h"
#include "MoeMachineFunctionInfo.h"
#include "TargetInfo/MoeTargetInfo.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include <optional>

using namespace llvm;

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeMoeTarget() {
  RegisterTargetMachine<MoeTargetMachine> X(getTheMoeTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeMoeAsmPrinterPass(PR);
  initializeMoeDAGToDAGISelLegacyPass(PR);
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

MoeTargetMachine::MoeTargetMachine(const Target &T, const Triple &TT,
                                    StringRef CPU, StringRef FS,
                                    const TargetOptions &Options,
                                    std::optional<Reloc::Model> RM,
                                    std::optional<CodeModel::Model> CM,
                                    CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
                               getEffectiveRelocModel(RM),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT, std::string(CPU), std::string(FS), *this) {
  initAsmInfo();
}

MoeTargetMachine::~MoeTargetMachine() = default;

namespace {
class MoePassConfig : public TargetPassConfig {
public:
  MoePassConfig(MoeTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  MoeTargetMachine &getMoeTargetMachine() const {
    return getTM<MoeTargetMachine>();
  }

  bool addInstSelector() override;
};
} // namespace

TargetPassConfig *MoeTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new MoePassConfig(*this, PM);
}

MachineFunctionInfo *MoeTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return MoeMachineFunctionInfo::create<MoeMachineFunctionInfo>(Allocator, F,
                                                                 STI);
}

bool MoePassConfig::addInstSelector() {
  addPass(createMoeISelDag(getMoeTargetMachine(), getOptLevel()));
  return false;
}
