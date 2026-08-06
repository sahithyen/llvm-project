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
};
}

class MoeSubtarget;

class MoeTargetLowering : public TargetLowering {
public:
  explicit MoeTargetLowering(const TargetMachine &TM,
                              const MoeSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  SDValue LowerShifts(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerConstantPool(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBR_CC(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerMUL(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerUDIV(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerUREM(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSDIV(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSREM(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerExtLoad(SDValue Op, SelectionDAG &DAG) const;

  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                              MachineBasicBlock *BB) const override;

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
  MachineBasicBlock *emitMul(MachineInstr &MI, MachineBasicBlock *BB) const;
  MachineBasicBlock *emitDivRem(MachineInstr &MI, MachineBasicBlock *BB) const;
  MachineBasicBlock *emitSDivRem(MachineInstr &MI, MachineBasicBlock *BB) const;
  MachineBasicBlock *emitVarShift(MachineInstr &MI, MachineBasicBlock *BB) const;
  Register emitCondNegate(MachineFunction *MF, MachineRegisterInfo &MRI,
                          const TargetInstrInfo *TII, const DebugLoc &DL,
                          MachineBasicBlock *&BB, Register ZeroReg,
                          Register TestReg, Register ValueReg) const;
};

} // namespace llvm

#endif
