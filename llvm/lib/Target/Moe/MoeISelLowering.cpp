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
#include "llvm/CodeGen/MachineConstantPool.h"
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

  // Atomics. The ISA has no atomic read-modify-write instruction of any
  // width, and it never will: on a backplane of discrete TTL there is no
  // second processor to be atomic against, and the one hazard that remains -
  // an interrupt landing mid-sequence - cannot be closed by userspace, which
  // cannot mask interrupts.
  //
  // So every atomic becomes an __atomic_* libcall. Zero here (rather than 8,
  // 16 or 32) is what makes AtomicExpandPass expand *all* of them, including
  // the byte-sized ones: there is no width this target can do inline.
  //
  // The libcalls are implemented in runtime/atomic.ll over the
  // kernel's kuser-page compare-and-swap, which is a restartable sequence
  // rather than an instruction - see psabi.md, 'The kuser page'. Nothing
  // implements the 8-byte forms, so a consumer that asks for one gets a link
  // error rather than something quiet; Rust is stopped from asking by its
  // target spec's max_atomic_width.
  setMaxAtomicSizeInBitsSupported(0);

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
  // The address of a label, from GCC's `&&label` extension - which the kernel
  // uses in _THIS_IP_, and therefore in every lockdep and tracing macro.
  setOperationAction(ISD::BlockAddress, MVT::i32, Custom);
  // __builtin_return_address, which the kernel reaches through _RET_IP_ in
  // every lock, allocator and tracing path.
  setOperationAction(ISD::RETURNADDR, MVT::i32, Custom);
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

  // A bare SELECT (a condition already materialised as a 0/1 value, rather
  // than a comparison the branch can read flags from) was left Legal, which
  // means "the target handles this" - and nothing did, so it reached
  // instruction selection and crashed with "Cannot select: ... = select".
  // Nothing in llvm-tests/ produced one; the shape that does is ordinary
  // kernel C where a condition is computed once and used to pick between two
  // values, which is how the Linux build found it.
  //
  // Expand is exactly right here and does not contradict the note above:
  // LegalizeDAG's expansion of SELECT rewrites it as SELECT_CC comparing the
  // condition against zero, and SELECT_CC is Custom. The constraint that
  // paragraph describes runs the other way - it is expanding SELECT_CC that
  // would need a SELECT to lower through.
  setOperationAction(ISD::SELECT, MVT::i32, Expand);

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
  // Variable-sized stack objects (alloca with a runtime size, and C99 VLAs)
  // are rejected rather than lowered. The generic expansion is a plain "SP -=
  // size", which on Moe silently destroys the function: there is no frame
  // pointer (see MoeFrameLowering::hasFPImpl), so nothing ever puts SP back,
  // and the epilogue's POP then reads the return address from inside the
  // allocation and returns to garbage. Confirmed by looking at the generated
  // code - the SP adjustment appears and no matching restore ever does.
  //
  // Supporting it properly means a frame pointer, which costs one of only six
  // allocatable registers (GP4/GP5 are reserved scratch) for a feature the
  // Linux kernel forbids in its own code anyway. Until something needs it, a
  // diagnostic is strictly better than a wrong answer.
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Custom);

  //===--------------------------------------------------------------------===//
  // Operations Moe has no instruction for, and no intention of growing one.
  //
  // Every one of these was left at its default of Legal, which claims the
  // target handles it - so each reached instruction selection and crashed with
  // "Cannot select". None of them was reachable from anything in llvm-tests/;
  // they turned up building the Linux kernel, which is simply a far larger
  // body of ordinary C than this backend had ever been pointed at.
  //
  // Expand is right for all of them rather than a libcall: the generic
  // expansions are built from shifts, masks and adds, which is what this
  // machine has, and a libcall would mean a runtime this kernel does not link
  // against.
  //===--------------------------------------------------------------------===//

  // Bit counting. There is no instruction for any of these - which is also why
  // Kconfig selects CPU_NO_EFFICIENT_FFS, so generic code prefers algorithms
  // that do not lean on them.
  for (auto Op : {ISD::CTTZ, ISD::CTTZ_ZERO_UNDEF, ISD::CTLZ,
                  ISD::CTLZ_ZERO_UNDEF, ISD::CTPOP, ISD::BITREVERSE})
    setOperationAction(Op, MVT::i32, Expand);

  // Byte swapping: memory is little-endian and stays that way ('Endianness' in
  // the Encoding chapter), so a swap is only ever an explicit one.
  setOperationAction(ISD::BSWAP, MVT::i32, Expand);

  // Rotates. SHIFT moves exactly one bit and has no rotate mode; the carry-in
  // form chains a shift across words rather than around one.
  setOperationAction(ISD::ROTL, MVT::i32, Expand);
  setOperationAction(ISD::ROTR, MVT::i32, Expand);

  // Min/max/abs are comparisons plus a select, which is what Expand produces.
  for (auto Op : {ISD::SMIN, ISD::SMAX, ISD::UMIN, ISD::UMAX, ISD::ABS})
    setOperationAction(Op, MVT::i32, Expand);

  // Combined divide-and-remainder: Moe computes both in one loop already (see
  // DIVMODPSEUDO), but through the separate SDIV/UDIV/SREM/UREM nodes, so the
  // combined form has to decompose into those.
  for (auto Op : {ISD::SDIVREM, ISD::UDIVREM})
    setOperationAction(Op, MVT::i32, Expand);

  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction(ISD::VAARG, MVT::Other, Expand);
  // va_copy. For a plain-pointer va_list there is nothing to it but copying
  // the pointer, which is exactly what Expand produces.
  setOperationAction(ISD::VACOPY, MVT::Other, Expand);
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

  // An i1 in memory occupies a whole byte, so a load of one is a byte load
  // that then cares about a single bit - which is what Promote does, turning
  // it into the i8 case handled above. Left at its Legal default it reached
  // instruction selection as a load of a type no instruction has, and crashed;
  // C's _Bool is where it comes from, which is why nothing in llvm-tests/
  // produced one and the kernel produces them constantly.
  for (auto ExtType : {ISD::ZEXTLOAD, ISD::SEXTLOAD, ISD::EXTLOAD})
    setLoadExtAction(ExtType, MVT::i32, MVT::i1, Promote);

  // DAGCombiner can reuse an existing zextload's value for a sextload of
  // the same address (rather than emitting a second load) by wrapping it in
  // a SIGN_EXTEND_INREG - a distinct ISD opcode from the LOAD node
  // LowerExtLoad handles above, and one this target has never needed until
  // now. Expand is the standard generic lowering (shift left then
  // arithmetic-shift-right by the same amount) and, since SHL/SRA are
  // already Custom-lowered via LowerShifts's constant-amount chain above,
  // this reuses that existing, already-working machinery for free.
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);
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
  case ISD::BlockAddress:
    return LowerBlockAddress(Op, DAG);
  case ISD::RETURNADDR:
    return LowerRETURNADDR(Op, DAG);
  case ISD::BR_CC:
    return LowerBR_CC(Op, DAG);
  case ISD::SETCC:
    return LowerSETCC(Op, DAG);
  case ISD::SELECT_CC:
    return LowerSELECT_CC(Op, DAG);
  case ISD::VASTART:
    return LowerVASTART(Op, DAG);
  case ISD::DYNAMIC_STACKALLOC:
    return LowerDYNAMIC_STACKALLOC(Op, DAG);
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
    // Naming the node is the difference between a five-minute fix and an
    // afternoon: "unimplemented operand" on its own says nothing about which
    // one, and the shapes that reach here come from code far away.
    report_fatal_error(Twine("Moe: no lowering for SelectionDAG node '") +
                       Op->getOperationName(&DAG) + "'");
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

