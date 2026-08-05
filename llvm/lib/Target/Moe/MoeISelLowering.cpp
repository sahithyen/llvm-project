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

  // No hardware multiply/divide, and no runtime library exists to LibCall
  // out to (see the Milestone 3 plan) - all five are expanded in-backend
  // into software loops: MUL (LowerMUL/emitMul), UDIV/UREM (LowerUDIV/
  // LowerUREM/emitDivRem), and SDIV/SREM (LowerSDIV/LowerSREM/emitSDivRem,
  // which wraps emitDivRem's unsigned core with sign correction).
  setOperationAction(ISD::MUL, MVT::i32, Custom);
  setOperationAction(ISD::SDIV, MVT::i32, Custom);
  setOperationAction(ISD::UDIV, MVT::i32, Custom);
  setOperationAction(ISD::SREM, MVT::i32, Custom);
  setOperationAction(ISD::UREM, MVT::i32, Custom);

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
  case ISD::MUL:
    return LowerMUL(Op, DAG);
  case ISD::UDIV:
    return LowerUDIV(Op, DAG);
  case ISD::UREM:
    return LowerUREM(Op, DAG);
  case ISD::SDIV:
    return LowerSDIV(Op, DAG);
  case ISD::SREM:
    return LowerSREM(Op, DAG);
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
// Multiply - no hardware MUL and no runtime library exists (see the
// Milestone 3 plan), so this constructs MULPSEUDO directly as a machine
// node (bypassing SelectionDAG pattern matching entirely, since LowerMUL
// already has exactly the two operands the pseudo needs) - emitMul expands
// it into a real shift-and-add loop after instruction selection.
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerMUL(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  return SDValue(DAG.getMachineNode(Moe::MULPSEUDO, dl, MVT::i32, LHS, RHS),
                 0);
}

//===----------------------------------------------------------------------===//
// Unsigned divide/remainder - both construct the same DIVMODPSEUDO machine
// node (built directly, not matched via a SelectionDAG Pat, same as MUL)
// and just pick a different result index; if UDIV and UREM of the same
// operands both appear, SelectionDAG's node uniquing naturally fuses them
// into one loop. emitDivRem does the actual restoring-division expansion.
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerUDIV(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::i32);
  SDValue Ops[] = {Op.getOperand(0), Op.getOperand(1)};
  SDNode *N = DAG.getMachineNode(Moe::DIVMODPSEUDO, dl, VTs, Ops);
  return SDValue(N, 0);
}

SDValue MoeTargetLowering::LowerUREM(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::i32);
  SDValue Ops[] = {Op.getOperand(0), Op.getOperand(1)};
  SDNode *N = DAG.getMachineNode(Moe::DIVMODPSEUDO, dl, VTs, Ops);
  return SDValue(N, 1);
}

//===----------------------------------------------------------------------===//
// Signed divide/remainder - same construction as UDIV/UREM, but via
// SDIVMODPSEUDO (emitSDivRem wraps the unsigned core with sign correction).
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerSDIV(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::i32);
  SDValue Ops[] = {Op.getOperand(0), Op.getOperand(1)};
  SDNode *N = DAG.getMachineNode(Moe::SDIVMODPSEUDO, dl, VTs, Ops);
  return SDValue(N, 0);
}

SDValue MoeTargetLowering::LowerSREM(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::i32);
  SDValue Ops[] = {Op.getOperand(0), Op.getOperand(1)};
  SDNode *N = DAG.getMachineNode(Moe::SDIVMODPSEUDO, dl, VTs, Ops);
  return SDValue(N, 1);
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
  case Moe::MULPSEUDO:
    return emitMul(MI, BB);
  case Moe::DIVMODPSEUDO:
    return emitDivRem(MI, BB);
  case Moe::SDIVMODPSEUDO:
    return emitSDivRem(MI, BB);
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

