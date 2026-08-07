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
  // the Milestone 1 plan's ABI section) and no flag-testing SELECT
  // instruction, so route control-flow comparisons through BR_CC.
  setOperationAction(ISD::BR_CC, MVT::i32, Custom);
  setOperationAction(ISD::BRCOND, MVT::Other, Expand);

  // Value-producing comparisons (Milestone 11) - see SETCCPSEUDO's def
  // comment in MoeInstrInfo.td for why this needs a real branchy sequence
  // (LowerSETCC/emitSetCC), not a single instruction.
  setOperationAction(ISD::SETCC, MVT::i32, Custom);

  // No flag-testing SELECT instruction either (same reason as BR_CC/SETCC
  // above). Needed for i64 relational comparisons (Milestone 12): the
  // generic i64 legalizer's compare-by-parts expansion (compare high words;
  // if equal, compare low words) combines the two results via SELECT_CC,
  // which otherwise survives all the way to instruction selection with no
  // lowering and crashes ("Cannot select: ... = select_cc"). Custom (a real
  // SETCCPSEUDO-shaped branchy sequence - see LowerSELECT_CC/emitSelectCC),
  // not Expand: LegalizeDAG asserts Expand'ing SELECT_CC requires SELECT to
  // NOT also be Expand (its generic Expand implementation lowers through
  // SELECT as an intermediate step), and Moe has no SELECT lowering of its
  // own to fall back to either.
  setOperationAction(ISD::SELECT_CC, MVT::i32, Custom);

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

  // i64 arithmetic (Milestone 12): add/sub/udiv/urem/sdiv/srem/icmp/zext/
  // sext/trunc all already work via the generic i64-from-i32-halves
  // legalizer's pure-i32 expansion now that real SETCC/SELECT_CC exist
  // (Milestone 11 above) - confirmed empirically, no action needed for any
  // of them. MUL and variable-amount shifts don't: no hardware or
  // software-loop-based wide-multiply/parts-shift primitive exists (or ever
  // will - see runtime/i64.ll's __muldi3/__ashldi3/__lshrdi3/__ashrdi3), so
  // explicitly Expand the primitives the generic legalizer tries first
  // (UMUL_LOHI etc, SHL_PARTS etc) so it falls through to the standard
  // compiler-rt RTLIB names instead of leaving them unhandled through to
  // instruction selection, which crashes ("Cannot select: ... =
  // umul_lohi"/"shl_parts").
  setOperationAction(ISD::MUL, MVT::i64, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::MULHS, MVT::i32, Expand);
  setOperationAction(ISD::MULHU, MVT::i32, Expand);
  setOperationAction(ISD::SHL, MVT::i64, Expand);
  setOperationAction(ISD::SRL, MVT::i64, Expand);
  setOperationAction(ISD::SRA, MVT::i64, Expand);
  setOperationAction(ISD::SHL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRA_PARTS, MVT::i32, Expand);

  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Expand);
  setOperationAction(ISD::STACKSAVE, MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);

  // Variadic functions (Milestone 14): getBuiltinVaListKind is
  // CharPtrBuiltinVaList (clang/lib/Basic/Targets/Moe.h) - va_list is just a
  // char*, but that model still routes va_start/va_arg through real
  // ISD::VASTART/ISD::VAARG nodes (confirmed empirically: -S -emit-llvm on a
  // real variadic function shows llvm.va_start/a genuine `va_arg`
  // instruction, not something clang's frontend expands away on its own).
  // VASTART needs Custom (LowerVASTART, writing the register-save area's
  // address - see LowerFormalArguments's isVarArg block); VAARG needs only
  // the fully generic Expand (plain pointer-walking, natural type-size
  // increments, no padding), not a real Custom lowering of its own - Moe has
  // no type needing more than 4-byte alignment as a variadic argument
  // (datalayout bakes i64 in at i64:32, matching the DoubleWidth=32
  // TargetInfo collapse), unlike e.g. Sparc's VAARG Custom-lowering, needed
  // there only for 8-byte-aligned doubles.
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction(ISD::VAARG, MVT::Other, Expand);
  // va_end is a no-op for a plain-pointer va_list (nothing to release).
  setOperationAction(ISD::VAEND, MVT::Other, Expand);

  // A Byte/Halfword-size LOAD only overwrites the selected lane of its
  // destination register - the rest keeps its prior value (see 'Handling
  // words, halfwords and bytes' in the Encoding chapter) - unlike most RISC
  // ISAs' natively-extending loads. So a zext/sext load needs a real
  // multi-instruction sequence (LowerExtLoad), not a single Pat-matched
  // instruction - see LOADrr_B/LOADrr_H's comment in MoeInstrInfo.td.
  // EXTLOAD (undefined upper bits) is treated identically to ZEXTLOAD: safe,
  // since "undefined" permits any value, and reuses the same sequence.
  for (MVT MemVT : {MVT::i8, MVT::i16}) {
    setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, MemVT, Custom);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i32, MemVT, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::i32, MemVT, Custom);
  }

  // DAGCombiner can reuse an existing zextload's value for a sextload of
  // the same address (rather than emitting a second load) by wrapping it in
  // a SIGN_EXTEND_INREG - a distinct ISD opcode from the LOAD node
  // LowerExtLoad handles above, and one this target has never needed until
  // now. Expand is the standard generic lowering (shift left then
  // arithmetic-shift-right by the same amount) and, since SHL/SRA are
  // already Custom-lowered via LowerShifts's constant-amount chain above,
  // this reuses that existing, already-working machinery for free.
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i8, Expand);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16, Expand);
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
  case ISD::SETCC:
    return LowerSETCC(Op, DAG);
  case ISD::SELECT_CC:
    return LowerSELECT_CC(Op, DAG);
  case ISD::VASTART:
    return LowerVASTART(Op, DAG);
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
  case ISD::LOAD:
    return LowerExtLoad(Op, DAG);
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

  unsigned SingleBitOpc;
  unsigned RealOpc; // Moe::SHL/SRL/SRA - the loop body emitVarShift builds.
  switch (Op.getOpcode()) {
  case ISD::SHL:
    SingleBitOpc = MoeISD::SHL1;
    RealOpc = Moe::SHL;
    break;
  case ISD::SRL:
    SingleBitOpc = MoeISD::SRL1;
    RealOpc = Moe::SRL;
    break;
  case ISD::SRA:
    SingleBitOpc = MoeISD::SRA1;
    RealOpc = Moe::SRA;
    break;
  default:
    llvm_unreachable("not a shift");
  }

  auto *Amt = dyn_cast<ConstantSDNode>(N->getOperand(1));
  if (!Amt) {
    // Not a constant amount - build VARSHIFTPSEUDO directly as a machine
    // node (bypassing SelectionDAG pattern matching, same as LowerMUL),
    // expanded into a real runtime loop by emitVarShift after instruction
    // selection.
    SDValue OpcConst = DAG.getTargetConstant(RealOpc, dl, MVT::i32);
    return SDValue(DAG.getMachineNode(Moe::VARSHIFTPSEUDO, dl, VT,
                                       N->getOperand(0), N->getOperand(1),
                                       OpcConst),
                   0);
  }

  uint64_t ShiftAmount = Amt->getZExtValue();
  SDValue Result = N->getOperand(0);
  while (ShiftAmount--)
    Result = DAG.getNode(SingleBitOpc, dl, VT, Result);
  return Result;
}

