//===-- MoeISelLowering.cpp - Moe DAG Lowering Implementation ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MoeTargetLowering class.
//
//===----------------------------------------------------------------------===//

#include "MoeISelLowering.h"
#include "MoeConstantPoolValue.h"
#include "MoeMachineFunctionInfo.h"
#include "MoeSubtarget.h"
#include "MoeTargetMachine.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#include "MoeGenCallingConv.inc"

MoeTargetLowering::MoeTargetLowering(const TargetMachine &TM,
                                      const MoeSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &Moe::GPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  setStackPointerRegisterToSaveRestore(Moe::SP);
  setBooleanContents(ZeroOrOneBooleanContent);

  // JUMP/LOAD/STORE's trailing operand word must land on a word-aligned
  // address (see 'Addressing mode' in the Encoding chapter); the MC-layer
  // encoder (Milestone 2) decides padding by tracking each function's
  // running byte offset from a known-aligned start, which only works if
  // that start is actually guaranteed word-aligned.
  setMinFunctionAlignment(Align(4));
  setPrefFunctionAlignment(Align(4));

  // No load-immediate instruction - every constant materializes via the
  // literal pool (see the Milestone 1 plan's "Constant materialization").
  setOperationAction(ISD::Constant, MVT::i32, Custom);
  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::ConstantPool, MVT::i32, Custom);

  // No CMP instruction (a compare is a SUB whose result is discarded - see
  // the Milestone 1 plan's ABI section) and no flag-testing SELECT/SETCC
  // instruction, so route everything through BR_CC.
  setOperationAction(ISD::BR_CC, MVT::i32, Custom);
  setOperationAction(ISD::BRCOND, MVT::Other, Expand);

  // SHIFT moves exactly one bit per instruction - see MoeISelLowering::
  // LowerShifts and the Milestone 1 plan's SHIFT notes. Variable-amount
  // shifts are out of scope for Milestone 1.
  setOperationAction(ISD::SHL, MVT::i32, Custom);
  setOperationAction(ISD::SRL, MVT::i32, Custom);
  setOperationAction(ISD::SRA, MVT::i32, Custom);

  // No hardware multiply/divide.
  setOperationAction(ISD::MUL, MVT::i32, LibCall);
  setOperationAction(ISD::SDIV, MVT::i32, LibCall);
  setOperationAction(ISD::UDIV, MVT::i32, LibCall);
  setOperationAction(ISD::SREM, MVT::i32, LibCall);
  setOperationAction(ISD::UREM, MVT::i32, LibCall);

  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Expand);
  setOperationAction(ISD::STACKSAVE, MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);
}

//===----------------------------------------------------------------------===//
// LowerOperation dispatch
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::SHL:
  case ISD::SRL:
  case ISD::SRA:
    return LowerShifts(Op, DAG);
  case ISD::Constant:
  case ISD::ConstantPool:
    return LowerConstantPool(Op, DAG);
  case ISD::GlobalAddress:
    return LowerGlobalAddress(Op, DAG);
  case ISD::BR_CC:
    return LowerBR_CC(Op, DAG);
  default:
    llvm_unreachable("unimplemented operand");
  }
}

//===----------------------------------------------------------------------===//
// Constant materialization - every constant, including the call sequence's
// return-address label (via MoeConstantPoolValue, built directly at the
// MachineInstr level in emitCall since it never goes through SelectionDAG),
// goes through a MachineConstantPool entry read by LOADabs. A GlobalValue is
// itself a Constant, so LowerGlobalAddress reuses the exact same
// TargetConstantPool + Wrapper mechanism.
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerConstantPool(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDLoc dl(Op);
  EVT PtrVT = Op.getValueType();

  const Constant *C;
  Align Alignment;
  if (auto *CN = dyn_cast<ConstantSDNode>(Op)) {
    C = ConstantInt::get(*DAG.getContext(), CN->getAPIntValue());
    Alignment = Align(4);
  } else {
    auto *CP = cast<ConstantPoolSDNode>(Op);
    assert(!CP->isMachineConstantPoolEntry() &&
           "target-specific constant pool entries not used by Milestone 1");
    C = CP->getConstVal();
    Alignment = CP->getAlign();
  }

  SDValue CPIdx = DAG.getTargetConstantPool(C, PtrVT, Alignment);
  SDValue Wrapper = DAG.getNode(MoeISD::Wrapper, dl, PtrVT, CPIdx);
  // The constant's VALUE, not its address, is what the caller wants (e.g.
  // `add i32 %x, 5` needs the number 5, not where it's stored) - LOADabs
  // reads through the pool entry to get it. The load is invariant/read-only
  // constant-pool data, so DAG.getEntryNode() is a safe chain.
  return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), Wrapper,
                      MachinePointerInfo::getConstantPool(DAG.getMachineFunction()));
}

