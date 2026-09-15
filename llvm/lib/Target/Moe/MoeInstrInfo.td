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

// A libcall (e.g. the soft-float runtime's __addsf3, emitted automatically
// by the generic float-softening legalizer - see the Milestone 8 plan) is
// referenced by an external-symbol name rather than a GlobalValue*, so its
// callee is a TargetExternalSymbol node instead of TargetGlobalAddress -
// same CALL pseudo, alternate callee-matching pattern.
def : Pat<(Moecall texternalsym:$callee), (CALL texternalsym:$callee)>;

// Expanded by MoeInstrInfo::expandPostRAPseudo (NOT a custom inserter -
// this pseudo must still look like a real `isReturn` terminator when
// PrologEpilogInserter runs, so it knows this block needs an epilogue;
// expanding any earlier, during ISel, would erase that information before
// PEI ever sees it) into:
//   POP.W    GP4
//   MOVE.1111 GP4 -> IA
let isReturn = 1, isTerminator = 1, isBarrier = 1, Uses = [SP], Defs = [SP] in
def RET : MoePseudo<(outs), (ins), "#RET", [(Moeret)]>;

// Software multiply (see the Milestone 3 plan's "Software multiply" section):
// SHIFT moves exactly one bit per instruction and its carry-out is the bit
// shifted out, so a shift-and-add loop can test each multiplier bit directly
// off SHIFT's C flag, with no AND-with-1 masking step (AND is register-
// register only anyway, so there's no immediate-mask instruction to use).
// Expanded by MoeTargetLowering::emitMul into a real runtime loop of SHIFT/
// ADD/INCREMENT/JCC - constructed directly as a machine node in LowerMUL
// (not matched via a SelectionDAG Pat), so no pattern is needed here.
let usesCustomInserter = 1 in
def MULPSEUDO : MoePseudo<(outs GPR:$dst), (ins GPR:$a, GPR:$b),
                           "#MUL $dst, $a, $b", []>;

// Software unsigned divide/remainder (see the Milestone 3 plan's "Software
// divide/remainder" section): a restoring binary long-division loop,
// producing quotient and remainder together so UDIV/UREM of the same
// operands share one loop. Each iteration shifts the dividend and remainder
// together as one wide value via SHIFT's Carry-in chaining (SHLc, already
// defined below - the same mechanic 'SHIFT' in the Instruction set chapter
// describes for multi-word shifts), then compares the shifted remainder
// against the divisor to decide the next quotient bit. Constructed directly
// as a machine node in LowerUDIV/LowerUREM (not matched via a SelectionDAG
// Pat), so no pattern is needed here.
let usesCustomInserter = 1 in
def DIVMODPSEUDO : MoePseudo<(outs GPR:$q, GPR:$r), (ins GPR:$a, GPR:$b),
                              "#DIVMOD $q, $r, $a, $b", []>;

// Software signed divide/remainder: wraps DIVMODPSEUDO's unsigned core with
// sign correction (see the Milestone 3 plan's "Software divide/remainder"
// section) - absolute-value the operands, run the same unsigned loop, then
// negate the quotient if the operands' signs differed and the remainder if
// the dividend was negative (C-style truncating-toward-zero division).
let usesCustomInserter = 1 in
def SDIVMODPSEUDO : MoePseudo<(outs GPR:$q, GPR:$r), (ins GPR:$a, GPR:$b),
                               "#SDIVMOD $q, $r, $a, $b", []>;

// Value-producing compare (Milestone 11): Moe has no CMP/SELECT/SETCC
// instruction, only a flags-setting SUBcmp consumed by a conditional branch
// (see BR_CC above) - so materializing a comparison's 0/1 result as a real
// value (needed by the generic i64-from-i32-halves legalizer's carry
// detection, and by ordinary C like `int f(int a,int b){return a<b;}`) needs
// a real branchy sequence, not a single instruction. Constructed directly as
// a machine node in LowerSETCC (not matched via a SelectionDAG Pat, same as
// MULPSEUDO above), and expanded by MoeTargetLowering::emitSetCC into
// SUBcmp+JCC+two 0/1-producing blocks merged by a PHI - the same shape
// emitCondNegate already uses for "produce a value from a branch condition",
// just testing $a against $b (with $cond) instead of a fixed sign test.
let usesCustomInserter = 1 in
def SETCCPSEUDO : MoePseudo<(outs GPR:$dst), (ins GPR:$a, GPR:$b, i32imm:$cond),
                             "#SETCC $dst, $a, $b, $cond", []>;

