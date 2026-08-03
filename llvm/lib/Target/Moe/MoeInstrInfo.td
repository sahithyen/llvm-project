//===-- MoeInstrInfo.td - Moe Instruction defs -------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "MoeInstrFormats.td"

//===----------------------------------------------------------------------===//
// Moe-specific SDNode definitions.
//===----------------------------------------------------------------------===//

// Moe has no hardware call/link instruction (JUMP never saves a return
// address) and no CMP instruction (comparisons are a SUB whose result is
// discarded, keeping only the flags) - see the Milestone 1 plan's ABI
// section. CALL/RET are lowered as Pseudo instructions expanded by a custom
// inserter into the real PUSH/JUMP/POP/MOVE sequences (see MoeISelLowering).
def SDT_MoeCall   : SDTypeProfile<0, -1, [SDTCisVT<0, iPTR>]>;
def SDT_MoeCallSeqStart : SDCallSeqStart<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;
def SDT_MoeCallSeqEnd   : SDCallSeqEnd<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;
def SDT_MoeCmp    : SDTypeProfile<0, 2, [SDTCisSameAs<0, 1>]>;
def SDT_MoeBrCC   : SDTypeProfile<0, 2, [SDTCisVT<0, OtherVT>, SDTCisVT<1, i32>]>;

def Moecall : SDNode<"MoeISD::CALL", SDT_MoeCall,
                      [SDNPHasChain, SDNPOutGlue, SDNPOptInGlue, SDNPVariadic]>;
def Moecallseq_start : SDNode<"ISD::CALLSEQ_START", SDT_MoeCallSeqStart,
                               [SDNPHasChain, SDNPOutGlue]>;
