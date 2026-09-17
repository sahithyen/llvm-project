; RUN: llc < %s -mtriple=moe -O0 | FileCheck %s
;
; Milestone 3's global-variable checkpoint (see the Milestone 3 plan's
; "Static/global data support" section): an initialized global (@g) and a
; zero-initialized one (@z) both round-trip through LowerGlobalAddress's
; constant-pool indirection (the same Wrapper mechanism a call target or
; literal constant goes through - see MoeISelLowering.cpp), landing in
; .data/.bss respectively (verified separately via llvm-readobj in
; llvm-tests/run-globals-test.sh, which also proves the store actually
; executes correctly, not just looks plausible in this text-only check).

; The `.s` forms are Addressing mode's short register-indirect: the operand is
; the halfword after the opcode rather than a word at the next aligned address,
; so the instruction is one aligned word (isa-evaluation.md's S9). MoeMCInstLower
; picks it whenever the offset fits twelve signed bits, which every offset here
; does - so what this file checks now is that it is picked, as well as that the
; global round-trips.

; CHECK-LABEL: compute:
; CHECK: load.w .LCPI0_0 -> gp0
; CHECK: load.w.s [gp0+0] -> gp1
; CHECK: load.w .LCPI0_1 -> gp2
; CHECK: add gp1, gp2 -> gp1
; CHECK: store.w.s gp1 -> [gp0+0]
; CHECK: load.w.s [gp0+0] -> gp0
; CHECK: load.w .LCPI0_2 -> gp1
; CHECK: load.w.s [gp1+0] -> gp1
; CHECK: add gp0, gp1 -> gp0
@g = global i32 5
@z = global i32 0

define i32 @compute() {
  %old = load i32, ptr @g
  %new = add i32 %old, 37
  store i32 %new, ptr @g
  %g2 = load i32, ptr @g
  %zv = load i32, ptr @z
  %sum = add i32 %g2, %zv
  ret i32 %sum
}