SDValue MoeTargetLowering::LowerGlobalAddress(SDValue Op,
                                               SelectionDAG &DAG) const {
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  int64_t Offset = cast<GlobalAddressSDNode>(Op)->getOffset();
  EVT PtrVT = Op.getValueType();
  SDLoc dl(Op);

  // A GlobalValue is itself a Constant - wrap it in a pool entry the same
  // way LowerConstantPool does, since Moe has no way to materialize an
  // address without first reading it from somewhere that already holds it
  // as data (see the Milestone 1 plan's "Constant materialization" notes).
  const Constant *C = GV;
  if (Offset != 0)
    C = ConstantExpr::getGetElementPtr(
        Type::getInt8Ty(*DAG.getContext()), const_cast<GlobalValue *>(GV),
        ConstantInt::get(Type::getInt64Ty(*DAG.getContext()), Offset));

  SDValue CPIdx = DAG.getTargetConstantPool(C, PtrVT, Align(4));
  SDValue Wrapper = DAG.getNode(MoeISD::Wrapper, dl, PtrVT, CPIdx);
  return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), Wrapper,
                      MachinePointerInfo::getConstantPool(DAG.getMachineFunction()));
}

//===----------------------------------------------------------------------===//
// Shifts - SHIFT moves exactly one bit per instruction, so a shift by
// constant N lowers to N chained target-specific single-bit-shift nodes
// (never re-marked Custom, so the legalizer doesn't re-visit them) - see
// the Milestone 1 plan's SHIFT notes. Variable-amount shifts aren't needed
// by Milestone 1's test case and aren't supported yet.
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerShifts(SDValue Op, SelectionDAG &DAG) const {
  SDNode *N = Op.getNode();
  EVT VT = Op.getValueType();
  SDLoc dl(N);

  auto *Amt = dyn_cast<ConstantSDNode>(N->getOperand(1));
  if (!Amt)
    report_fatal_error(
        "Moe: variable-amount shifts are not yet supported (Milestone 1)");

  uint64_t ShiftAmount = Amt->getZExtValue();
  unsigned SingleBitOpc;
  switch (Op.getOpcode()) {
  case ISD::SHL:
    SingleBitOpc = MoeISD::SHL1;
    break;
  case ISD::SRL:
    SingleBitOpc = MoeISD::SRL1;
    break;
  case ISD::SRA:
    SingleBitOpc = MoeISD::SRA1;
    break;
  default:
    llvm_unreachable("not a shift");
  }

  SDValue Result = N->getOperand(0);
  while (ShiftAmount--)
    Result = DAG.getNode(SingleBitOpc, dl, VT, Result);
  return Result;
}

//===----------------------------------------------------------------------===//
// Compares/branches - Moe has no CMP instruction; a compare is a SUB whose
// result is discarded (SUBcmp), matched to Moecmp, feeding JCC's condition
// operand directly (Moe's 16 conditions map onto ISD::CondCode 1:1 in
// meaning, just not in numbering, so no glue-value translation table is
// needed beyond this switch) - see the Milestone 1 plan's ABI/instruction
// selection notes.
//===----------------------------------------------------------------------===//