//===----------------------------------------------------------------------===//
// Software multiply - SHIFT moves exactly one bit per instruction (see the
// spec's SHIFT section) and its carry-out is the bit shifted out, so this
// loop tests each multiplier bit directly off SHIFT's C flag: no separate
// AND-with-1 masking instruction needed (and AND has no immediate-mask form
// anyway) - see the Milestone 3 plan's "Software multiply" section.
//
// Blocks (BB is MULPSEUDO's parent block, split at the pseudo):
//   BB:         multiplicand/multiplier copies, result/counter/bound seeded
//               via self-XOR (always zero, regardless of the register's
//               prior value - there's no load-immediate instruction) and
//               INCREMENT's immediate field (bound = 32); falls through to
//               LoopBB.
//   LoopBB:     shift multiplier right by 1 (C = old bit0); if C is clear,
//               branch straight to ContinueBB (skip the add); otherwise
//               fall through to AddBB.
//   AddBB:      result += multiplicand; falls through to ContinueBB.
//   ContinueBB: merges result (PHI: unchanged value from LoopBB's skip edge,
//               or AddBB's updated value); shifts multiplicand left by 1;
//               increments counter; compares counter against bound (32);
//               loops back to LoopBB while not equal, else falls through to
//               ExitBB.
//   ExitBB:     receives MULPSEUDO's original successors; copies the final
//               result into MULPSEUDO's original destination register.
//
// Flags are always consumed by the very next instruction after whatever set
// them (SHIFT's C by the immediately-following JCC.CC; the counter/bound
// SUBcmp's Z by the immediately-following JCC.NE), with nothing else
// touching F in between - so no flag-preservation trick is needed despite
// ADD/SHIFT/INCREMENT all clobbering S/Z/C/O as a side effect.
//===----------------------------------------------------------------------===//