// Value-producing select-on-compare (Milestone 12): needed by the generic
// i64-from-i32-halves legalizer's relational-compare expansion (compare
// high words; if equal, compare low words; SELECT_CC combines the two
// results) - see LowerSELECT_CC/emitSelectCC. Same shape as SETCCPSEUDO
// (SUBcmp+JCC+merge-by-PHI), generalized to merge two arbitrary operands
// ($t/$f) instead of hardcoded 0/1.
let usesCustomInserter = 1 in
def SELECTCCPSEUDO : MoePseudo<(outs GPR:$dst),
                                (ins GPR:$a, GPR:$b, GPR:$t, GPR:$f, i32imm:$cond),
                                "#SELECTCC $dst, $a, $b, $t, $f, $cond", []>;

// Software variable-amount shift (see the report_fatal_error this replaces in
// MoeISelLowering::LowerShifts): SHIFT moves exactly one bit per instruction
// with no shift-count field, so a shift by a runtime-computed amount needs a
// runtime loop - the same shape as MULPSEUDO's loop but simpler (no
// conditional add step, just one unconditional single-bit shift per
// iteration). One pseudo shared by SHL/SRL/SRA, discriminated by an
// immediate opcode-selector operand (which real single-bit-shift opcode the
// loop body should use), since the block-wiring is otherwise identical
// across all three - mirrors DIVMODPSEUDO being shared by UDIV and UREM.
// Constructed directly as a machine node in LowerShifts's non-constant-amount
// branch (not matched via a SelectionDAG Pat), so no pattern is needed here.
let usesCustomInserter = 1 in
def VARSHIFTPSEUDO : MoePseudo<(outs GPR:$dst),
                                (ins GPR:$val, GPR:$amt, i32imm:$opc),
                                "#VARSHIFT $dst, $val, $amt, $opc", []>;

// FIADDR (a bare stack-object address used as an ordinary i32 value) is
// defined further below, after moemem, which its (ins) list depends on - see
// its definition next to LOADrr/STORErr for the full explanation.

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

// Defs = [F] (here and on every other ALU-family instruction below - AND/OR/
// XOR, INCREMENT, the SHIFT family, SUBcmp): real hardware sets the ALU
// flags on every one of these, not just SUBcmp/SHIFT. F (the real, already-
// defined architectural Flags register - see registers.md - already
// permanently Reserved, so this never affects allocation) gives that a real,
// modeled dependency instead of the bare hasSideEffects=1 barrier Milestone
// 10 first used: hasSideEffects alone stops SUBcmp/SHIFT themselves from
// being deleted or reordered, but does NOT stop an unrelated instruction
// (confirmed: XOR, computing an always-taken branch's 0/1 result in
// MoeISelLowering::emitSetCC) from being sunk into the gap between a SUBcmp
// and the JCC reading its flags - Machine Sinking's interference analysis
// only checks registers it knows are live, and flags weren't modeled as one.
// Declaring Defs=[F] here (and Uses=[F] on JCC, below) closes that gap for
// every machine pass generally, not just the one instance caught -
// confirmed via a real, reproducible wrong-answer bug: llc -O1 sinking
// emitSetCC's zero-result XOR between SUBcmp and JCC, so JCC tested XOR's
// (always-zero) flags instead of the actual comparison's.
let Defs = [F] in {
def ADD : AddSubForm<0b1001, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                      "add $a, $b -> $o",
                      [(set GPR:$o, (add GPR:$a, GPR:$b))]>;

def SUB : AddSubForm<0b1010, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                      "sub $a, $b -> $o",
                      [(set GPR:$o, (sub GPR:$a, GPR:$b))]>;
}