//===----------------------------------------------------------------------===//
// Sub-word (byte/halfword) extending loads - see LOADrr_B/LOADrr_H's comment
// in MoeInstrInfo.td for why these need real multi-instruction sequences
// rather than a single Pat-matched instruction: a Byte/Halfword-size LOAD
// only merges into the selected lane of its destination register, leaving
// the rest unchanged, so zext needs the destination zeroed first and sext
// needs the already-working constant-shift-chain machinery (SHL1/SRA1 from
// LowerShifts above) to discard the garbage upper bits. Built as straight-
// line DAG.getMachineNode chaining (mirroring LowerMUL's direct construction
// of MULPSEUDO), not a pseudo + custom inserter, since this sequence never
// branches or loops - no new MachineBasicBlock is needed.
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerExtLoad(SDValue Op, SelectionDAG &DAG) const {
  LoadSDNode *LD = cast<LoadSDNode>(Op);
  ISD::LoadExtType ExtType = LD->getExtensionType();
  if (ExtType == ISD::NON_EXTLOAD)
    return SDValue(); // word loads stay on the existing, already-working Pat path

  EVT MemVT = LD->getMemoryVT();
  assert((MemVT == MVT::i8 || MemVT == MVT::i16) &&
         "Moe: only byte/halfword extending loads are custom-lowered");
  SDLoc dl(Op);
  SDValue Chain = LD->getChain();
  SDValue Base = LD->getBasePtr();
  SDValue Offset = DAG.getTargetConstant(0, dl, MVT::i32);

  // A FrameIndex-typed Base (sub-word access straight through a local
  // variable's own address) is fine here since Milestone 7's FIADDR pseudo:
  // Base is still an ordinary (non-machine) FrameIndex SDValue at this
  // point, and MoeDAGToDAGISel::Select's ISD::FrameIndex case selects it
  // into FIADDR independently, before this function's own hand-built
  // machine nodes (which reference Base as an operand) get emitted - see
  // the Milestone 7 plan.

  unsigned RawLoadOpc = (MemVT == MVT::i8) ? Moe::LOADrr_B : Moe::LOADrr_H;
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::Other);

  if (ExtType == ISD::SEXTLOAD) {
    // The raw load's tied "old value" input can be any i32 SDValue - its
    // garbage upper bits are discarded by the shift chain below regardless
    // of what they are.
    SDNode *RawLoad = DAG.getMachineNode(RawLoadOpc, dl, VTs,
                                          {Base, Base, Offset, Chain});
    DAG.setNodeMemRefs(cast<MachineSDNode>(RawLoad), {LD->getMemOperand()});
    SDValue Result = SDValue(RawLoad, 0);
    unsigned ShiftCount = 32 - MemVT.getSizeInBits();
    for (unsigned i = 0; i < ShiftCount; ++i)
      Result = DAG.getNode(MoeISD::SHL1, dl, MVT::i32, Result);
    for (unsigned i = 0; i < ShiftCount; ++i)
      Result = DAG.getNode(MoeISD::SRA1, dl, MVT::i32, Result);
    return DAG.getMergeValues({Result, SDValue(RawLoad, 1)}, dl);
  }

  // ZEXTLOAD / EXTLOAD: zero the destination first, then the raw load
  // merges into the now-zero low lane. The zero must be built via
  // DAG.getMachineNode(Moe::XOR, ...), NOT DAG.getNode(ISD::XOR, ...) - the
  // generic, target-independent legalizer constant-folds X^X to a Constant 0
  // node before instruction selection ever runs (confirmed via
  // -print-after-all while designing this), silently defeating the
  // self-zero trick (the same one emitMul's init block already uses at the
  // MachineInstr level, where this fold doesn't apply) if built the
  // "obvious" DAG-level way.
  SDNode *Zero = DAG.getMachineNode(Moe::XOR, dl, MVT::i32, Base, Base);
  SDNode *RawLoad = DAG.getMachineNode(
      RawLoadOpc, dl, VTs, {SDValue(Zero, 0), Base, Offset, Chain});
  DAG.setNodeMemRefs(cast<MachineSDNode>(RawLoad), {LD->getMemOperand()});
  return DAG.getMergeValues({SDValue(RawLoad, 0), SDValue(RawLoad, 1)}, dl);
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

