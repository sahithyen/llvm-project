//===-- MoeSubtarget.h - Define Subtarget for the Moe ----------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the Moe specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOESUBTARGET_H
#define LLVM_LIB_TARGET_MOE_MOESUBTARGET_H

#include "MoeFrameLowering.h"
#include "MoeISelLowering.h"
#include "MoeInstrInfo.h"
#include "MoeRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGTargetInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include <memory>
#include <string>

#define GET_SUBTARGETINFO_HEADER
#include "MoeGenSubtargetInfo.inc"

namespace llvm {
class StringRef;
class TargetMachine;

class MoeSubtarget : public MoeGenSubtargetInfo {
  virtual void anchor();
  MoeInstrInfo InstrInfo;
  MoeTargetLowering TLInfo;
  std::unique_ptr<const SelectionDAGTargetInfo> TSInfo;
  MoeFrameLowering FrameLowering;

public:
  MoeSubtarget(const Triple &TT, const std::string &CPU, const std::string &FS,
               const TargetMachine &TM);

  ~MoeSubtarget() override;

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const TargetFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const MoeInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const MoeRegisterInfo *getRegisterInfo() const override {
    return &getInstrInfo()->getRegisterInfo();
  }
  const MoeTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const SelectionDAGTargetInfo *getSelectionDAGInfo() const override {
    return TSInfo.get();
  }
};
} // namespace llvm

#endif
