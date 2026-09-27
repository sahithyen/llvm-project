//===-- MoeISelLowering.h - Moe DAG Lowering Interface ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces that Moe uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MOE_MOEISELLOWERING_H
#define LLVM_LIB_TARGET_MOE_MOEISELLOWERING_H

#include "Moe.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {
namespace MoeISD {
enum NodeType {
  FIRST_NUMBER = ISD::BUILTIN_OP_END,
  CALL,
  RET,
  CMP,
  BR_CC,
  Wrapper,
  SHL1,
  SRL1,
  SRA1,
  // The by-two forms, candidate N3: SHIFT's Amount bit. A constant shift by n
  // is ceil(n/2) of these plus at most one single-bit shift.
  SHL2,
  SRL2,
  SRA2,
  // And by four - the second amount bit. There is no by-three node: a shift by
  // three is by-two then by-one, which LowerShifts already builds.
  SHL4,
  SRL4,
  SRA4,
};
}

class MoeSubtarget;

class MoeTargetLowering : public TargetLowering {
public:
  explicit MoeTargetLowering(const TargetMachine &TM,
                              const MoeSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  /// MULDIV's divide is 37 T-states, and so is the high multiply that
  /// magic-number division would replace it with - so a 32-bit divide by a
  /// constant stays a divide. A 64-bit one does not: there is no 64-bit
  /// divide, and leaving it alone makes a __divdi3 call, which the kernel -
  /// which relies on the multiply - does not even link against.
  bool isIntDivCheap(EVT VT, AttributeList Attr) const override {
    return VT == MVT::i32;
  }

  /// ...but a signed divide or remainder by a power of two is a few shifts,
  /// and the generic hooks give that up whenever division is "cheap". Always
  /// take the shifts.
  SDValue BuildSDIVPow2(SDNode *N, const APInt &Divisor, SelectionDAG &DAG,
                        SmallVectorImpl<SDNode *> &Created) const override {
    return SDValue();
  }
  SDValue BuildSREMPow2(SDNode *N, const APInt &Divisor, SelectionDAG &DAG,
                        SmallVectorImpl<SDNode *> &Created) const override {
    return SDValue();
  }

  // Inline asm (Milestone 15): only the "r" (any GP register) constraint -
  // register operands only, no memory constraints, matching MoeAsmParser's
  // own scope (no memory-operand AsmParser support for inline asm to
  // assemble against yet either).
  ConstraintType getConstraintType(StringRef Constraint) const override;
  std::pair<unsigned, const TargetRegisterClass *>
  getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                               StringRef Constraint, MVT VT) const override;

  SDValue LowerShifts(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerConstantPool(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBR_CC(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSETCC(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSELECT_CC(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerMUL(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerMulByConstant(SDValue X, uint32_t C, const SDLoc &dl,
                             SelectionDAG &DAG) const;
  SDValue LowerSDivRem(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerExtLoad(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG) const;

  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                              MachineBasicBlock *BB) const override;

  /// Switch statements become a chain of compares rather than a jump table.
  ///
  /// A table is perfectly implementable here - the entries would be absolute
  /// addresses (there is no PC-relative form to choose between) and the
  /// dispatch would be a load followed by MOVE -> IA. Two of the three pieces
  /// now exist: BRIND is a real instruction (MoeInstrInfo.td), and a computed
  /// goto selects to it. What is still missing is BR_JT expanded into it and
  /// ISD::JumpTable lowered through the same constant-pool indirection every
  /// other address goes through. Left undone deliberately: it is a
  /// code-density optimisation on a machine that will want one, not something
  /// a kernel needs to boot, and turning it off is one line that cannot be
  /// subtly wrong.
  bool areJTsAllowed(const Function *Fn) const override { return false; }

private:
  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool isVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &dl, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                     SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerCallResult(SDValue Chain, SDValue InGlue,
                          CallingConv::ID CallConv, bool isVarArg,
                          const SmallVectorImpl<ISD::InputArg> &Ins,
                          const SDLoc &dl, SelectionDAG &DAG,
                          SmallVectorImpl<SDValue> &InVals) const;

  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &dl,
                      SelectionDAG &DAG) const override;

  MachineBasicBlock *emitCall(MachineInstr &MI, MachineBasicBlock *BB) const;
  MachineBasicBlock *emitVarShift(MachineInstr &MI, MachineBasicBlock *BB) const;
  SDValue LowerDYNAMIC_STACKALLOC(SDValue Op, SelectionDAG &DAG) const;
  MachineBasicBlock *emitSetCC(MachineInstr &MI, MachineBasicBlock *BB) const;
  MachineBasicBlock *emitSelectCC(MachineInstr &MI, MachineBasicBlock *BB) const;
};

} // namespace llvm

#endif