def Moecallseq_end   : SDNode<"ISD::CALLSEQ_END", SDT_MoeCallSeqEnd,
                               [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;
def Moeret    : SDNode<"MoeISD::RET", SDTNone,
                        [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;
def Moecmp    : SDNode<"MoeISD::CMP", SDT_MoeCmp, [SDNPOutGlue]>;
def Moebrcc   : SDNode<"MoeISD::BR_CC", SDT_MoeBrCC, [SDNPHasChain, SDNPInGlue]>;

// Wraps a constant-pool-index/global-address/external-symbol/block-address
// leaf so it can appear as the address operand of LOADabs/STOREabs - see
// MoeISelLowering::LowerConstantPool/LowerGlobalAddress.
def SDT_MoeWrapper : SDTypeProfile<1, 1, [SDTCisSameAs<0, 1>, SDTCisPtrTy<0>]>;
def Moewrapper : SDNode<"MoeISD::Wrapper", SDT_MoeWrapper>;

// Single-bit shifts (carry_in=0), matching SHL/SRL/SRA exactly - distinct
// target-specific opcodes (never marked Custom) so MoeISelLowering::
// LowerShifts can chain N of them for a shift by constant N without the
// legalizer re-visiting and re-lowering each intermediate step - see the
// Milestone 1 plan's SHIFT notes.
def Moeshl1 : SDNode<"MoeISD::SHL1", SDTIntUnaryOp>;
def Moesrl1 : SDNode<"MoeISD::SRL1", SDTIntUnaryOp>;
def Moesra1 : SDNode<"MoeISD::SRA1", SDTIntUnaryOp>;

//===----------------------------------------------------------------------===//
// Operands.
//===----------------------------------------------------------------------===//

// Operand for printing out a condition code - see 'Condition selection' in
// the Encoding chapter.
def cc : Operand<i32> {
  let PrintMethod = "printCCOperand";
}

// A bare jump target for conditional/unconditional branches within a
// function (basic-block-relative for asm printing purposes; Milestone 1
// only emits this as a symbolic label operand, deferring exact trailing-
// operand-word encoding to a later milestone - see 'Addressing mode' in the
// Encoding chapter and the Milestone 1 plan's MC-layer notes).
def jmptarget : Operand<OtherVT>;

// A direct call target (global function symbol).
def calltarget : Operand<iPTR>;

// LOADabs/STOREabs's trailing-operand address (Addressing mode = Absolute,
// see 'Addressing mode' in the Encoding chapter): a constant-pool index
// (Stage 3's literal-pool integer/global constants) or, for the call
// sequence's return-address materialization, a MachineBasicBlock reference
// (the continuation block right after the call, whose label MCInstLower
// already lowers generically the same way branch targets are) - built
// directly via BuildMI in MoeISelLowering's custom inserter, not matched by
// a SelectionDAG Pat, so no MIOperandInfo restriction is needed here.
def moeaddr : Operand<i32>;

//===----------------------------------------------------------------------===//
// Pseudo-instructions.
//===----------------------------------------------------------------------===//

// ADJCALLSTACKDOWN/UP bracket a call and implicitly use/def SP, so
// prolog-epilog insertion knows a call site may touch it even before it's
// expanded into real PUSH/JUMP instructions.
let Defs = [SP], Uses = [SP] in {
def ADJCALLSTACKDOWN : MoePseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                                  "#ADJCALLSTACKDOWN $amt1 $amt2",
                                  [(Moecallseq_start timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP   : MoePseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                                  "#ADJCALLSTACKUP $amt1 $amt2",
                                  [(Moecallseq_end timm:$amt1, timm:$amt2)]>;
}

// Expanded by MoeTargetLowering::EmitInstrWithCustomInserter into:
//   LOAD.W  [<fresh return-address label>] -> GP4
//   PUSH.W  GP4
//   JUMP.AL [$callee]
//   <fresh return-address label>:
// GP4/GP5 are caller-saved scratch (see the ABI), so no explicit Defs list
// is needed for them; the argument/return registers are marked used/defined
// by MoeISelLowering::LowerCall via implicit operands on this pseudo.
let isCall = 1, usesCustomInserter = 1, Uses = [SP], Defs = [SP] in
def CALL : MoePseudo<(outs), (ins calltarget:$callee), "#CALL $callee",
                      [(Moecall tglobaladdr:$callee)]>;

// Expanded by MoeInstrInfo::expandPostRAPseudo (NOT a custom inserter -
// this pseudo must still look like a real `isReturn` terminator when
// PrologEpilogInserter runs, so it knows this block needs an epilogue;
// expanding any earlier, during ISel, would erase that information before
// PEI ever sees it) into:
//   POP.W    GP4
//   MOVE.1111 GP4 -> IA
let isReturn = 1, isTerminator = 1, isBarrier = 1, Uses = [SP], Defs = [SP] in
def RET : MoePseudo<(outs), (ins), "#RET", [(Moeret)]>;

//===----------------------------------------------------------------------===//
// MOVE (0b0100) - the only register-to-register copy; also how a computed
// value redirects control flow (MOVE ... -> IA), used by RET's expansion.
//===----------------------------------------------------------------------===//

// No Pat here deliberately: a plain register-to-register copy never needs
// SelectionDAG pattern matching (it's synthesized directly by
// MoeInstrInfo::copyPhysReg and register-allocator coalescing) - and a
// pattern shaped like `(set GPR:$to, GPR:$from)` has no discriminating
// opcode, so it would wrongly match ANY i32-producing node (e.g. `add`),
// not just an actual copy.
let mask = 0b1111 in
def MOVE : MoveForm<(outs AllReg:$to), (ins AllReg:$from), "move $from -> $to", []>;

//===----------------------------------------------------------------------===//
// ADD (0b1001) / SUB (0b1010) - word size, no carry-in (i32 fits in one
// register; multi-word carry chains aren't needed by Milestone 1).
//===----------------------------------------------------------------------===//

def ADD : AddSubForm<0b1001, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                      "add $a, $b -> $o",
                      [(set GPR:$o, (add GPR:$a, GPR:$b))]>;

def SUB : AddSubForm<0b1010, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                      "sub $a, $b -> $o",
                      [(set GPR:$o, (sub GPR:$a, GPR:$b))]>;

// Comparison-only SUB (Test-mode-equivalent): Moe has no CMP opcode, so a
// compare is a real SUB whose result goes to a dead scratch register and
// only the flags are consumed - see the Milestone 1 plan's ABI/instruction
// selection notes.
// Real hardware SUB always writes its 3-bit `o` field to some GP register
// (unlike AND/OR/XOR/SHIFT, SUB has no Test-mode result-discard bit) - since
// this pseudo's result is never used, `o` is just hardcoded to GP0 (0),
// mirroring Test mode's own "O must be encoded as 0" convention for a
// discarded result.
let isCompare = 1, o = 0 in
def SUBcmp : AddSubForm<0b1010, (outs), (ins GPR:$a, GPR:$b),
                         "sub $a, $b -> (dead)", [(Moecmp GPR:$a, GPR:$b)]>;

//===----------------------------------------------------------------------===//
// AND (0b1100) / OR (0b1101) / XOR (0b1110)
//===----------------------------------------------------------------------===//

def AND : AluTestForm<0b1100, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                       "and $a, $b -> $o",
                       [(set GPR:$o, (and GPR:$a, GPR:$b))]>;

def OR : AluTestForm<0b1101, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                      "or $a, $b -> $o",
                      [(set GPR:$o, (or GPR:$a, GPR:$b))]>;

def XOR : AluTestForm<0b1110, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                       "xor $a, $b -> $o",
                       [(set GPR:$o, (xor GPR:$a, GPR:$b))]>;

//===----------------------------------------------------------------------===//
// SHIFT (0b1111) - single bit only per instruction; ISel expands shl/lshr/
// ashr by a constant amount N into N chained SHIFTs (carryin=1 to feed the
// vacated bit from the previous shift's carry-out) - see
// MoeISelLowering::LowerShifts. Variable-amount shifts are deferred past
// Milestone 1 (its test case only needs small constant shift amounts).
//===----------------------------------------------------------------------===//

let right = 0 in
def SHL : ShiftForm<(outs GPR:$o), (ins GPR:$a), "shift.left $a -> $o",
                     [(set GPR:$o, (Moeshl1 GPR:$a))]>;

let right = 1, arithmetic = 0 in
def SRL : ShiftForm<(outs GPR:$o), (ins GPR:$a), "shift.right $a -> $o",
                     [(set GPR:$o, (Moesrl1 GPR:$a))]>;

let right = 1, arithmetic = 1 in
def SRA : ShiftForm<(outs GPR:$o), (ins GPR:$a), "shift.right.arith $a -> $o",
                     [(set GPR:$o, (Moesra1 GPR:$a))]>;

let right = 0, carryin = 1 in
def SHLc : ShiftForm<(outs GPR:$o), (ins GPR:$a), "shift.left.c $a -> $o", []>;

let right = 1, arithmetic = 0, carryin = 1 in
def SRLc : ShiftForm<(outs GPR:$o), (ins GPR:$a), "shift.right.c $a -> $o", []>;

//===----------------------------------------------------------------------===//
// INCREMENT (0b1011) - o can be any of the 16 registers, but Milestone 1
// only ever targets it at GPR (stack-pointer adjustment uses SUB/ADD with a
// literal-pool constant instead, per the Milestone 1 plan's FrameLowering
// notes, since frame sizes can exceed INCREMENT's 6-bit/0-63 range).
//===----------------------------------------------------------------------===//

def INCREMENT : IncrementForm<(outs GPR:$o), (ins GPR:$oin, i32imm:$value),
                               "increment $o += $value", []> {
  let Constraints = "$o = $oin";
}

//===----------------------------------------------------------------------===//
// JUMP (0b0000) - conditional/unconditional branch. Condition AL (0) is
// unconditional, used both for `br` and the CALL pseudo's expansion.
//===----------------------------------------------------------------------===//

let isBranch = 1, isTerminator = 1 in {
  let isBarrier = 1 in
  def JMP : JumpForm<0b00, (outs), (ins jmptarget:$dst), "jump.al $dst",
                      [(br bb:$dst)]> {
    let cond = 0; // AL
  }

  def JCC : JumpForm<0b00, (outs), (ins jmptarget:$dst, cc:$cond),
                      "jump.$cond $dst",
                      [(Moebrcc bb:$dst, timm:$cond)]>;
} // isBranch, isTerminator

// Same real encoding as JMP (opcode 0b0000, condition AL), but with a
// calltarget (global-address) operand instead of jmptarget (MBB). Used only
// by the CALL pseudo's custom-inserter expansion, to jump to a *different*
// function - never marked isBranch/isTerminator, since generic MI
// CFG/liveness analysis (analyzeBranch, LiveIntervals, ...) assumes any
// isBranch instruction's target operand is one of this function's own
// MachineBasicBlocks, which would misinterpret a global-address operand.
// Not marked isBarrier either: architecturally the callee's own return
// sequence jumps back to right after this instruction, so - like an
// ordinary hardware CALL on any other target - execution is modeled as
// falling through to the continuation block, not diverging.
def JMPabs : JumpForm<0b00, (outs), (ins calltarget:$dst), "jump.al $dst", []> {
  let cond = 0; // AL
}

//===----------------------------------------------------------------------===//
// PUSH (0b0111) / POP (0b1000) - full-descending stack. Used directly (not
// through generic patterns) by the call/return sequence and by
// MoeFrameLowering's callee-saved register save/restore - see the
// Milestone 1 plan's "Division of labor" note.
//===----------------------------------------------------------------------===//

def PUSH : PushPopForm<0b0111, (outs), (ins GPR:$reg), "push $reg", []>;
def POP  : PushPopForm<0b1000, (outs GPR:$reg), (ins), "pop $reg", []>;

//===----------------------------------------------------------------------===//
// LOAD (0b0101) / STORE (0b0110), Absolute addressing mode only (mode 0) -
// the only way to materialize a constant/global address/return-address
// label, since there is no load-immediate instruction - see the Milestone 1
// plan's "Constant materialization" section. Register-indirect (mode 2)
// LOAD/STORE, used for frame-index/spill access, is added once
// eliminateFrameIndex is implemented - see the Milestone 1 plan's
// "Frame-index elimination and spills" section.
//===----------------------------------------------------------------------===//

def LOADabs : LoadStoreForm<0b0101, 0b00, (outs GPR:$reg), (ins moeaddr:$addr),
                             "load.w $addr -> $reg",
                             [(set GPR:$reg, (load (Moewrapper tconstpool:$addr)))]>;

def STOREabs : LoadStoreForm<0b0110, 0b00, (outs), (ins GPR:$reg, moeaddr:$addr),
                              "store.w $reg -> $addr", []>;

//===----------------------------------------------------------------------===//
// LOAD/STORE, Register-indirect addressing mode (mode 2) - used for
// frame-index/spill access (via eliminateFrameIndex/storeRegToStackSlot/
// loadRegFromStackSlot) and general pointer dereference (e.g. loading
// through a global's address once it's been materialized into a register
// by LOADabs) - see the Milestone 1 plan's "Frame-index elimination and
// spills" section. The trailing operand's base-register-select/28-bit-
// offset packing (bits 3-0 / bits 31-4) is an MC-encoding detail deferred
// past Milestone 1 (asm-text output only - see the plan's MC-layer notes);
// moemem's base/offset are carried as ordinary MachineOperands for now.
//===----------------------------------------------------------------------===//

def moemem : Operand<i32> {
  let PrintMethod = "printMemOperand";
  let MIOperandInfo = (ops GPR, i32imm);
}

def moeaddr_ri : ComplexPattern<i32, 2, "SelectAddr", [], []>;

def LOADrr : LoadStoreForm<0b0101, 0b10, (outs GPR:$reg), (ins moemem:$addr),
                            "load.w $addr -> $reg",
                            [(set GPR:$reg, (load moeaddr_ri:$addr))]>;

def STORErr : LoadStoreForm<0b0110, 0b10, (outs), (ins GPR:$reg, moemem:$addr),
                             "store.w $reg -> $addr",
                             [(store GPR:$reg, moeaddr_ri:$addr)]>;