// Constructs SETCCPSEUDO directly as a machine node (bypassing SelectionDAG
// pattern matching, same as LowerMUL) - emitSetCC expands it into a real
// SUBcmp+JCC branchy sequence producing 0/1 after instruction selection. See
// SETCCPSEUDO's def comment in MoeInstrInfo.td.
SDValue MoeTargetLowering::LowerSETCC(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  SDValue TargetCC = DAG.getTargetConstant(getMoeCondCode(CC), dl, MVT::i32);
  return SDValue(
      DAG.getMachineNode(Moe::SETCCPSEUDO, dl, MVT::i32, LHS, RHS, TargetCC),
      0);
}

// Constructs SELECTCCPSEUDO directly as a machine node (bypassing
// SelectionDAG pattern matching, same as LowerSETCC/LowerMUL) - emitSelectCC
// expands it into the same SUBcmp+JCC branchy sequence as emitSetCC, merging
// True/False operands instead of hardcoded 0/1. See SELECTCCPSEUDO's def
// comment in MoeInstrInfo.td.
SDValue MoeTargetLowering::LowerSELECT_CC(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  SDValue True = Op.getOperand(2);
  SDValue False = Op.getOperand(3);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(4))->get();
  SDValue TargetCC = DAG.getTargetConstant(getMoeCondCode(CC), dl, MVT::i32);
  SDValue Ops[] = {LHS, RHS, True, False, TargetCC};
  return SDValue(
      DAG.getMachineNode(Moe::SELECTCCPSEUDO, dl, MVT::i32, Ops), 0);
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
      // +4: the callee's entry SP points directly at the return address the
      // caller's CALL sequence just pushed (POP'd only by this function's
      // own epilogue, not by hardware) - so the first overflow-argument
      // word the caller stored (at its own pre-push SP + offset 0) actually
      // sits one word *above* entry SP, not at it. Confirmed by execution:
      // without this adjustment, every stack-passed argument silently reads
      // back the value meant for the previous overflow slot (or garbage for
      // the very first one) - see the Milestone 6 plan's "more than 4
      // argument words" finding.
      int FI = MFI.CreateFixedObject(4, VA.getLocMemOffset() + 4, true);
      SDValue FIN = DAG.getFrameIndex(FI, getFrameIndexTy(DAG.getDataLayout()));
      InVals.push_back(DAG.getLoad(
          VA.getLocVT(), dl, Chain, FIN,
          MachinePointerInfo::getFixedStack(DAG.getMachineFunction(), FI)));
    }
  }

  // Variadic functions (Milestone 14): spill whichever of GP0-3 named
  // arguments didn't consume into a register-save area, so va_arg's fully
  // generic pointer-walking expansion (see the VASTART/VAARG
  // setOperationAction comment above) can read them contiguously starting
  // from LowerVASTART's initial pointer.
  //
  // Deliberate, documented scope cut: this only covers the up-to-4-total-
  // arguments case (named + variadic together) - real overflow variadic
  // arguments (a 5th+ total argument, stack-passed by LowerCall's existing,
  // unmodified convention) are NOT reachable by continuing to walk past the
  // register-save area. Making that work would need the register-save area
  // and the stack-passed-overflow area to be memory-contiguous, but Moe's
  // entry SP points directly at the just-pushed return address (unlike e.g.
  // Sparc's register-window ABI, which reserves fixed, args-independent
  // stack space for every possible register argument) - the return address
  // occupies exactly the 4 bytes that would need to sit between the two
  // areas, and there is no way to skip over it with a plain, uniform
  // pointer-plus-size VAARG expansion. Solving this needs real ABI work
  // (e.g. always stack-passing variadic arguments), out of scope here - a
  // variadic function called with more than 4 total arguments will read
  // back garbage for the 5th and later, not silently or loudly fail.
  if (isVarArg) {
    static const MCPhysReg ArgRegs[] = {Moe::GP0, Moe::GP1, Moe::GP2,
                                         Moe::GP3};
    unsigned NumConsumed = CCInfo.getFirstUnallocated(ArgRegs);
    unsigned NumToSave = 4 - NumConsumed;

    // Always create this (even a 0-size object, if every register was
    // consumed by named parameters) so LowerVASTART always has a valid
    // frame index to reference - a variadic function is always a valid
    // va_start target regardless of how many named parameters it declares.
    MachineFrameInfo &MFI = MF.getFrameInfo();
    int VarArgsFI = MFI.CreateStackObject(4 * NumToSave, Align(4), false);
    MF.getInfo<MoeMachineFunctionInfo>()->setVarArgsFrameIndex(VarArgsFI);

    if (NumToSave > 0) {
      SmallVector<SDValue, 4> OutChains;
      for (unsigned i = NumConsumed; i != 4; ++i) {
        Register VReg = RegInfo.createVirtualRegister(&Moe::GPRRegClass);
        RegInfo.addLiveIn(ArgRegs[i], VReg);
        SDValue ArgValue = DAG.getCopyFromReg(Chain, dl, VReg, MVT::i32);

        SDValue FIN = DAG.getFrameIndex(VarArgsFI,
                                         getFrameIndexTy(DAG.getDataLayout()));
        SDValue Offset =
            DAG.getNode(ISD::ADD, dl, getFrameIndexTy(DAG.getDataLayout()),
                        FIN, DAG.getIntPtrConstant(4 * (i - NumConsumed), dl));
        OutChains.push_back(
            DAG.getStore(Chain, dl, ArgValue, Offset, MachinePointerInfo()));
      }
      Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, OutChains);
    }
  }

  return Chain;
}