static unsigned getMoeCondCode(ISD::CondCode CC) {
  switch (CC) {
  case ISD::SETEQ:
    return MoeCC::COND_EQ;
  case ISD::SETNE:
    return MoeCC::COND_NE;
  case ISD::SETLT:
    return MoeCC::COND_LT;
  case ISD::SETLE:
    return MoeCC::COND_LE;
  case ISD::SETGT:
    return MoeCC::COND_GT;
  case ISD::SETGE:
    return MoeCC::COND_GE;
  case ISD::SETULT:
    return MoeCC::COND_CC;
  case ISD::SETUGT:
    return MoeCC::COND_HI;
  case ISD::SETULE:
    return MoeCC::COND_LS;
  case ISD::SETUGE:
    return MoeCC::COND_CS;
  default:
    llvm_unreachable("Invalid integer condition!");
  }
}

SDValue MoeTargetLowering::LowerBR_CC(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(1))->get();
  SDValue LHS = Op.getOperand(2);
  SDValue RHS = Op.getOperand(3);
  SDValue Dest = Op.getOperand(4);
  SDLoc dl(Op);

  SDValue TargetCC = DAG.getTargetConstant(getMoeCondCode(CC), dl, MVT::i32);
  SDValue Flag = DAG.getNode(MoeISD::CMP, dl, MVT::Glue, LHS, RHS);
  return DAG.getNode(MoeISD::BR_CC, dl, Op.getValueType(), Chain, Dest,
                      TargetCC, Flag);
}

//===----------------------------------------------------------------------===//
// Calling convention (see MoeCallingConv.td and the Milestone 1 plan's ABI
// section: GP0-3 args/caller-saved, GP0 also the return value, GP4-5
// reserved scratch, GP6-7 callee-saved).
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_Moe);

  for (const CCValAssign &VA : ArgLocs) {
    if (VA.isRegLoc()) {
      Register VReg = RegInfo.createVirtualRegister(&Moe::GPRRegClass);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      SDValue ArgValue = DAG.getCopyFromReg(Chain, dl, VReg, VA.getLocVT());
      InVals.push_back(ArgValue);
    } else {
      assert(VA.isMemLoc());
      MachineFrameInfo &MFI = MF.getFrameInfo();
      int FI = MFI.CreateFixedObject(4, VA.getLocMemOffset(), true);
      SDValue FIN = DAG.getFrameIndex(FI, getFrameIndexTy(DAG.getDataLayout()));
      InVals.push_back(DAG.getLoad(
          VA.getLocVT(), dl, Chain, FIN,
          MachinePointerInfo::getFixedStack(DAG.getMachineFunction(), FI)));
    }
  }

  return Chain;
}

bool MoeTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_Moe);
}

SDValue MoeTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &dl,
    SelectionDAG &DAG) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_Moe);

  SDValue Glue;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  for (const CCValAssign &VA : RVLocs) {
    assert(VA.isRegLoc() && "Can only return in registers!");
    Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(), OutVals[&VA - &RVLocs[0]], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(MoeISD::RET, dl, MVT::Other, RetOps);
}

SDValue MoeTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                      SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc &dl = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins = CLI.Ins;
  SDValue Chain = CLI.Chain;
  SDValue Callee = CLI.Callee;
  CLI.IsTailCall = false;
  CallingConv::ID CallConv = CLI.CallConv;
  bool isVarArg = CLI.IsVarArg;

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_Moe);

  unsigned NumBytes = CCInfo.getStackSize();
  Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, dl);

  SmallVector<std::pair<unsigned, SDValue>, 4> RegsToPass;
  SmallVector<SDValue, 12> MemOpChains;
  SDValue StackPtr;

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = OutVals[i];

    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
    } else {
      assert(VA.isMemLoc());
      if (!StackPtr.getNode())
        StackPtr = DAG.getCopyFromReg(Chain, dl, Moe::SP,
                                       getFrameIndexTy(DAG.getDataLayout()));
      SDValue PtrOff =
          DAG.getNode(ISD::ADD, dl, getFrameIndexTy(DAG.getDataLayout()),
                      StackPtr, DAG.getIntPtrConstant(VA.getLocMemOffset(), dl));
      MemOpChains.push_back(
          DAG.getStore(Chain, dl, Arg, PtrOff, MachinePointerInfo()));
    }
  }

  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains);

  SDValue InGlue;
  for (const auto &[Reg, N] : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, dl, Reg, N, InGlue);
    InGlue = Chain.getValue(1);
  }

  // Direct calls only for Milestone 1 - turn the callee into a
  // TargetGlobalAddress so legalize doesn't try to touch it.
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, MVT::i32);
  else
    report_fatal_error("Moe: only direct calls are supported (Milestone 1)");

  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  for (const auto &[Reg, N] : RegsToPass)
    Ops.push_back(DAG.getRegister(Reg, N.getValueType()));
  if (InGlue.getNode())
    Ops.push_back(InGlue);

  Chain = DAG.getNode(MoeISD::CALL, dl, NodeTys, Ops);
  InGlue = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain, NumBytes, 0, InGlue, dl);
  InGlue = Chain.getValue(1);

  return LowerCallResult(Chain, InGlue, CallConv, isVarArg, Ins, dl, DAG,
                         InVals);
}