MachineBasicBlock *MoeTargetLowering::emitMul(MachineInstr &MI,
                                               MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *RC = &Moe::GPRRegClass;

  Register DstReg = MI.getOperand(0).getReg();
  Register AReg = MI.getOperand(1).getReg();
  Register BReg = MI.getOperand(2).getReg();

  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++BB->getIterator();
  MachineBasicBlock *LoopBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *AddBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ContinueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, LoopBB);
  MF->insert(InsertPt, AddBB);
  MF->insert(InsertPt, ContinueBB);
  MF->insert(InsertPt, ExitBB);

  ExitBB->splice(ExitBB->begin(), BB,
                 std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitBB->transferSuccessorsAndUpdatePHIs(BB);

  BB->addSuccessor(LoopBB);
  LoopBB->addSuccessor(ContinueBB); // carry clear: skip the add
  LoopBB->addSuccessor(AddBB);      // carry set: fall through to the add
  AddBB->addSuccessor(ContinueBB);
  ContinueBB->addSuccessor(LoopBB); // counter != bound: loop back
  ContinueBB->addSuccessor(ExitBB); // counter == bound: done

  // Every virtual register the loop touches is declared up front so PHIs
  // can reference values defined later in program order (the back-edge
  // inputs from ContinueBB), matching the same forward-declaration
  // technique MSP430's EmitShiftInstr uses for its shift-amount/shift-value
  // PHIs.
  Register Multiplicand0 = MRI.createVirtualRegister(RC);
  Register Multiplier0 = MRI.createVirtualRegister(RC);
  Register Result0 = MRI.createVirtualRegister(RC);
  Register Counter0 = MRI.createVirtualRegister(RC);
  Register Bound = MRI.createVirtualRegister(RC);

  Register MultiplicandPhi = MRI.createVirtualRegister(RC);
  Register MultiplierPhi = MRI.createVirtualRegister(RC);
  Register ResultPhi = MRI.createVirtualRegister(RC);
  Register CounterPhi = MRI.createVirtualRegister(RC);
  Register MultiplierNext = MRI.createVirtualRegister(RC);
  Register ResultAdded = MRI.createVirtualRegister(RC);
  Register ResultNext = MRI.createVirtualRegister(RC);
  Register MultiplicandNext = MRI.createVirtualRegister(RC);
  Register CounterNext = MRI.createVirtualRegister(RC);
  Register BoundZero = MRI.createVirtualRegister(RC);

  // BB
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Multiplicand0)
      .addReg(AReg);
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Multiplier0)
      .addReg(BReg);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), Result0)
      .addReg(Multiplicand0)
      .addReg(Multiplicand0);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), Counter0)
      .addReg(Multiplicand0)
      .addReg(Multiplicand0);
  // Bound = 32: INCREMENT's tied "$o = $oin" constraint needs oin to be a
  // *different* SSA value than the fresh result register it defines - see
  // the Milestone 3 plan/emitSDivRem's verifier-caught bug note - so the
  // zeroed seed (BoundZero) and the incremented result (Bound) must be
  // distinct virtual registers, not the same one reused in place.
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), BoundZero)
      .addReg(Multiplicand0)
      .addReg(Multiplicand0);
  BuildMI(*BB, MI, DL, TII->get(Moe::INCREMENT), Bound)
      .addReg(BoundZero)
      .addImm(32);

  // LoopBB: MultiplicandPhi/CounterPhi/ResultPhi's back-edge values are
  // defined in ContinueBB; MultiplierPhi's back-edge value (MultiplierNext)
  // is defined right here in LoopBB and simply flows unchanged through
  // AddBB/ContinueBB back to this PHI.
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), MultiplicandPhi)
      .addReg(Multiplicand0).addMBB(BB)
      .addReg(MultiplicandNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), MultiplierPhi)
      .addReg(Multiplier0).addMBB(BB)
      .addReg(MultiplierNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), ResultPhi)
      .addReg(Result0).addMBB(BB)
      .addReg(ResultNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), CounterPhi)
      .addReg(Counter0).addMBB(BB)
      .addReg(CounterNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(Moe::SRL), MultiplierNext).addReg(MultiplierPhi);
  BuildMI(LoopBB, DL, TII->get(Moe::JCC))
      .addMBB(ContinueBB)
      .addImm(MoeCC::COND_CC);

  // AddBB
  BuildMI(AddBB, DL, TII->get(Moe::ADD), ResultAdded)
      .addReg(ResultPhi)
      .addReg(MultiplicandPhi);

  // ContinueBB
  BuildMI(*ContinueBB, ContinueBB->begin(), DL, TII->get(TargetOpcode::PHI),
          ResultNext)
      .addReg(ResultPhi).addMBB(LoopBB)
      .addReg(ResultAdded).addMBB(AddBB);
  BuildMI(ContinueBB, DL, TII->get(Moe::SHL), MultiplicandNext)
      .addReg(MultiplicandPhi);
  BuildMI(ContinueBB, DL, TII->get(Moe::INCREMENT), CounterNext)
      .addReg(CounterPhi)
      .addImm(1);
  BuildMI(ContinueBB, DL, TII->get(Moe::SUBcmp))
      .addReg(CounterNext)
      .addReg(Bound);
  BuildMI(ContinueBB, DL, TII->get(Moe::JCC))
      .addMBB(LoopBB)
      .addImm(MoeCC::COND_NE);

  // ExitBB: ContinueBB is ExitBB's only predecessor, so a plain COPY
  // suffices - no PHI needed for a single incoming value.
  BuildMI(*ExitBB, ExitBB->begin(), DL, TII->get(TargetOpcode::COPY), DstReg)
      .addReg(ResultNext);

  MI.eraseFromParent();
  return ExitBB;
}

//===----------------------------------------------------------------------===//
// Software unsigned divide/remainder - restoring binary long division. Each
// iteration shifts the dividend left by one bit while simultaneously
// shifting that bit into the remainder from the bottom, via SHIFT's Carry-in
// chaining (SHLc - see the spec's SHIFT section on how Carry in feeds a
// shifted-out bit from one SHIFT into the next, the same mechanic a
// multi-word shift chain uses): SHL on the dividend produces C = its old
// MSB, and the immediately-following SHLc on the remainder consumes that
// same C as its shifted-in LSB.
//
// Blocks (BB is DIVMODPSEUDO's parent block, split at the pseudo):
//   BB:         dividend/divisor copies; remainder/quotient/counter/bound
//               seeded via self-XOR/INCREMENT (see emitMul's BB for the
//               same trick). Falls through to LoopBB.
//   LoopBB:     shift dividend left by 1 (C = old MSB); chain-shift
//               remainder left by 1, pulling that bit in (SHLc); compare
//               the shifted remainder against the divisor (SUBcmp - C=1
//               means no borrow, i.e. remainder >= divisor, see the spec's
//               SUB flags note); branch to SubBB if so, else fall through
//               to NoSubBB.
//   NoSubBB:    quotient bit is 0 this iteration (plain SHL, fills 0);
//               explicit jump to ContinueBB (not layout-adjacent).
//   SubBB:      remainder -= divisor; quotient bit is 1 this iteration
//               (SHL then INCREMENT +1); falls through to ContinueBB.
//   ContinueBB: merges remainder and quotient (PHI: NoSubBB's unchanged/
//               0-filled values, or SubBB's subtracted/1-filled values);
//               increments counter; compares against bound (32); loops back
//               to LoopBB while not equal, else falls through to ExitBB.
//   ExitBB:     receives DIVMODPSEUDO's original successors; copies the
//               final quotient/remainder into the pseudo's original
//               destination registers.
//
// As in emitMul, every flag-consuming branch/chain-shift immediately
// follows the instruction that set the flags it needs, so ADD/SUB/SHIFT/
// INCREMENT's shared clobbering of S/Z/C/O never needs a preservation
// trick.
//===----------------------------------------------------------------------===//