// va_start just stores the register-save area's address (a real FrameIndex
// value, resolved to a genuine SP-relative address the same way FIADDR
// already handles "a stack object's address as a value" - see the
// Milestone 7 plan - not a frame-pointer-plus-offset computation like
// Sparc's LowerVASTART, since Moe has no frame pointer at all) into the
// va_list slot.
SDValue MoeTargetLowering::LowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MoeMachineFunctionInfo *FuncInfo = MF.getInfo<MoeMachineFunctionInfo>();
  SDLoc dl(Op);

  int VarArgsFI = FuncInfo->getVarArgsFrameIndex();
  SDValue FIN = DAG.getFrameIndex(VarArgsFI, getFrameIndexTy(DAG.getDataLayout()));
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), dl, FIN, Op.getOperand(1),
                       MachinePointerInfo(SV));
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
  // TargetGlobalAddress so legalize doesn't try to touch it. A libcall (e.g.
  // the soft-float runtime's __addsf3, emitted automatically by the generic
  // float-softening legalizer - see the Milestone 8 plan) has an
  // ExternalSymbolSDNode callee instead, referenced by name rather than by
  // GlobalValue*, since the calling module never defines it - same
  // TargetExternalSymbol treatment.
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, MVT::i32);
  else if (auto *ES = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(ES->getSymbol(), MVT::i32);
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
  case Moe::VARSHIFTPSEUDO:
    return emitVarShift(MI, BB);
  case Moe::SETCCPSEUDO:
    return emitSetCC(MI, BB);
  case Moe::SELECTCCPSEUDO:
    return emitSelectCC(MI, BB);
  default:
    llvm_unreachable("Unexpected instr type to insert");
  }
}