// Comparison-only SUB (Test-mode-equivalent): Moe has no CMP opcode, so a
// compare is a real SUB whose result goes to a dead scratch register and
// only the flags are consumed - see the Milestone 1 plan's ABI/instruction
// selection notes.
// Real hardware SUB always writes its 3-bit `o` field to some GP register
// (unlike AND/OR/XOR/SHIFT, SUB has no Test-mode result-discard bit) - since
// this pseudo's result is never used, `o` is just hardcoded to GP0 (0),
// mirroring Test mode's own "O must be encoded as 0" convention for a
// discarded result. Real hardware still physically writes that dead result
// into GP0 (register encoding 0) every time - Defs = [GP0] tells the
// register allocator this instruction clobbers GP0 even though this
// pseudo's own (outs) list is empty, so nothing else is kept live there
// across a SUBcmp (see the Milestone 3 plan: this surfaced as a real bug -
// the software-multiply loop's counter got silently allocated to GP0 and
// destroyed by its own loop-condition SUBcmp before this fix).
// hasSideEffects = 1: SUBcmp's real effect - the ALU flags it sets for the
// conditional branch that (by construction) always immediately follows it -
// is expressed only as SelectionDAG glue (Moecmp's SDNPOutGlue/Moebrcc's
// SDNPInGlue), a pre-ISel-only mechanism. Once lowered to a real MachineInstr,
// the only thing this pseudo declares is Defs=[GP0], which is dead by
// design (the compare result itself is always discarded) - so with
// hasSideEffects left at its default 0, DeadMachineInstructionElim (part of
// the -O1+ pipeline, not -O0's) correctly-by-its-own-model but wrongly-in-
// reality deletes the whole instruction as unused, silently breaking every
// loop whose exit condition depends on it (the flags it should have set
// never get set, so the following conditional branch reads stale/wrong
// flags). Confirmed via a real, reproducible infinite loop (not a crash) in
// mul_test.ll/muldiv_test.ll/sdiv_test.ll/cross_call_test.ll at -O1 - traced
// with moe-emu --trace, showing the SUBcmp instruction simply missing from
// the loop body between INCREMENT and the conditional JUMP that depends on
// it. hasSideEffects=1 is the standard LLVM idiom for exactly this situation
// (an instruction whose real effect isn't captured by its declared Defs/Uses)
// - it blocks deletion and discourages reordering across it, for every
// machine pass, not just this one that was caught empirically.
let isCompare = 1, o = 0, Defs = [GP0, F], hasSideEffects = 1 in
def SUBcmp : AddSubForm<0b1010, (outs), (ins GPR:$a, GPR:$b),
                         "sub $a, $b -> (dead)", [(Moecmp GPR:$a, GPR:$b)]>;

//===----------------------------------------------------------------------===//
// AND (0b1100) / OR (0b1101) / XOR (0b1110)
//===----------------------------------------------------------------------===//

let Defs = [F] in {
def AND : AluTestForm<0b1100, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                       "and $a, $b -> $o",
                       [(set GPR:$o, (and GPR:$a, GPR:$b))]>;

def OR : AluTestForm<0b1101, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                      "or $a, $b -> $o",
                      [(set GPR:$o, (or GPR:$a, GPR:$b))]>;

def XOR : AluTestForm<0b1110, (outs GPR:$o), (ins GPR:$a, GPR:$b),
                       "xor $a, $b -> $o",
                       [(set GPR:$o, (xor GPR:$a, GPR:$b))]>;
}

//===----------------------------------------------------------------------===//
// SHIFT (0b1111) - single bit only per instruction; ISel expands shl/lshr/
// ashr by a constant amount N into N chained SHIFTs (carryin=1 to feed the
// vacated bit from the previous shift's carry-out) - see
// MoeISelLowering::LowerShifts. Variable-amount shifts are deferred past
// Milestone 1 (its test case only needs small constant shift amounts).
//===----------------------------------------------------------------------===//

// hasSideEffects = 1 on the whole family: like SUBcmp (see its own comment),
// every SHIFT sets the real ALU carry flag as a side effect that's never
// declared as a Def/Use anywhere - normally invisible, but emitMul/
// emitDivRem/emitSDivRem/emitVarShift's hand-built loops rely on that carry
// flowing undisturbed from one specific SHIFT into the very next
// instruction (a JCC testing it directly, for SHL/SRL/SRA; another SHLc/SRLc
// consuming it as carry-in, for the div/mod loops' remainder-shift chaining
// - see LowerUDIV's comment on SHLc). Confirmed via a real, reproducible
// wrong-answer bug at -O1 (not a crash): llc's own -O1 assembly for
// mul_test.ll shows LoopBB's SRL sunk down past the JCC that's supposed to
// test its carry output, into the block on the other side of that branch -
// a machine-sinking-style pass moved it because nothing declared the
// dependency. Same fix as SUBcmp and for the same reason: this is the
// standard LLVM idiom for "this instruction's real effect isn't captured by
// its declared Defs/Uses," blocking exactly this kind of reordering for
// every machine pass, not just the one caught empirically.
let hasSideEffects = 1, Defs = [F] in {
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
}