MachineBasicBlock *MoeTargetLowering::emitDivRem(MachineInstr &MI,
                                                  MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *RC = &Moe::GPRRegClass;

  Register QuotientDst = MI.getOperand(0).getReg();
  Register RemainderDst = MI.getOperand(1).getReg();
  Register AReg = MI.getOperand(2).getReg();
  Register BReg = MI.getOperand(3).getReg();

  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++BB->getIterator();
  MachineBasicBlock *LoopBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *NoSubBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *SubBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ContinueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, LoopBB);
  MF->insert(InsertPt, NoSubBB);
  MF->insert(InsertPt, SubBB);
  MF->insert(InsertPt, ContinueBB);
  MF->insert(InsertPt, ExitBB);

  ExitBB->splice(ExitBB->begin(), BB,
                 std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitBB->transferSuccessorsAndUpdatePHIs(BB);

  BB->addSuccessor(LoopBB);
  LoopBB->addSuccessor(SubBB);      // carry set (remainder >= divisor): subtract
  LoopBB->addSuccessor(NoSubBB);    // carry clear: no subtract
  NoSubBB->addSuccessor(ContinueBB);
  SubBB->addSuccessor(ContinueBB);
  ContinueBB->addSuccessor(LoopBB); // counter != bound: loop back
  ContinueBB->addSuccessor(ExitBB); // counter == bound: done

  Register Dividend0 = MRI.createVirtualRegister(RC);
  Register Divisor0 = MRI.createVirtualRegister(RC);
  Register Remainder0 = MRI.createVirtualRegister(RC);
  Register Quotient0 = MRI.createVirtualRegister(RC);
  Register Counter0 = MRI.createVirtualRegister(RC);
  Register Bound = MRI.createVirtualRegister(RC);

  Register DividendPhi = MRI.createVirtualRegister(RC);
  Register RemainderPhi = MRI.createVirtualRegister(RC);
  Register QuotientPhi = MRI.createVirtualRegister(RC);
  Register CounterPhi = MRI.createVirtualRegister(RC);
  Register DividendNext = MRI.createVirtualRegister(RC);
  Register RemainderShifted = MRI.createVirtualRegister(RC);
  Register QuotientNoSub = MRI.createVirtualRegister(RC);
  Register RemainderSub = MRI.createVirtualRegister(RC);
  Register QuotientSubTmp = MRI.createVirtualRegister(RC);
  Register QuotientSub = MRI.createVirtualRegister(RC);
  Register RemainderNext = MRI.createVirtualRegister(RC);
  Register QuotientNext = MRI.createVirtualRegister(RC);
  Register CounterNext = MRI.createVirtualRegister(RC);
  Register BoundZero = MRI.createVirtualRegister(RC);

  // BB
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Dividend0).addReg(AReg);
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Divisor0).addReg(BReg);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), Remainder0)
      .addReg(Dividend0)
      .addReg(Dividend0);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), Quotient0)
      .addReg(Dividend0)
      .addReg(Dividend0);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), Counter0)
      .addReg(Dividend0)
      .addReg(Dividend0);
  // Bound = 32: see emitMul's BoundZero comment - INCREMENT's tied "$o =
  // $oin" needs a fresh destination register distinct from its zeroed seed.
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), BoundZero)
      .addReg(Dividend0)
      .addReg(Dividend0);
  BuildMI(*BB, MI, DL, TII->get(Moe::INCREMENT), Bound)
      .addReg(BoundZero)
      .addImm(32);

  // LoopBB
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), DividendPhi)
      .addReg(Dividend0).addMBB(BB)
      .addReg(DividendNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), RemainderPhi)
      .addReg(Remainder0).addMBB(BB)
      .addReg(RemainderNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), QuotientPhi)
      .addReg(Quotient0).addMBB(BB)
      .addReg(QuotientNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), CounterPhi)
      .addReg(Counter0).addMBB(BB)
      .addReg(CounterNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(Moe::SHL), DividendNext).addReg(DividendPhi);
  BuildMI(LoopBB, DL, TII->get(Moe::SHLc), RemainderShifted)
      .addReg(RemainderPhi);
  BuildMI(LoopBB, DL, TII->get(Moe::SUBcmp))
      .addReg(RemainderShifted)
      .addReg(Divisor0);
  BuildMI(LoopBB, DL, TII->get(Moe::JCC)).addMBB(SubBB).addImm(MoeCC::COND_CS);

  // NoSubBB: remainder unchanged (RemainderShifted), quotient bit 0.
  BuildMI(NoSubBB, DL, TII->get(Moe::SHL), QuotientNoSub).addReg(QuotientPhi);
  BuildMI(NoSubBB, DL, TII->get(Moe::JMP)).addMBB(ContinueBB);

  // SubBB: remainder -= divisor, quotient bit 1.
  BuildMI(SubBB, DL, TII->get(Moe::SUB), RemainderSub)
      .addReg(RemainderShifted)
      .addReg(Divisor0);
  BuildMI(SubBB, DL, TII->get(Moe::SHL), QuotientSubTmp).addReg(QuotientPhi);
  BuildMI(SubBB, DL, TII->get(Moe::INCREMENT), QuotientSub)
      .addReg(QuotientSubTmp)
      .addImm(1);

  // ContinueBB
  BuildMI(*ContinueBB, ContinueBB->begin(), DL, TII->get(TargetOpcode::PHI),
          RemainderNext)
      .addReg(RemainderShifted).addMBB(NoSubBB)
      .addReg(RemainderSub).addMBB(SubBB);
  BuildMI(*ContinueBB, ContinueBB->begin(), DL, TII->get(TargetOpcode::PHI),
          QuotientNext)
      .addReg(QuotientNoSub).addMBB(NoSubBB)
      .addReg(QuotientSub).addMBB(SubBB);
  BuildMI(ContinueBB, DL, TII->get(Moe::INCREMENT), CounterNext)
      .addReg(CounterPhi)
      .addImm(1);
  BuildMI(ContinueBB, DL, TII->get(Moe::SUBcmp))
      .addReg(CounterNext)
      .addReg(Bound);
  BuildMI(ContinueBB, DL, TII->get(Moe::JCC))
      .addMBB(LoopBB)
      .addImm(MoeCC::COND_NE);

  // ExitBB: ContinueBB is ExitBB's only predecessor, so plain COPYs suffice.
  BuildMI(*ExitBB, ExitBB->begin(), DL, TII->get(TargetOpcode::COPY),
          RemainderDst)
      .addReg(RemainderNext);
  BuildMI(*ExitBB, ExitBB->begin(), DL, TII->get(TargetOpcode::COPY),
          QuotientDst)
      .addReg(QuotientNext);

  MI.eraseFromParent();
  return ExitBB;
}