SDValue MoeTargetLowering::LowerCallResult(
    SDValue Chain, SDValue InGlue, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeCallResult(Ins, RetCC_Moe);

  for (const CCValAssign &VA : RVLocs) {
    Chain = DAG.getCopyFromReg(Chain, dl, VA.getLocReg(), VA.getValVT(), InGlue)
                .getValue(1);
    InGlue = Chain.getValue(2);
    InVals.push_back(Chain.getValue(0));
  }

  return Chain;
}

//===----------------------------------------------------------------------===//
// Custom inserters - the call and return sequences are each more than one
// real instruction (no hardware call/link instruction - see the Milestone 1
// plan's ABI section), so CALL/RET are Pseudo instructions expanded here,
// after instruction selection but before register allocation.
//===----------------------------------------------------------------------===//

MachineBasicBlock *
MoeTargetLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                                MachineBasicBlock *BB) const {
  switch (MI.getOpcode()) {
  case Moe::CALL:
    return emitCall(MI, BB);
  default:
    llvm_unreachable("Unexpected instr type to insert");
  }
}

MachineBasicBlock *MoeTargetLowering::emitCall(MachineInstr &MI,
                                                MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();

  // A fresh local symbol marking "the address right after this call
  // sequence" - the return address the callee jumps back to. Using a plain
  // EH_LABEL pseudo (instead of splitting BB and referencing the new
  // block's own label) avoids relying on a block boundary that later CFG
  // simplification passes (block/branch folding) would just merge back
  // away, since there's no real branch between "before" and "after" the
  // call from their point of view - see the Milestone 1 plan's call-
  // sequence design.
  MCSymbol *RetSym = MF->getContext().createTempSymbol("callret", true);

  // LOADabs always dereferences its trailing address operand (loads
  // memory[addr], not addr itself - see the spec's LOAD section), so getting
  // RetSym's address as a *value* into GP4 needs the same pool indirection
  // every other constant goes through (LowerConstantPool/LowerGlobalAddress):
  // a pool slot holding RetSym's address, read through by LOADabs.
  Type *PtrTy = Type::getInt32Ty(MF->getFunction().getContext());
  MoeConstantPoolValue *CPV = MoeConstantPoolValue::Create(PtrTy, RetSym);
  unsigned CPIdx = MF->getConstantPool()->getConstantPoolIndex(CPV, Align(4));

  // LOAD.W [pool entry containing RetSym] -> GP4 ; PUSH.W GP4 ; JUMP.AL [callee] ; RetSym:
  BuildMI(*BB, MI, DL, TII->get(Moe::LOADabs), Moe::GP4)
      .addConstantPoolIndex(CPIdx);
  BuildMI(*BB, MI, DL, TII->get(Moe::PUSH)).addReg(Moe::GP4, RegState::Kill);
  MachineInstrBuilder MIB =
      BuildMI(*BB, MI, DL, TII->get(Moe::JMPabs)).add(MI.getOperand(0));
  // Preserve the implicit arg-register uses the variadic Moecall pattern
  // attached to the pseudo, so they stay live across the expansion.
  for (unsigned i = 1, e = MI.getNumOperands(); i != e; ++i)
    MIB.add(MI.getOperand(i));
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::EH_LABEL)).addSym(RetSym);

  MI.eraseFromParent();
  return BB;
}