// The address of a basic block, as a value. Same shape as LowerGlobalAddress
// and for the same reason: Moe cannot materialise any address except by
// reading it out of memory, so the block's address goes into a constant pool
// entry and is loaded from there. A BlockAddress is itself an LLVM Constant,
// so it drops straight into a pool entry, and the AsmPrinter emits it as a
// reference to the block's label.
SDValue MoeTargetLowering::LowerBlockAddress(SDValue Op,
                                              SelectionDAG &DAG) const {
  auto *BAN = cast<BlockAddressSDNode>(Op);
  const BlockAddress *BA = BAN->getBlockAddress();
  int64_t Offset = BAN->getOffset();
  EVT PtrVT = Op.getValueType();
  SDLoc dl(Op);

  const Constant *C = BA;
  if (Offset != 0)
    C = ConstantExpr::getGetElementPtr(
        Type::getInt8Ty(*DAG.getContext()), const_cast<BlockAddress *>(BA),
        ConstantInt::get(Type::getInt32Ty(*DAG.getContext()), Offset));

  SDValue CPIdx = DAG.getTargetConstantPool(C, PtrVT, Align(4));
  SDValue Wrapper = DAG.getNode(MoeISD::Wrapper, dl, PtrVT, CPIdx);
  return DAG.getLoad(
      PtrVT, dl, DAG.getEntryNode(), Wrapper,
      MachinePointerInfo::getConstantPool(DAG.getMachineFunction()));
}

