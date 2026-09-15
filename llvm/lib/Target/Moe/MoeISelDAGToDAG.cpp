//===-- MoeISelDAGToDAG.cpp - A dag to dag inst selector for Moe ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the Moe target. Milestone 1
// needs no custom SDNode selection at all - every instruction pattern in
// MoeInstrInfo.td is handled by the auto-generated table-driven matcher
// (SelectCode). A custom SelectAddr (for Register-indirect frame-index
// addressing) is added once that lands - see the Milestone 1 plan's
// "Frame-index elimination and spills" section.
//
//===----------------------------------------------------------------------===//

#include "Moe.h"
#include "MoeISelLowering.h"
#include "MoeTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"

using namespace llvm;

#define DEBUG_TYPE "moe-isel"
#define PASS_NAME "Moe DAG->DAG Pattern Instruction Selection"

namespace {
class MoeDAGToDAGISel : public SelectionDAGISel {
public:
  MoeDAGToDAGISel() = delete;

  MoeDAGToDAGISel(MoeTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISel(TM, OptLevel) {}

  void Select(SDNode *N) override;

  // Register-indirect addressing (moemem): a bare frame-index (disp 0,
  // later rewritten in place by MoeRegisterInfo::eliminateFrameIndex), a
  // frame-index plus a constant offset folded in, or any other i32 pointer
  // value held in a register (disp 0) - e.g. a global's address already
  // materialized by LOADabs - optionally plus a constant offset. See the
  // Milestone 1 plan's "Frame-index elimination and spills" section.
  bool SelectAddr(SDValue Addr, SDValue &Base, SDValue &Disp);

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    InlineAsm::ConstraintCode ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

#include "MoeGenDAGISel.inc"
};

class MoeDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  MoeDAGToDAGISelLegacy(MoeTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISelLegacy(
            ID, std::make_unique<MoeDAGToDAGISel>(TM, OptLevel)) {}
};
} // end anonymous namespace

char MoeDAGToDAGISelLegacy::ID;

INITIALIZE_PASS(MoeDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createMoeISelDag(MoeTargetMachine &TM,
                                      CodeGenOptLevel OptLevel) {
  return new MoeDAGToDAGISelLegacy(TM, OptLevel);
}

bool MoeDAGToDAGISel::SelectAddr(SDValue Addr, SDValue &Base, SDValue &Disp) {
  // A Wrapper(TargetConstantPool/...) node is exactly LOADabs/STOREabs's own
  // pattern (Absolute addressing - see MoeInstrInfo.td's LOADabs) - reject
  // it here so that more specific pattern always wins, rather than
  // ambiguously also "matching" this catch-all Register-indirect pattern
  // (whose fallback below would otherwise treat the Wrapper node itself,
  // not a real register, as the base).
  if (Addr.getOpcode() == MoeISD::Wrapper)
    return false;

  if (auto *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), Addr.getValueType());
    Disp = CurDAG->getTargetConstant(0, SDLoc(Addr), MVT::i32);
    return true;
  }

  if (Addr.getOpcode() == ISD::ADD) {
    if (auto *FIN = dyn_cast<FrameIndexSDNode>(Addr.getOperand(0))) {
      if (auto *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1))) {
        Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), Addr.getValueType());
        Disp = CurDAG->getTargetConstant(CN->getSExtValue(), SDLoc(Addr), MVT::i32);
        return true;
      }
    }
    if (auto *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1))) {
      Base = Addr.getOperand(0);
      Disp = CurDAG->getTargetConstant(CN->getSExtValue(), SDLoc(Addr), MVT::i32);
      return true;
    }
  }

  // Otherwise: a plain i32 pointer value in a register (e.g. a global's
  // address already materialized by LOADabs, or a pointer function
  // argument), no offset.
  Base = Addr;
  Disp = CurDAG->getTargetConstant(0, SDLoc(Addr), MVT::i32);
  return true;
}

// The "m" inline-asm constraint. Without this, any asm operand constrained "m"
// aborts instruction selection outright ("Could not match memory address.
// Inline asm failure!"), because the generic
// SelectionDAGISel::SelectInlineAsmMemoryOperands has no target-independent way
// to turn an address into operands the target's printer understands.
//
// This matters well beyond convenience: Linux uses "m" pervasively in its
// uaccess, bitops and atomic headers, so a kernel port cannot get far without
// it. Moe has exactly one memory-operand shape - Register-indirect's (base
// register, offset) pair, see moemem in MoeInstrInfo.td - and SelectAddr
// already produces it for ordinary loads and stores, including folding a
// constant offset and handing back a frame index for a stack object (which
// MoeRegisterInfo::eliminateFrameIndex then rewrites in place, exactly as it
// does for LOADrr/STORErr, since the operand pair sits adjacent here too).
bool MoeDAGToDAGISel::SelectInlineAsmMemoryOperand(
    const SDValue &Op, InlineAsm::ConstraintCode ConstraintID,
    std::vector<SDValue> &OutOps) {
  switch (ConstraintID) {
  case InlineAsm::ConstraintCode::m:
  // "o" is "m" restricted to an offsettable address. Every address Moe can
  // express IS offsettable - the offset is a 28-bit field in the trailing
  // operand word, not part of the instruction - so the two are the same here.
  case InlineAsm::ConstraintCode::o: {
    SDValue Base, Disp;
    if (!SelectAddr(Op, Base, Disp))
      return true;
    OutOps.push_back(Base);
    OutOps.push_back(Disp);
    return false;
  }
  default:
    // Anything else (including "X", which clang can hand down for an operand
    // it could not classify) is reported unhandled rather than guessed at.
    return true;
  }
}

void MoeDAGToDAGISel::Select(SDNode *Node) {
  if (Node->isMachineOpcode()) {
    Node->setNodeId(-1);
    return;
  }

  // A bare stack-object address used as an ordinary value (not as a
  // LOAD/STORE address operand - that case is already absorbed into
  // moeaddr_ri/SelectAddr's Pat matching before a FrameIndex node would ever
  // reach here as an independent root). ISD::FrameIndex defaults to Legal
  // (never marked Custom - see the Milestone 7 plan), so it's never routed
  // through MoeTargetLowering::LowerOperation; it must be handled directly
  // here, mirroring MSP430's identical ISD::FrameIndex case in its own
  // Select() override.
  if (Node->getOpcode() == ISD::FrameIndex) {
    SDLoc dl(Node);
    int FI = cast<FrameIndexSDNode>(Node)->getIndex();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, Node->getValueType(0));
    SDValue Disp = CurDAG->getTargetConstant(0, dl, MVT::i32);
    CurDAG->SelectNodeTo(Node, Moe::FIADDR, Node->getValueType(0), TFI, Disp);
    return;
  }

  SelectCode(Node);
}