//===----------------------------------------------------------------------===//
// Conditional negate - tests TestReg's sign (SUBcmp against a caller-
// supplied zero register; S=1 iff TestReg < 0, see the spec's Flags
// section) and conditionally negates ValueReg (0 - ValueReg, via the same
// zero-register trick emitMul/emitDivRem use to materialize 0 - there's no
// dedicated NEGATE instruction). TestReg and ValueReg may be the same
// register (an absolute-value use) or different (e.g. testing one value's
// sign to decide whether to negate a different one) - see emitSDivRem's
// uses of both shapes. Same skip-the-work/fall-through-does-the-work/merge
// shape as emitMul's LoopBB/AddBB/ContinueBB: *BB is updated to the merge
// block where the result is available and the caller should continue
// appending code (this never creates a distinct "exit" block of its own -
// the caller keeps building directly in the returned block).
//===----------------------------------------------------------------------===//

Register MoeTargetLowering::emitCondNegate(MachineFunction *MF,
                                            MachineRegisterInfo &MRI,
                                            const TargetInstrInfo *TII,
                                            const DebugLoc &DL,
                                            MachineBasicBlock *&BB,
                                            Register ZeroReg, Register TestReg,
                                            Register ValueReg) const {
  const TargetRegisterClass *RC = &Moe::GPRRegClass;
  MachineBasicBlock *CurBB = BB;
  const BasicBlock *LLVM_BB = CurBB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++CurBB->getIterator();
  MachineBasicBlock *NegBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *MergeBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, NegBB);
  MF->insert(InsertPt, MergeBB);

  CurBB->addSuccessor(MergeBB); // positive/zero: skip negation
  CurBB->addSuccessor(NegBB);   // negative: fall through and negate

  Register Negated = MRI.createVirtualRegister(RC);
  Register Result = MRI.createVirtualRegister(RC);

  BuildMI(CurBB, DL, TII->get(Moe::SUBcmp)).addReg(TestReg).addReg(ZeroReg);
  BuildMI(CurBB, DL, TII->get(Moe::JCC)).addMBB(MergeBB).addImm(MoeCC::COND_PL);

  BuildMI(NegBB, DL, TII->get(Moe::SUB), Negated)
      .addReg(ZeroReg)
      .addReg(ValueReg);
  NegBB->addSuccessor(MergeBB);

  BuildMI(*MergeBB, MergeBB->begin(), DL, TII->get(TargetOpcode::PHI), Result)
      .addReg(ValueReg).addMBB(CurBB)
      .addReg(Negated).addMBB(NegBB);

  BB = MergeBB;
  return Result;
}