MachineBasicBlock *MoeTargetLowering::emitCall(MachineInstr &MI,
                                                MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();

  // The call's continuation - everything that originally followed MI in BB -
  // becomes a real MachineBasicBlock, not a mid-block EH_LABEL (Milestone
  // 1-3's approach). See the Milestone 4 plan for the full diagnosis: with a
  // mid-block EH_LABEL, RegAllocFast's per-block reload placement
  // (reloadAtBegin/getMBBBeginInsertionPoint, which explicitly knows to
  // insert reloads right after a block-leading label) never applies, since
  // that mechanism only runs at real block boundaries. RegAllocFast instead
  // processes each block in a single *backward* per-instruction scan
  // (confirmed via -debug-only=regalloc) that has no notion of "this jump's
  // fallthrough is fake, control only resumes via the callee's own jump
  // back to a label" - it was observed placing a reload for a value needed
  // by a *later* call right after JMPabs (the last instruction it can see),
  // landing it in dead code before the label and silently corrupting any
  // value that must survive from before this call to a later call's
  // argument setup. A real block boundary lets RegAllocFast's own
  // block-crossing liveness mechanism handle this correctly instead.
  //
  // Everything originally after MI - including ADJCALLSTACKUP and
  // LowerCallResult's `%result:gpr = COPY $gp0` (reading the call's return
  // value straight out of the physical register RetCC_Moe assigns it to) -
  // moves into ContinueBB: none of it can stay in BB, since nothing placed
  // in BB after JMPabs is ever actually reached (an earlier attempt at this
  // fix tried keeping the result COPY in BB on the theory that a fresh
  // block's physical-register live-in is disallowed pre-regalloc - it built
  // cleanly and passed -verify-machineinstrs, but silently produced dead
  // code for exactly the same reason as the original bug, only caught by
  // actually executing a 3-call chain where the result has to survive to a
  // later call - see sdiv_test.ll and the Milestone 4 plan). GP0's live-in
  // below (added when the call's result is actually consumed - see the
  // Milestone 7 plan's addition of that condition) does trip
  // MachineVerifier's "allocatable live-in but isn't entry/landing-pad"
  // check while still in SSA form (pre-PHI-elimination) - that
  // check is gated off once the function leaves SSA form
  // (MF->getProperties().hasNoPHIs(), see MachineVerifier.cpp), and no
  // later pass in this pipeline (phi-elimination, two-address, RegAllocFast
  // itself) actually depends on the invariant it's protecting for a block
  // with no PHIs of its own to confuse - confirmed by execution, not just
  // by satisfying the verifier at one intermediate stage.
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++BB->getIterator();
  MachineBasicBlock *ContinueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, ContinueBB);
  ContinueBB->splice(ContinueBB->begin(), BB,
                      std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ContinueBB->transferSuccessorsAndUpdatePHIs(BB);
  BB->addSuccessor(ContinueBB);

  // Only declare GP0 live-in here if this call's return value is actually
  // consumed - i.e. ContinueBB (just spliced in above) contains
  // LowerCallResult's `COPY $gp0` reading it. A call whose LLVM-level
  // return type is void, or was demoted to a hidden sret pointer (see the
  // Milestone 7 plan's multi-field-struct-return finding), has no such
  // COPY - and unconditionally marking GP0 live-in regardless (this
  // function's original Milestone 4 behavior) collides with
  // RegAllocFast::reloadAtBegin: it treats *any* physical register a block
  // declares live-in as already holding a valid value, silently skipping
  // the reload of any OTHER virtual register that also happens to have
  // been allocated that exact physical register before the call - e.g.
  // FIADDR's hidden-return-pointer result, when it's passed as this call's
  // own first argument in GP0 and is also needed again afterward. This was
  // discovered via a real, reproducible wrong-answer bug (not a crash),
  // not a hypothetical - see struct_abi_test.ll's excluded multi-field-
  // struct-return case before this fix.
  bool ReturnValueConsumed = false;
  for (MachineInstr &Later : *ContinueBB) {
    if (Later.isCopy() && Later.getOperand(1).isReg() &&
        Later.getOperand(1).getReg() == Moe::GP0) {
      ReturnValueConsumed = true;
      break;
    }
  }
  if (ReturnValueConsumed)
    ContinueBB->addLiveIn(Moe::GP0);

  // ContinueBB has exactly one predecessor (BB) and is reached purely by
  // fallthrough from the AsmPrinter's point of view (JMPabs is deliberately
  // not a terminator - see JMPabs's own def comment), so
  // shouldEmitLabelForBasicBlock would normally skip emitting its label
  // entirely. Force it: the callee's return sequence jumps to this exact
  // address, so the label must exist in the final assembly even though
  // nothing else branches to it.
  ContinueBB->setLabelMustBeEmitted();
  // The label's only real use (below) is as a raw data value in a constant
  // pool entry - invisible to the normal "is this MBB referenced anywhere"
  // scan every CFG-cleanup pass relies on. At -O0 none of those passes run,
  // so this was invisible; at -O1+, BranchFolding sees a trivial single-
  // predecessor fallthrough block and merges/deletes it, leaving the
  // constant pool's LOADabs referencing a symbol that's never emitted
  // ("Undefined temporary symbol .LBBn_m" from the MC assembler - confirmed
  // via llc -O1 -filetype=obj on every existing test with a real CALL).
  // setMachineBlockAddressTaken() is the standard flag BranchFolding,
  // TailDuplicator, CodeGenPrepare, IfConversion, GCEmptyBasicBlocks,
  // MachineOutliner and BasicBlockPathCloning all check before touching a
  // block for exactly this reason.
  ContinueBB->setMachineBlockAddressTaken();
  MCSymbol *RetSym = ContinueBB->getSymbol();

  // LOADabs always dereferences its trailing address operand (loads
  // memory[addr], not addr itself - see the spec's LOAD section), so getting
  // RetSym's address as a *value* into GP4 needs the same pool indirection
  // every other constant goes through (LowerConstantPool/LowerGlobalAddress):
  // a pool slot holding RetSym's address, read through by LOADabs.
  Type *PtrTy = Type::getInt32Ty(MF->getFunction().getContext());
  MoeConstantPoolValue *CPV = MoeConstantPoolValue::Create(PtrTy, RetSym);
  unsigned CPIdx = MF->getConstantPool()->getConstantPoolIndex(CPV, Align(4));

  // LOAD.W [pool entry containing RetSym] -> GP4 ; PUSH.W GP4 ; JUMP.AL [callee]
  // ; ContinueBB (RetSym):
  BuildMI(*BB, MI, DL, TII->get(Moe::LOADabs), Moe::GP4)
      .addConstantPoolIndex(CPIdx);
  BuildMI(*BB, MI, DL, TII->get(Moe::PUSH)).addReg(Moe::GP4, RegState::Kill);
  MachineInstrBuilder MIB =
      BuildMI(*BB, MI, DL, TII->get(Moe::JMPabs)).add(MI.getOperand(0));
  // Preserve the implicit arg-register uses the variadic Moecall pattern
  // attached to the pseudo, so they stay live across the expansion.
  for (unsigned i = 1, e = MI.getNumOperands(); i != e; ++i)
    MIB.add(MI.getOperand(i));
  // Every call defines GP0 (RetCC_Moe assigns every i32 return there,
  // unconditionally - even a void call's callee still executes a RET that
  // restores GP0 to *some* value), but nothing on this pseudo said so before
  // now: LowerCallResult's `COPY $gp0` right after the call relied on GP0
  // merely *looking* defined from an earlier argument load (the verifier's
  // linear scan doesn't know the call clobbers it in between) - which broke
  // for a zero-argument call, where GP0 was never touched at all before the
  // read. Declaring the real semantic fact directly fixes both cases.
  //
  // GP1-GP3 need the same treatment, for a related but distinct reason: they
  // (like GP0) are caller-saved per the ABI (only GP6/GP7 are callee-saved -
  // see MoeRegisterInfo::getCalleeSavedRegs), so the callee is free to use
  // them as scratch regardless of whether THIS call site happened to pass an
  // argument through them - LowerCall only ever attached them as implicit
  // *uses* (the argument values going in), never as clobbers. RegAllocFast
  // never noticed: it reloads every value from its spill slot before each
  // use regardless of clobber info, so an incomplete clobber list was
  // invisible at -O0. The Greedy allocator (-O1+) trusts clobber info to
  // decide what's safe to keep live across a call in a register - without
  // this, it could leave some other still-needed value sitting in GP1/GP2/
  // GP3 across the call, for the callee to silently stomp on. Confirmed via
  // a real, reproducible wrong-answer bug (not a crash) on cross_call_test.ll
  // at -O1 - GP0 read back as 0 instead of the second call's real result.
  for (unsigned Reg : {Moe::GP0, Moe::GP1, Moe::GP2, Moe::GP3})
    MIB.addReg(Reg, RegState::ImplicitDefine);

  MI.eraseFromParent();
  return ContinueBB;
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
// Software variable-amount shift - same runtime-loop shape as emitMul, but
// simpler (no conditional add step: every iteration does the same
// unconditional single-bit shift) and, critically, WHILE-shaped rather than
// emitMul/emitDivRem's do-while shape: a runtime shift amount of 0 is valid
// and common and must produce the input unchanged, so the counter-vs-amount
// check must happen before any shift executes, not after (emitMul's
// do-while shape is only safe there because MUL/DIV always run exactly 32
// iterations regardless of operand value).
//
// Blocks (BB is VARSHIFTPSEUDO's parent block, split at the pseudo):
//   BB:         value/amount copies (Value0, Amt0); counter seeded to 0 via
//               self-XOR (same trick emitMul's BB uses); falls through to
//               LoopBB. Amt0 is loop-invariant, so it's referenced directly
//               by LoopBB rather than threaded through a PHI (the same way
//               emitMul's Bound is referenced directly by ContinueBB).
//   LoopBB:     PHI-merges ValuePhi/CounterPhi (back-edge values defined in
//               ContinueBB); compares CounterPhi against Amt0 - if equal,
//               the requested number of shifts has already happened, so
//               branch to ExitBB (this is what makes amt=0 correct: on the
//               very first pass through LoopBB, CounterPhi is still 0, so
//               an amt of 0 branches straight to ExitBB without ever
//               reaching ContinueBB); otherwise falls through to ContinueBB.
//   ContinueBB: one unconditional single-bit shift (Moe::SHL/SRL/SRA,
//               selected by the pseudo's $opc operand) on ValuePhi ->
//               ValueNext; increments CounterPhi by 1 -> CounterNext;
//               unconditional jump back to LoopBB.
//   ExitBB:     receives VARSHIFTPSEUDO's original successors; copies
//               ValuePhi (the value as of the moment the counter reached
//               the requested amount) into the pseudo's original
//               destination register.
//===----------------------------------------------------------------------===//

MachineBasicBlock *MoeTargetLowering::emitVarShift(MachineInstr &MI,
                                                    MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *RC = &Moe::GPRRegClass;

  Register DstReg = MI.getOperand(0).getReg();
  Register ValReg = MI.getOperand(1).getReg();
  Register AmtReg = MI.getOperand(2).getReg();
  unsigned ShiftOpc = MI.getOperand(3).getImm();

  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++BB->getIterator();
  MachineBasicBlock *LoopBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ContinueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, LoopBB);
  MF->insert(InsertPt, ContinueBB);
  MF->insert(InsertPt, ExitBB);

  ExitBB->splice(ExitBB->begin(), BB,
                 std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitBB->transferSuccessorsAndUpdatePHIs(BB);

  BB->addSuccessor(LoopBB);
  LoopBB->addSuccessor(ExitBB);     // counter == amount: done
  LoopBB->addSuccessor(ContinueBB); // counter != amount: fall through, shift
  ContinueBB->addSuccessor(LoopBB); // always loop back

  Register Value0 = MRI.createVirtualRegister(RC);
  Register Amt0 = MRI.createVirtualRegister(RC);
  Register Counter0 = MRI.createVirtualRegister(RC);

  Register ValuePhi = MRI.createVirtualRegister(RC);
  Register CounterPhi = MRI.createVirtualRegister(RC);
  Register ValueNext = MRI.createVirtualRegister(RC);
  Register CounterNext = MRI.createVirtualRegister(RC);

  // BB
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Value0).addReg(ValReg);
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Amt0).addReg(AmtReg);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), Counter0)
      .addReg(Value0)
      .addReg(Value0);

  // LoopBB
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), ValuePhi)
      .addReg(Value0).addMBB(BB)
      .addReg(ValueNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), CounterPhi)
      .addReg(Counter0).addMBB(BB)
      .addReg(CounterNext).addMBB(ContinueBB);
  BuildMI(LoopBB, DL, TII->get(Moe::SUBcmp)).addReg(CounterPhi).addReg(Amt0);
  BuildMI(LoopBB, DL, TII->get(Moe::JCC))
      .addMBB(ExitBB)
      .addImm(MoeCC::COND_EQ);

  // ContinueBB
  BuildMI(ContinueBB, DL, TII->get(ShiftOpc), ValueNext).addReg(ValuePhi);
  BuildMI(ContinueBB, DL, TII->get(Moe::INCREMENT), CounterNext)
      .addReg(CounterPhi)
      .addImm(1);
  BuildMI(ContinueBB, DL, TII->get(Moe::JMP)).addMBB(LoopBB);

  // ExitBB: LoopBB is ExitBB's only predecessor, so a plain COPY suffices -
  // no PHI needed for a single incoming value.
  BuildMI(*ExitBB, ExitBB->begin(), DL, TII->get(TargetOpcode::COPY), DstReg)
      .addReg(ValuePhi);

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
// Value-producing compare (Milestone 11) - see SETCCPSEUDO's def comment in
// MoeInstrInfo.td. Same skip/fall-through/merge shape as emitCondNegate:
// *BB ends with SUBcmp+JCC branching to TrueBB (condition holds) and falling
// through to FalseBB (condition doesn't hold, placed immediately after *BB
// in insertion order so the fallthrough is real) - both set the result to a
// self-XOR-then-optionally-INCREMENT 0/1 (the same "self-zero" and
// tied-operand-needs-a-distinct-seed tricks emitMul's Bound/BoundZero
// already use) and merge into DstReg via a PHI. Unlike emitCondNegate (a
// helper called mid-block by another custom-inserter), this IS a pseudo-
// dispatch target in its own right, so it splices/transfers *MI's original
// block's remainder and successors the same way emitMul/emitCall do.
//===----------------------------------------------------------------------===//

