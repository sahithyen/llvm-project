//===-- MoeELFStreamer.h - Moe ELF object output ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MCTARGETDESC_MOEELFSTREAMER_H
#define LLVM_LIB_TARGET_MOE_MCTARGETDESC_MOEELFSTREAMER_H

#include "llvm/MC/MCELFStreamer.h"

namespace llvm {

/// Emits JUMP/LOAD/STORE's trailing operand word (and the alignment padding
/// in front of it) as ordinary streamer output, so MC's layout decides the
/// padding instead of the code emitter guessing at its own position. See
/// MoeELFStreamer.cpp's file comment for why that guess had to go.
class MoeELFStreamer : public MCELFStreamer {
public:
  using MCELFStreamer::MCELFStreamer;

  void emitInstruction(const MCInst &Inst,
                       const MCSubtargetInfo &STI) override;
};

} // namespace llvm

#endif