//===----------------------------------------------------------------------===//
// Software signed divide/remainder - wraps the same restoring-division loop
// emitDivRem uses (duplicated here rather than shared, so emitDivRem's
// already-verified block wiring stays untouched by this function) with
// sign correction: absolute-value both operands via emitCondNegate, run the
// unsigned loop, then negate the quotient if the operands' original signs
// differed and the remainder if the dividend was negative (C-style
// truncating-toward-zero division - the remainder takes the dividend's
// sign). AReg XOR BReg's sign bit equals AReg's-sign XOR BReg's-sign, so no
// separate boolean sign tracking is needed for the quotient correction -
// see the Milestone 3 plan's "Software divide/remainder" section.
//===----------------------------------------------------------------------===//

MachineBasicBlock *MoeTargetLowering::emitSDivRem(MachineInstr &MI,
                                                   MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *RC = &Moe::GPRRegClass;

  Register QuotientDst = MI.getOperand(0).getReg();
  Register RemainderDst = MI.getOperand(1).getReg();
  Register AReg = MI.getOperand(2).getReg();
  Register BReg = MI.getOperand(3).getReg();

  MachineBasicBlock *EntryBB = BB;

  // Detach whatever originally followed MI (e.g. the lowered `ret`'s own
  // COPY+RET) into an unlinked scratch block immediately, before any other
  // block-building below. Until this happens, EntryBB->end() still means
  // "after that original tail," not "after our own code" - every other
  // BuildMI call in this function (and in emitCondNegate) that appends via
  // a bare MachineBasicBlock* (rather than an explicit "insert before X"
  // iterator) would otherwise land after that stale tail instead of where
  // intended, only to get swept along by the final splice below into the
  // real exit block in the wrong position (past its own RET) - this was a
  // real bug, caught by -verify-machineinstrs, see the Milestone 3 plan.
  // TailScratch is deliberately never inserted into MF's block list here;
  // its content is spliced into the real ExitBB at the very end.
  MachineBasicBlock *TailScratch =
      MF->CreateMachineBasicBlock(EntryBB->getBasicBlock());
  TailScratch->splice(TailScratch->begin(), EntryBB,
                       std::next(MachineBasicBlock::iterator(MI)),
                       EntryBB->end());
  TailScratch->transferSuccessorsAndUpdatePHIs(EntryBB);

  Register ZeroReg = MRI.createVirtualRegister(RC);
  Register SignXor = MRI.createVirtualRegister(RC);
  BuildMI(*EntryBB, MI, DL, TII->get(Moe::XOR), ZeroReg)
      .addReg(AReg)
      .addReg(AReg);
  BuildMI(*EntryBB, MI, DL, TII->get(Moe::XOR), SignXor)
      .addReg(AReg)
      .addReg(BReg);

  MachineBasicBlock *CurBB = EntryBB;
  Register AbsA = emitCondNegate(MF, MRI, TII, DL, CurBB, ZeroReg, AReg, AReg);
  Register AbsB = emitCondNegate(MF, MRI, TII, DL, CurBB, ZeroReg, BReg, BReg);

  // Unsigned restoring-division loop on the absolute values - same shape as
  // emitDivRem's LoopBB/NoSubBB/SubBB/ContinueBB (see that function for the
  // per-instruction rationale).
  const BasicBlock *LLVM_BB = CurBB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++CurBB->getIterator();
  MachineBasicBlock *LoopBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *NoSubBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *SubBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ContinueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *LoopExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, LoopBB);
  MF->insert(InsertPt, NoSubBB);
  MF->insert(InsertPt, SubBB);
  MF->insert(InsertPt, ContinueBB);
  MF->insert(InsertPt, LoopExitBB);

  CurBB->addSuccessor(LoopBB);
  LoopBB->addSuccessor(SubBB);
  LoopBB->addSuccessor(NoSubBB);
  NoSubBB->addSuccessor(ContinueBB);
  SubBB->addSuccessor(ContinueBB);
  ContinueBB->addSuccessor(LoopBB);
  ContinueBB->addSuccessor(LoopExitBB);

  Register Remainder0 = MRI.createVirtualRegister(RC);
  Register Quotient0 = MRI.createVirtualRegister(RC);
  Register Counter0 = MRI.createVirtualRegister(RC);
  Register Bound = MRI.createVirtualRegister(RC);

  Register DividendPhi = MRI.createVirtualRegister(RC);
  Register RemainderPhi = MRI.createVirtualRegister(RC);
  Register QuotientPhi = MRI.createVirtualRegister(RC);
  Register CounterPhi = MRI.createVirtualRegister(RC);
  Register DividendNext = MRI.createVirtualRegister(RC);
  Register RemainderShifted = MRI.createVirtualRegister(RC);
  Register QuotientNoSub = MRI.createVirtualRegister(RC);
  Register RemainderSub = MRI.createVirtualRegister(RC);
  Register QuotientSubTmp = MRI.createVirtualRegister(RC);
  Register QuotientSub = MRI.createVirtualRegister(RC);
  Register RemainderNext = MRI.createVirtualRegister(RC);
  Register QuotientNext = MRI.createVirtualRegister(RC);
  Register CounterNext = MRI.createVirtualRegister(RC);
  Register UQuotient = MRI.createVirtualRegister(RC);
  Register URemainder = MRI.createVirtualRegister(RC);
  Register BoundZero = MRI.createVirtualRegister(RC);

  BuildMI(CurBB, DL, TII->get(Moe::XOR), Remainder0).addReg(AbsA).addReg(AbsA);
  BuildMI(CurBB, DL, TII->get(Moe::XOR), Quotient0).addReg(AbsA).addReg(AbsA);
  BuildMI(CurBB, DL, TII->get(Moe::XOR), Counter0).addReg(AbsA).addReg(AbsA);
  // Bound = 32: see emitMul's BoundZero comment - INCREMENT's tied "$o =
  // $oin" needs a fresh destination register distinct from its zeroed seed.
  BuildMI(CurBB, DL, TII->get(Moe::XOR), BoundZero).addReg(AbsA).addReg(AbsA);
  BuildMI(CurBB, DL, TII->get(Moe::INCREMENT), Bound)
      .addReg(BoundZero)
      .addImm(32);

  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), DividendPhi)
      .addReg(AbsA).addMBB(CurBB)
      .addReg(DividendNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), RemainderPhi)
      .addReg(Remainder0).addMBB(CurBB)
      .addReg(RemainderNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), QuotientPhi)
      .addReg(Quotient0).addMBB(CurBB)
      .addReg(QuotientNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), CounterPhi)
      .addReg(Counter0).addMBB(CurBB)
      .addReg(CounterNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(Moe::SHL), DividendNext).addReg(DividendPhi);
  BuildMI(LoopBB, DL, TII->get(Moe::SHLc), RemainderShifted)
      .addReg(RemainderPhi);
  BuildMI(LoopBB, DL, TII->get(Moe::SUBcmp))
      .addReg(RemainderShifted)
      .addReg(AbsB);
  BuildMI(LoopBB, DL, TII->get(Moe::JCC)).addMBB(SubBB).addImm(MoeCC::COND_CS);

  BuildMI(NoSubBB, DL, TII->get(Moe::SHL), QuotientNoSub).addReg(QuotientPhi);
  BuildMI(NoSubBB, DL, TII->get(Moe::JMP)).addMBB(ContinueBB);

  BuildMI(SubBB, DL, TII->get(Moe::SUB), RemainderSub)
      .addReg(RemainderShifted)
      .addReg(AbsB);
  BuildMI(SubBB, DL, TII->get(Moe::SHL), QuotientSubTmp).addReg(QuotientPhi);
  BuildMI(SubBB, DL, TII->get(Moe::INCREMENT), QuotientSub)
      .addReg(QuotientSubTmp)
      .addImm(1);

  BuildMI(*ContinueBB, ContinueBB->begin(), DL, TII->get(TargetOpcode::PHI),
          RemainderNext)
      .addReg(RemainderShifted).addMBB(NoSubBB)
      .addReg(RemainderSub).addMBB(SubBB);
  BuildMI(*ContinueBB, ContinueBB->begin(), DL, TII->get(TargetOpcode::PHI),
          QuotientNext)
      .addReg(QuotientNoSub).addMBB(NoSubBB)
      .addReg(QuotientSub).addMBB(SubBB);
  BuildMI(ContinueBB, DL, TII->get(Moe::INCREMENT), CounterNext)
      .addReg(CounterPhi)
      .addImm(1);
  BuildMI(ContinueBB, DL, TII->get(Moe::SUBcmp))
      .addReg(CounterNext)
      .addReg(Bound);
  BuildMI(ContinueBB, DL, TII->get(Moe::JCC))
      .addMBB(LoopBB)
      .addImm(MoeCC::COND_NE);

  // UQuotient/URemainder are exactly ContinueBB's final QuotientNext/
  // RemainderNext, valid at LoopExitBB (its only predecessor) with no PHI
  // needed - a plain COPY makes them available under stable names for the
  // sign-correction step below.
  BuildMI(*LoopExitBB, LoopExitBB->begin(), DL, TII->get(TargetOpcode::COPY),
          URemainder)
      .addReg(RemainderNext);
  BuildMI(*LoopExitBB, LoopExitBB->begin(), DL, TII->get(TargetOpcode::COPY),
          UQuotient)
      .addReg(QuotientNext);

  CurBB = LoopExitBB;
  Register FinalQuotient =
      emitCondNegate(MF, MRI, TII, DL, CurBB, ZeroReg, SignXor, UQuotient);
  Register FinalRemainder =
      emitCondNegate(MF, MRI, TII, DL, CurBB, ZeroReg, AReg, URemainder);

  // ExitBB already holds one PHI (FinalRemainder's, from the last
  // emitCondNegate call) - unlike emitMul/emitDivRem's fresh ExitBB, the
  // final copies must be appended *after* that PHI, and the original tail
  // spliced in after those copies (not at begin(), which would land before
  // the PHI and violate "PHIs must lead the block").
  MachineBasicBlock *ExitBB = CurBB;
  BuildMI(*ExitBB, ExitBB->end(), DL, TII->get(TargetOpcode::COPY),
          RemainderDst)
      .addReg(FinalRemainder);
  BuildMI(*ExitBB, ExitBB->end(), DL, TII->get(TargetOpcode::COPY),
          QuotientDst)
      .addReg(FinalQuotient);

  ExitBB->splice(ExitBB->end(), TailScratch, TailScratch->begin(),
                 TailScratch->end());
  ExitBB->transferSuccessorsAndUpdatePHIs(TailScratch);

  MI.eraseFromParent();
  return ExitBB;
}