// __builtin_return_address(depth).
//
// Depth 0 is easy and exact: Moe has no call instruction, so the caller pushes
// the return address itself and the callee's entry SP points straight at it
// (see psabi.md's call sequence). A fixed stack object at offset 0 from entry
// SP names that word, and eliminateFrameIndex resolves it against the final
// frame size the same way it resolves an incoming stack argument.
//
// Any greater depth returns zero. Walking further needs a frame pointer to
// find the caller's frame, and there is none - one of only six allocatable
// registers is too high a price for it (psabi.md's stack section). Zero is
// what the generic code already treats as "no further frames", so callers
// degrade to a shorter backtrace rather than a wrong one.
SDValue MoeTargetLowering::LowerRETURNADDR(SDValue Op,
                                            SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  EVT VT = Op.getValueType();
  SDLoc dl(Op);

  MFI.setReturnAddressIsTaken(true);

  if (Op.getConstantOperandVal(0) != 0)
    return DAG.getConstant(0, dl, VT);

  int FI = MFI.CreateFixedObject(4, 0, true);
  SDValue FIN = DAG.getFrameIndex(FI, getFrameIndexTy(DAG.getDataLayout()));
  return DAG.getLoad(VT, dl, DAG.getEntryNode(), FIN,
                     MachinePointerInfo::getFixedStack(MF, FI));
}

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

  if (isa<ConstantSDNode>(Op)) {
    // A constant VALUE, which this target has to keep in memory because there
    // is no load-immediate: `add i32 %x, 5` needs the number 5, and LOADabs
    // reads it out of the pool entry. The load is invariant constant-pool
    // data, so DAG.getEntryNode() is a safe chain.
    SDValue CPIdx = DAG.getTargetConstantPool(C, PtrVT, Alignment);
    SDValue Wrapper = DAG.getNode(MoeISD::Wrapper, dl, PtrVT, CPIdx);
    return DAG.getLoad(
        PtrVT, dl, DAG.getEntryNode(), Wrapper,
        MachinePointerInfo::getConstantPool(DAG.getMachineFunction()));
  }

  // An ISD::ConstantPool node, on the other hand, IS an address: the thing in
  // the pool is an aggregate - a jump table of bytes, a lookup table, a
  // spilled vector constant - and what the code wants is where it lives, to
  // index into it. Returning its first word instead, which is what this used
  // to do for every case alike, is a silent wrong answer: musl's malloc
  // indexes a 32-byte de Bruijn table this way and dereferenced its contents
  // as a pointer.
  //
  // Materializing that address needs a second pool entry holding it, because
  // LOADabs always dereferences. Nothing in LLVM's Constant hierarchy can say
  // "the address of pool entry 3", so the entry is a MoeConstantPoolValue
  // naming that index, and the AsmPrinter resolves it to the label it gives
  // that entry.
  MachineFunction &MF = DAG.getMachineFunction();
  unsigned CPI = MF.getConstantPool()->getConstantPoolIndex(C, Alignment);
  MoeConstantPoolValue *CPV = MoeConstantPoolValue::CreateCPIRef(
      PointerType::getUnqual(*DAG.getContext()), CPI);
  SDValue AddrIdx = DAG.getTargetConstantPool(CPV, PtrVT, Align(4));
  SDValue Wrapper = DAG.getNode(MoeISD::Wrapper, dl, PtrVT, AddrIdx);
  return DAG.getLoad(
      PtrVT, dl, DAG.getEntryNode(), Wrapper,
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

//===----------------------------------------------------------------------===//
// Variable-sized stack allocation - alloca with a runtime size, and the C99
// variable-length arrays built on it.
//
// SP moves down by the (word-rounded) size and the new SP is the result. What
// makes that safe is the frame pointer MoeFrameLowering gives exactly the
// functions that do this: the locals stay addressable from it while SP is
// somewhere unknown, and the epilogue restores SP from it rather than by
// adding back a size nothing recorded.
//
// This used to be a hard error, on the reasoning that the Linux kernel forbids
// variable-length arrays in its own code. A libc does not: musl uses them in
// seven files, execl and getcwd among them, and no amount of psABI wording
// makes those go away.
//===----------------------------------------------------------------------===//

SDValue MoeTargetLowering::LowerDYNAMIC_STACKALLOC(SDValue Op,
                                                    SelectionDAG &DAG) const {
  SDLoc dl(Op);
  EVT VT = Op.getValueType();
  SDValue Chain = Op.getOperand(0);
  SDValue Size = Op.getOperand(1);
  MaybeAlign Alignment(Op.getConstantOperandVal(2));

  // The stack is word-aligned at all times (psabi.md), and PUSH/POP move SP by
  // a word whatever they transfer, so an allocation that left SP misaligned
  // would break the next call rather than this allocation.
  Align StackAlign =
      DAG.getSubtarget().getFrameLowering()->getStackAlign();
  Align Wanted = std::max(StackAlign, Alignment.valueOrOne());

  SDValue SP = DAG.getCopyFromReg(Chain, dl, Moe::SP, VT);
  SDValue Allocated = DAG.getNode(ISD::SUB, dl, VT, SP, Size);
  if (Wanted > Align(1))
    Allocated = DAG.getNode(
        ISD::AND, dl, VT, Allocated,
        DAG.getSignedConstant(-(int64_t)Wanted.value(), dl, VT));

  Chain = DAG.getCopyToReg(SP.getValue(1), dl, Moe::SP, Allocated);
  return DAG.getMergeValues({Allocated, Chain}, dl);
}

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
// Inline asm (Milestone 15) - only "r" (any GP register), modeled directly
// on MSP430TargetLowering's identically-shaped implementation. clang's
// generic TargetInfo::validateAsmConstraint already recognizes 'r'
// universally before ever reaching Moe's own (always-false) override - see
// clang/lib/Basic/TargetInfo.cpp - so no clang-side change was needed,
// confirmed empirically: compiling real inline-asm C reached this backend
// code, failing only with "couldn't allocate output register for
// constraint 'r'" until these two overrides existed.
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
MoeTargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return C_RegisterClass;
    default:
      break;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass *>
MoeTargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                                 StringRef Constraint,
                                                 MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return std::make_pair(0U, &Moe::GPRRegClass);
    default:
      break;
    }
  }
  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
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

  if (isVarArg) {
    // Every argument of a variadic function is passed on the stack, named ones
    // included (see CC_Moe for why a register-save area cannot work here), so
    // the unnamed arguments sit contiguously right after the named ones in the
    // caller's outgoing area. va_start's pointer is therefore just the first
    // word past the named parameters, and va_arg is a plain pointer walk -
    // which is what makes a variadic call with more than four total arguments
    // work at all. It previously read back garbage from the fifth onwards.
    //
    // The +4 is the same one the stack-argument loads above need: entry SP
    // points at the return address the caller pushed, one word below where its
    // outgoing arguments start.
    MachineFrameInfo &MFI = MF.getFrameInfo();
    int VarArgsFI = MFI.CreateFixedObject(4, CCInfo.getStackSize() + 4, true);
    MF.getInfo<MoeMachineFunctionInfo>()->setVarArgsFrameIndex(VarArgsFI);
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
  // A direct callee becomes a Target* node so legalization leaves it alone. An
  // indirect one is an ordinary i32 value in a register and is passed through
  // untouched - the CALLreg pattern matches on that, and emitCall ends the
  // sequence with MOVE -> IA instead of JUMP.
  //
  // Indirect calls used to be a hard error here. Nothing in llvm-tests/ made
  // one; the Linux kernel makes almost nothing else, since every driver,
  // filesystem and subsystem is reached through a struct full of function
  // pointers.
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, MVT::i32);
  else if (auto *ES = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(ES->getSymbol(), MVT::i32);

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
  case Moe::CALLreg:
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
  MachineFunction *MF = BB->getParent();
  DebugLoc DL = MI.getDebugLoc();

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

  // Only declare GP0 live-in here if THIS call's return value is actually
  // consumed - i.e. ContinueBB (just spliced in above) contains
  // LowerCallResult's `COPY $gp0` reading it, before any OTHER call. A call
  // whose LLVM-level return type is void, or was demoted to a hidden sret
  // pointer (see the Milestone 7 plan's multi-field-struct-return finding),
  // has no such COPY - and unconditionally marking GP0 live-in regardless
  // (this function's original Milestone 4 behavior) collides with
  // RegAllocFast::reloadAtBegin: it treats *any* physical register a block
  // declares live-in as already holding a valid value, silently skipping
  // the reload of any OTHER virtual register that also happens to have
  // been allocated that exact physical register before the call - e.g.
  // FIADDR's hidden-return-pointer result, when it's passed as this call's
  // own first argument in GP0 and is also needed again afterward. This was
  // discovered via a real, reproducible wrong-answer bug (not a crash),
  // not a hypothetical - see struct_abi_test.ll's excluded multi-field-
  // struct-return case before this fix.
  //
  // Milestone 18 found this scan needs to STOP at the next CALL pseudo, not
  // scan the whole rest of ContinueBB: at the point any one CALL gets
  // expanded, ContinueBB (freshly spliced from the single original block)
  // still contains every LATER statement's code too, including any FURTHER,
  // unrelated call's own `COPY $gp0` reading *its* return value - the
  // original unscoped scan found that later COPY and wrongly concluded
  // *this* call's result was consumed, marking GP0 live-in and making
  // RegAllocFast skip reloading a same-named-register value (like an
  // sret pointer) that's actually still needed after THIS call returns.
  // Reproduced by a real wrong-answer bug (not a crash) once a
  // sret-demoted call (any function returning a type wider than one
  // register, e.g. `double`) was immediately followed, in the same
  // originating block, by a further call whose own result IS a plain
  // register return - see f64_test.c's chained-double-then-int-cast shape
  // and run-f64-test.sh.
  bool ReturnValueConsumed = false;
  for (MachineInstr &Later : *ContinueBB) {
    if (Later.getOpcode() == Moe::CALL || Later.getOpcode() == Moe::CALLreg)
      break;
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

  // The sequence itself - LOADabs, PUSH, jump - is NOT built here. It is built
  // by MoeInstrInfo::expandCall, after register allocation, and that split is
  // deliberate: see that function for the wrong-answer bug that came of
  // leaving the middle of a call available for the allocator to put spill code
  // into.
  //
  // What happens here is the block split, which has to happen during ISel so
  // the continuation exists as a real MachineBasicBlock for the allocator to
  // reason about, and swapping the pseudo for one that carries the pool slot
  // holding the continuation's address.
  Type *PtrTy = Type::getInt32Ty(MF->getFunction().getContext());
  MoeConstantPoolValue *CPV = MoeConstantPoolValue::Create(PtrTy, RetSym);
  unsigned CPIdx = MF->getConstantPool()->getConstantPoolIndex(CPV, Align(4));

  bool IsIndirect = MI.getOpcode() == Moe::CALLreg;
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
  MachineInstrBuilder MIB =
      BuildMI(*BB, MI, DL,
              TII->get(IsIndirect ? Moe::CALLSEQreg : Moe::CALLSEQ));
  if (IsIndirect)
    MIB.addReg(MI.getOperand(0).getReg());
  else
    MIB.add(MI.getOperand(0));
  MIB.addConstantPoolIndex(CPIdx);
  // Everything LowerCall attached - the argument registers going in.
  for (unsigned i = 1, e = MI.getNumOperands(); i != e; ++i)
    MIB.add(MI.getOperand(i));

  MI.eraseFromParent();
  return ContinueBB;
}

//===----------------------------------------------------------------------===//
// Software multiply - SHIFT moves exactly one bit per instruction (see the
// spec's SHIFT section) and its carry-out is the bit shifted out, so each
// multiplier bit is tested directly off SHIFT's C flag, with no AND and no
// mask constant.
//
// The loop runs once per bit of the multiplier's *length*, not 32 times: the
// same SHIFT that puts the low bit in C also sets Z when what is left of the
// multiplier is zero, and a JUMP reads flags without changing them, so a
// chain of conditional jumps can act on both. That matters more than it
// looks. The kernel multiplies by small constants and by 16-bit halves all
// the time (every 64-bit multiply is built from four 16x16 ones), and a
// fixed 32-iteration loop spent over a thousand instructions on each 64-bit
// multiply - enough, with the other 64-bit helpers, for a single timer
// interrupt to outlast the timer period.
//
// Blocks (BB is MULPSEUDO's parent block, split at the pseudo):
//   BB:         multiplicand/multiplier copies, result seeded to zero via
//               self-XOR (there is no load-immediate). Falls through to
//               LoopBB.
//   LoopBB:     shift multiplier right by 1: C = the bit shifted out, Z = the
//               rest is zero. C=1 and Z=0 jumps to AddLoopBB.
//   TestAddBB:  C=1 (so Z=1): jumps to AddExitBB.
//   TestExitBB: C=0 and Z=1: jumps to ExitBB.
//   SkipBB:     C=0 and Z=0: shift multiplicand left, back to LoopBB.
//   AddLoopBB:  result += multiplicand, shift multiplicand left, back to
//               LoopBB.
//   AddExitBB:  result += multiplicand; falls through to ExitBB.
//   ExitBB:     receives MULPSEUDO's original successors; the result is the
//               PHI of TestExitBB's and AddExitBB's, copied into the pseudo's
//               destination.
//
// Only JUMPs sit between the SHIFT and the flags it set being read, and a
// spill or reload the register allocator puts there is a LOAD, STORE or
// MOVE, none of which touch F.
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
  MachineBasicBlock *TestAddBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TestExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *SkipBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *AddLoopBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *AddExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ExitBB = MF->CreateMachineBasicBlock(LLVM_BB);
  for (MachineBasicBlock *New : {LoopBB, TestAddBB, TestExitBB, SkipBB,
                                 AddLoopBB, AddExitBB, ExitBB})
    MF->insert(InsertPt, New);

  ExitBB->splice(ExitBB->begin(), BB,
                 std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitBB->transferSuccessorsAndUpdatePHIs(BB);

  BB->addSuccessor(LoopBB);
  LoopBB->addSuccessor(AddLoopBB);
  LoopBB->addSuccessor(TestAddBB);
  TestAddBB->addSuccessor(AddExitBB);
  TestAddBB->addSuccessor(TestExitBB);
  TestExitBB->addSuccessor(ExitBB);
  TestExitBB->addSuccessor(SkipBB);
  SkipBB->addSuccessor(LoopBB);
  AddLoopBB->addSuccessor(LoopBB);
  AddExitBB->addSuccessor(ExitBB);

  Register A0 = MRI.createVirtualRegister(RC);
  Register B0 = MRI.createVirtualRegister(RC);
  Register R0 = MRI.createVirtualRegister(RC);
  Register APhi = MRI.createVirtualRegister(RC);
  Register BPhi = MRI.createVirtualRegister(RC);
  Register RPhi = MRI.createVirtualRegister(RC);
  Register BNext = MRI.createVirtualRegister(RC);
  Register ASkip = MRI.createVirtualRegister(RC);
  Register RAddLoop = MRI.createVirtualRegister(RC);
  Register AAddLoop = MRI.createVirtualRegister(RC);
  Register RAddExit = MRI.createVirtualRegister(RC);
  Register RExit = MRI.createVirtualRegister(RC);

  // BB
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), A0).addReg(AReg);
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), B0).addReg(BReg);
  BuildMI(*BB, MI, DL, TII->get(Moe::XOR), R0).addReg(A0).addReg(A0);

  // LoopBB
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), APhi)
      .addReg(A0).addMBB(BB)
      .addReg(ASkip).addMBB(SkipBB)
      .addReg(AAddLoop).addMBB(AddLoopBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), BPhi)
      .addReg(B0).addMBB(BB)
      .addReg(BNext).addMBB(SkipBB)
      .addReg(BNext).addMBB(AddLoopBB);
  BuildMI(LoopBB, DL, TII->get(TargetOpcode::PHI), RPhi)
      .addReg(R0).addMBB(BB)
      .addReg(RPhi).addMBB(SkipBB)
      .addReg(RAddLoop).addMBB(AddLoopBB);
  BuildMI(LoopBB, DL, TII->get(Moe::SRL), BNext).addReg(BPhi);
  BuildMI(LoopBB, DL, TII->get(Moe::JCC))
      .addMBB(AddLoopBB)
      .addImm(MoeCC::COND_HI);

  // TestAddBB, TestExitBB: still the flags LoopBB's SHIFT set.
  BuildMI(TestAddBB, DL, TII->get(Moe::JCC))
      .addMBB(AddExitBB)
      .addImm(MoeCC::COND_CS);
  BuildMI(TestExitBB, DL, TII->get(Moe::JCC))
      .addMBB(ExitBB)
      .addImm(MoeCC::COND_EQ);

  // SkipBB
  BuildMI(SkipBB, DL, TII->get(Moe::SHL), ASkip).addReg(APhi);
  BuildMI(SkipBB, DL, TII->get(Moe::JMP)).addMBB(LoopBB);

  // AddLoopBB
  BuildMI(AddLoopBB, DL, TII->get(Moe::ADD), RAddLoop)
      .addReg(RPhi)
      .addReg(APhi);
  BuildMI(AddLoopBB, DL, TII->get(Moe::SHL), AAddLoop).addReg(APhi);
  BuildMI(AddLoopBB, DL, TII->get(Moe::JMP)).addMBB(LoopBB);

  // AddExitBB
  BuildMI(AddExitBB, DL, TII->get(Moe::ADD), RAddExit)
      .addReg(RPhi)
      .addReg(APhi);
  BuildMI(AddExitBB, DL, TII->get(Moe::JMP)).addMBB(ExitBB);

  // ExitBB
  MachineBasicBlock::iterator ExitBegin = ExitBB->begin();
  BuildMI(*ExitBB, ExitBegin, DL, TII->get(TargetOpcode::PHI), RExit)
      .addReg(RPhi).addMBB(TestExitBB)
      .addReg(RAddExit).addMBB(AddExitBB);
  BuildMI(*ExitBB, ExitBegin, DL, TII->get(TargetOpcode::COPY), DstReg)
      .addReg(RExit);

  MI.eraseFromParent();
  return ExitBB;
}