MachineBasicBlock *MoeTargetLowering::emitSetCC(MachineInstr &MI,
                                                 MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *RC = &Moe::GPRRegClass;

  Register DstReg = MI.getOperand(0).getReg();
  Register AReg = MI.getOperand(1).getReg();
  Register BReg = MI.getOperand(2).getReg();
  int64_t Cond = MI.getOperand(3).getImm();

  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++BB->getIterator();
  // Insertion order matters: FalseBB must land immediately after BB so BB's
  // JCC (which explicitly branches to TrueBB, and otherwise falls through)
  // actually falls through to it - same convention emitMul's LoopBB/AddBB
  // ordering already relies on.
  MachineBasicBlock *FalseBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TrueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *MergeBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, FalseBB);
  MF->insert(InsertPt, TrueBB);
  MF->insert(InsertPt, MergeBB);

  MergeBB->splice(MergeBB->begin(), BB,
                   std::next(MachineBasicBlock::iterator(MI)), BB->end());
  MergeBB->transferSuccessorsAndUpdatePHIs(BB);

  BB->addSuccessor(TrueBB);   // condition holds
  BB->addSuccessor(FalseBB);  // condition doesn't hold: fall through
  FalseBB->addSuccessor(MergeBB);
  TrueBB->addSuccessor(MergeBB);

  Register ZeroReg = MRI.createVirtualRegister(RC);
  Register OneSeed = MRI.createVirtualRegister(RC);
  Register OneReg = MRI.createVirtualRegister(RC);

  // ZeroReg is computed in *BB, BEFORE SUBcmp - not in FalseBB - and FalseBB
  // is left with nothing but its terminator. This isn't just tidiness: a
  // non-terminator instruction sitting alone in FalseBB (BB's sole
  // fallthrough successor) is exactly what a trivial-block-merge pass (seen
  // in practice: BranchFolding, part of the -O1+ pipeline) hoists back into
  // BB - and since XOR's real flag-clobbering side effect is (like every
  // ALU op except SUBcmp/SHIFT) undeclared, nothing stops it from landing
  // between SUBcmp and the JCC that reads its flags, silently corrupting the
  // comparison. Confirmed via a real, reproducible wrong-answer bug at -O1:
  // exactly this hoist happened, and JCC ended up testing XOR's
  // always-zero-flags instead of SUBcmp's. Computing ZeroReg first sidesteps
  // the hazard entirely - nothing is left in FalseBB for any pass to hoist.
  BuildMI(*BB, BB->end(), DL, TII->get(Moe::XOR), ZeroReg)
      .addReg(AReg)
      .addReg(AReg);
  BuildMI(*BB, BB->end(), DL, TII->get(Moe::SUBcmp)).addReg(AReg).addReg(BReg);
  BuildMI(*BB, BB->end(), DL, TII->get(Moe::JCC)).addMBB(TrueBB).addImm(Cond);

  // FalseBB is placed immediately before TrueBB (not MergeBB), so - unlike
  // AddBB's plain fallthrough in emitMul, which has nothing else in between
  // - it needs an explicit unconditional jump to skip over TrueBB and reach
  // MergeBB. Real Moe::JMP, not JCC+COND_AL: MoeInstrInfo::analyzeBranch
  // (which MachineBlockPlacement/BranchFolding depend on) only recognizes
  // the dedicated JMP opcode as unconditional - a JCC, any condition
  // included, is always treated as conditional-with-an-implicit-fallthrough-
  // successor, which FalseBB doesn't have (its only real successor is
  // MergeBB) - confirmed via a real crash: MachineBlockPlacement's
  // updateTerminator asserting isSuccessor(PreviousLayoutSuccessor) at -O1+.
  BuildMI(FalseBB, DL, TII->get(Moe::JMP)).addMBB(MergeBB);

  // OneSeed/OneReg must be distinct SSA values - INCREMENT's tied "$o = $oin"
  // constraint needs oin to differ from the fresh result it defines (see
  // emitMul's Bound/BoundZero comment).
  BuildMI(TrueBB, DL, TII->get(Moe::XOR), OneSeed).addReg(AReg).addReg(AReg);
  BuildMI(TrueBB, DL, TII->get(Moe::INCREMENT), OneReg)
      .addReg(OneSeed)
      .addImm(1);

  BuildMI(*MergeBB, MergeBB->begin(), DL, TII->get(TargetOpcode::PHI), DstReg)
      .addReg(ZeroReg).addMBB(FalseBB)
      .addReg(OneReg).addMBB(TrueBB);

  MI.eraseFromParent();
  return MergeBB;
}