//===----------------------------------------------------------------------===//
// INCREMENT (0b1011) - o can be any of the 16 registers, but Milestone 1
// only ever targets it at GPR (stack-pointer adjustment uses SUB/ADD with a
// literal-pool constant instead, per the Milestone 1 plan's FrameLowering
// notes, since frame sizes can exceed INCREMENT's 6-bit/0-63 range).
//===----------------------------------------------------------------------===//

let Defs = [F] in
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

  let Uses = [F] in
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
// SYSCALL (0b0001) / IRET (0b0010) / INVALIDATE (0b0011) - the three
// operand-less control-transfer instructions, using MoeInstrFormats.td's
// NoOperandForm. None of them is ever produced by instruction selection:
// there is no C-level construct that lowers to a trap entry, a trap return
// or a translation-cache flush, so none has a Pattern and none appears in
// MoeISelLowering. They exist purely so hand-written supervisor assembly
// (the kernel's head.S/entry.S, and llvm-tests' supervisor/paging tests)
// can be assembled at all - before this, `syscall`/`iret`/`invalidate` were
// not defined anywhere in the backend and every one of them is needed by
// the first few lines of any trap vector.
//
// Flags are set honestly rather than defensively, since machine passes act
// on them:
//  - All three have side effects nothing in their Defs/Uses can express
//    (SYSCALL diverts control to the trap vector; INVALIDATE drops the
//    translation cache, which is invisible to every memory-dependence
//    analysis LLVM has), so hasSideEffects blocks deletion and reordering
//    across them - the same idiom SUBcmp and the SHIFT family already use
//    here for the same reason.
//  - IRET is a return, a terminator and a barrier: it restores IA from RA,
//    so nothing after it in its block is reachable.
//  - SYSCALL is deliberately NOT a barrier: architecturally the handler's
//    own IRET resumes at the instruction after it (see 'Traps'), so control
//    falls through, exactly as it does for an ordinary call.
//===----------------------------------------------------------------------===//

let hasSideEffects = 1 in {

// Trap entry writes RA/RF/IFS and updates F (see the 5-step sequence in
// 'Trap entry' in the Traps chapter); IA becomes the trap vector.
let Defs = [IA, F, RA, RF, IFS] in
def SYSCALL : NoOperandForm<0b0001, "syscall", []>;

let isReturn = 1, isTerminator = 1, isBarrier = 1, Uses = [RA, RF],
    Defs = [IA, F] in
def IRET : NoOperandForm<0b0010, "iret", []>;

def INVALIDATE : NoOperandForm<0b0011, "invalidate", []>;

} // hasSideEffects

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

// The base register is BaseReg, not GPR: 'Base register selection' in the
// Encoding chapter admits GP0-GP7, IA, SP, TT and F. SP matters most - every
// frame-relative access is `[sp+offset]` - and with GPR here the assembler
// rejected exactly the text llc itself emits.
def moemem : Operand<i32> {
  let PrintMethod = "printMemOperand";
  let MIOperandInfo = (ops BaseReg, i32imm);
}

def moeaddr_ri : ComplexPattern<i32, 2, "SelectAddr", [], []>;

def LOADrr : LoadStoreForm<0b0101, 0b10, (outs GPR:$reg), (ins moemem:$addr),
                            "load.w $addr -> $reg",
                            [(set GPR:$reg, (load moeaddr_ri:$addr))]>;

def STORErr : LoadStoreForm<0b0110, 0b10, (outs), (ins GPR:$reg, moemem:$addr),
                             "store.w $reg -> $addr",
                             [(store GPR:$reg, moeaddr_ri:$addr)]>;