//===----------------------------------------------------------------------===//
// Software variable-amount shift. SHIFT moves one bit per instruction, so a
// shift by a runtime amount N has to execute N of them - but it does not have
// to count to N. The amount's five low bits are shifted out one at a time,
// each into C, and bit k set means "do 2^k single shifts now":
//
//   amount >> 1, C=bit0 ? shift x 1 time
//   amount >> 1, C=bit1 ? shift x 2 times
//   ...
//   amount >> 1, C=bit4 ? shift x 16 times
//
// That is ten instructions of tests plus exactly N shifts, against five or so
// instructions per shift for a counting loop, and it examines only the bits
// that exist: an amount outside 0-31 is poison in IR, and this behaves as if
// the amount were masked to five bits rather than spinning. An earlier
// counting loop compared against the raw amount and, handed -1, ran about
// four billion times - clang turns `c ? x >> (s - 12) : x << (12 - s)` into a
// select over both shifts, and Linux's alloc_large_system_hash is exactly
// that.
//
// Blocks (BB is VARSHIFTPSEUDO's parent block, split at the pseudo), for each
// bit k from 0 to 4:
//   TestBB_k:  amount >>= 1; C clear jumps to MergeBB_k (TestBB_0 is BB).
//   ShiftBB_k: 2^k single-bit shifts of the value; falls through.
//   MergeBB_k: PHI of the value with and without those shifts; it is also
//              TestBB_(k+1), and MergeBB_4 receives the pseudo's original
//              successors and copies the value into its destination.
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

  MachineBasicBlock *ShiftBBs[5], *MergeBBs[5];
  for (unsigned K = 0; K < 5; ++K) {
    ShiftBBs[K] = MF->CreateMachineBasicBlock(LLVM_BB);
    MergeBBs[K] = MF->CreateMachineBasicBlock(LLVM_BB);
    MF->insert(InsertPt, ShiftBBs[K]);
    MF->insert(InsertPt, MergeBBs[K]);
  }
  MachineBasicBlock *ExitBB = MergeBBs[4];

  ExitBB->splice(ExitBB->begin(), BB,
                 std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitBB->transferSuccessorsAndUpdatePHIs(BB);

  Register Value = MRI.createVirtualRegister(RC);
  Register Amount = MRI.createVirtualRegister(RC);
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Value).addReg(ValReg);
  BuildMI(*BB, MI, DL, TII->get(TargetOpcode::COPY), Amount).addReg(AmtReg);

  // Instructions for TestBB_0 go before the pseudo in BB; later tests go at
  // the end of the previous merge block, after its PHI.
  MachineBasicBlock *TestBB = BB;
  for (unsigned K = 0; K < 5; ++K) {
    MachineBasicBlock *ShiftBB = ShiftBBs[K], *MergeBB = MergeBBs[K];
    Register AmountNext = MRI.createVirtualRegister(RC);

    auto Emit = [&](unsigned Opc, Register Dst) {
      if (TestBB == BB)
        return BuildMI(*BB, MI, DL, TII->get(Opc), Dst);
      return BuildMI(TestBB, DL, TII->get(Opc), Dst);
    };
    Emit(Moe::SRL, AmountNext).addReg(Amount);
    if (TestBB == BB)
      BuildMI(*BB, MI, DL, TII->get(Moe::JCC))
          .addMBB(MergeBB)
          .addImm(MoeCC::COND_CC);
    else
      BuildMI(TestBB, DL, TII->get(Moe::JCC))
          .addMBB(MergeBB)
          .addImm(MoeCC::COND_CC);

    TestBB->addSuccessor(MergeBB);
    TestBB->addSuccessor(ShiftBB);
    ShiftBB->addSuccessor(MergeBB);

    Register Shifted = Value;
    for (unsigned I = 0; I < (1u << K); ++I) {
      Register Next = MRI.createVirtualRegister(RC);
      BuildMI(ShiftBB, DL, TII->get(ShiftOpc), Next).addReg(Shifted);
      Shifted = Next;
    }

    Register Merged = MRI.createVirtualRegister(RC);
    BuildMI(*MergeBB, MergeBB->begin(), DL, TII->get(TargetOpcode::PHI), Merged)
        .addReg(Value).addMBB(TestBB)
        .addReg(Shifted).addMBB(ShiftBB);

    Value = Merged;
    Amount = AmountNext;
    TestBB = MergeBB;
  }

  // The PHI is ExitBB's first instruction; the copy goes right after it.
  BuildMI(*ExitBB, std::next(ExitBB->begin()), DL,
          TII->get(TargetOpcode::COPY), DstReg)
      .addReg(Value);

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