//===----------------------------------------------------------------------===//
// Value-producing select-on-compare (Milestone 12) - see SELECTCCPSEUDO's
// def comment in MoeInstrInfo.td. Same shape as emitSetCC, simplified: no
// value needs computing in TrueBB/FalseBB (True/False are already-available
// operands), so both are empty except their terminator - just enough to give
// the PHI two distinct predecessor edges to merge through.
//===----------------------------------------------------------------------===//

MachineBasicBlock *MoeTargetLowering::emitSelectCC(MachineInstr &MI,
                                                    MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();

  Register DstReg = MI.getOperand(0).getReg();
  Register AReg = MI.getOperand(1).getReg();
  Register BReg = MI.getOperand(2).getReg();
  Register TrueReg = MI.getOperand(3).getReg();
  Register FalseReg = MI.getOperand(4).getReg();
  int64_t Cond = MI.getOperand(5).getImm();

  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator InsertPt = ++BB->getIterator();
  // Same insertion-order/fallthrough convention as emitSetCC.
  MachineBasicBlock *FalseBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TrueBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *MergeBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(InsertPt, FalseBB);
  MF->insert(InsertPt, TrueBB);
  MF->insert(InsertPt, MergeBB);

  MergeBB->splice(MergeBB->begin(), BB,
                   std::next(MachineBasicBlock::iterator(MI)), BB->end());
  MergeBB->transferSuccessorsAndUpdatePHIs(BB);

  BB->addSuccessor(TrueBB);   // condition holds
  BB->addSuccessor(FalseBB);  // condition doesn't hold: fall through
  FalseBB->addSuccessor(MergeBB);
  TrueBB->addSuccessor(MergeBB);

  BuildMI(*BB, BB->end(), DL, TII->get(Moe::SUBcmp)).addReg(AReg).addReg(BReg);
  BuildMI(*BB, BB->end(), DL, TII->get(Moe::JCC)).addMBB(TrueBB).addImm(Cond);

  // Real Moe::JMP, not JCC+COND_AL - see emitSetCC's identical comment.
  BuildMI(FalseBB, DL, TII->get(Moe::JMP)).addMBB(MergeBB);

  BuildMI(*MergeBB, MergeBB->begin(), DL, TII->get(TargetOpcode::PHI), DstReg)
      .addReg(FalseReg).addMBB(FalseBB)
      .addReg(TrueReg).addMBB(TrueBB);

  MI.eraseFromParent();
  return MergeBB;
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