// A bare stack-object address used as an ordinary i32 VALUE (as opposed to a
// LOAD/STORE address operand, which already works via moeaddr_ri/SelectAddr
// above - see the Milestone 6 plan's "FrameIndex-as-value" gap). Constructed
// directly as a machine node by MoeISelDAGToDAG::Select's ISD::FrameIndex
// case (mirroring how MULPSEUDO is built directly by LowerMUL, not
// Pat-matched), carrying the same (TargetFrameIndex, disp) operand pair
// SelectAddr already produces for the address-operand case, via moemem - so
// eliminateFrameIndex's existing per-instruction offset computation applies
// uniformly to this and to LOADrr/STORErr alike. Deliberately has NO
// usesCustomInserter: unlike MULPSEUDO/DIVMODPSEUDO/VARSHIFTPSEUDO,
// expanding this pseudo needs the object's final SP-relative offset, which
// isn't known until PrologEpilogInserter runs - so all of its expansion (not
// just the offset arithmetic) is deferred to
// MoeRegisterInfo::eliminateFrameIndex's FIADDR special case, which BuildMI's
// a real MOVE/LOADabs/ADD sequence using the GP4/GP5 scratch convention
// MoeFrameLowering::adjustStackPointer already established, then erases the
// pseudo.
def FIADDR : MoePseudo<(outs GPR:$dst), (ins moemem:$addr),
                        "#FIADDR $dst, $addr", []>;

//===----------------------------------------------------------------------===//
// Sub-word (byte/halfword) LOAD/STORE, register-indirect addressing mode -
// see 'Size selection'/'Bytes selection' in the Encoding chapter. STORE has
// a real Pat matching a plain truncstore directly: memory isn't
// register-lane-merged, so a single instruction (size=Byte/Halfword,
// bytesel=0 - confirmed against emulator/src/execute.rs that bytesel only
// selects a *register* lane, never adjusting the effective memory address)
// is all a byte/halfword store needs.
//
// LOAD is different and deliberately Pat-less (mirrors PUSH/POP's
// empty-pattern precedent): per 'Handling words, halfwords and bytes' in the
// Encoding chapter, a Byte/Halfword-size LOAD only overwrites the selected
// lane of its destination register, leaving the rest unchanged - unlike most
// RISC ISAs' natively-extending loads. So this raw instruction is only ever
// useful as a building block for a real zext/sext, built explicitly by
// MoeISelLowering::LowerExtLoad (zero the destination first for zext, or
// discard the untouched garbage bits via the existing constant-shift-chain
// machinery for sext) - never matched from a bare load/zextload/sextload DAG
// node standalone. The tied "$dst = $oldval" operand models the hardware's
// genuine partial-merge-write semantic directly, the same way INCREMENT's
// existing "$o = $oin" tie already does for its own read-modify-write.
//===----------------------------------------------------------------------===//

let bytesel = 0b00, size = 0b01 in // Byte
def STORErr_B : LoadStoreForm<0b0110, 0b10, (outs), (ins GPR:$reg, moemem:$addr),
                               "store.b $reg -> $addr",
                               [(truncstorei8 GPR:$reg, moeaddr_ri:$addr)]>;

let bytesel = 0b00, size = 0b10 in // Halfword
def STORErr_H : LoadStoreForm<0b0110, 0b10, (outs), (ins GPR:$reg, moemem:$addr),
                               "store.h $reg -> $addr",
                               [(truncstorei16 GPR:$reg, moeaddr_ri:$addr)]>;

// The output must be named $reg (not e.g. $dst) - LoadStoreForm's bits<4>
// reg encoding field auto-binds to an operand of that exact name, the same
// way every other LoadStoreForm instruction's destination/source operand is
// named $reg.
let bytesel = 0b00, size = 0b01 in { // Byte
def LOADrr_B : LoadStoreForm<0b0101, 0b10, (outs GPR:$reg),
                              (ins GPR:$oldval, moemem:$addr),
                              "load.b $addr -> $reg", []> {
  let Constraints = "$reg = $oldval";
}
}

let bytesel = 0b00, size = 0b10 in { // Halfword
def LOADrr_H : LoadStoreForm<0b0101, 0b10, (outs GPR:$reg),
                              (ins GPR:$oldval, moemem:$addr),
                              "load.h $addr -> $reg", []> {
  let Constraints = "$reg = $oldval";
}
}
