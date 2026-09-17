; RUN: llc -mtriple=moe -O0 < %s | FileCheck %s
; RUN: env MOE_NO_SHORT_BRANCH=1 llc -mtriple=moe -O0 < %s | FileCheck %s --check-prefix=LONG

; The ISA evaluation's N1: a branch to one of this function's own blocks is
; four bytes rather than eight when its target is within the +-2 KiB a 12-bit
; displacement reaches. MoeShortBranches picks the form, because the
; displacement is a symbol difference nothing knows until layout. See that
; file's header for why it cannot be done in MoeMCInstLower or by relaxation.

define i32 @near(i32 %n) {
; CHECK-LABEL: near:
; CHECK:         jump.{{[A-Z]+}}.pc.s .LBB0_
; CHECK:         jump.al.pc.s .LBB0_
; LONG-LABEL:  near:
; LONG-NOT:      .pc.s
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %yes, label %no
yes:
  ret i32 1
no:
  ret i32 0
}

; A branch whose span contains inline assembly keeps the long form, however
; close the target looks. An inline-asm length is a guess, not a bound -
; `.space 8192` is one statement and eight thousand bytes - and a guess inside
; the span is the one thing that could make the real displacement larger than
; the predicted one. The branch to %no jumps over the asm and so stays long;
; the one to %yes lands before it and is still shortened, which is what keeps
; this check from passing for the wrong reason.
define i32 @across_inline_asm(i32 %n) {
; CHECK-LABEL: across_inline_asm:
; CHECK:         jump.{{[A-Z]+}} .LBB1_2
; CHECK-NEXT:    jump.al.pc.s .LBB1_1
; CHECK:         .zero 8192
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %yes, label %no
yes:
  call void asm sideeffect ".space 8192", ""()
  ret i32 1
no:
  ret i32 0
}
