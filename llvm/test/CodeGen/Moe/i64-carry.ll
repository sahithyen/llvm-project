; RUN: llc < %s -mtriple=moe -O2 | FileCheck %s
;
; A 64-bit add or subtract is a glued pair: ADD or SUB on the low words, which
; sets C, then add.c or sub.c on the high words, which consumes it. Until
; 2026-09-30 the type legalizer recovered the carry with a compare and a
; branch, and SUB's Carry in could not have been used for it: the ISA made it
; A + NOT B + NOT C, which chains nothing ('Flags' in the Encoding chapter).

; CHECK-LABEL: add64:
; CHECK-NOT: jump
; CHECK: add gp1, gp3 -> [[LO:gp[0-9]]]
; CHECK-NEXT: add.c gp2, {{gp[0-9]}} -> [[HI:gp[0-9]]]
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
define i64 @add64(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}

; CHECK-LABEL: sub64:
; CHECK-NOT: jump
; CHECK: sub gp1, gp3 -> [[LO:gp[0-9]]]
; CHECK-NEXT: sub.c gp2, {{gp[0-9]}} -> [[HI:gp[0-9]]]
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
define i64 @sub64(i64 %a, i64 %b) {
  %r = sub i64 %a, %b
  ret i64 %r
}

; Three words chain through two carried instructions. A reload may sit between
; them - a LOAD leaves F alone - but no branch.
; CHECK-LABEL: add96:
; CHECK-NOT: jump
; CHECK: add.c gp
; CHECK-NOT: jump
; CHECK: add.c gp
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
define i96 @add96(i96 %a, i96 %b) {
  %r = add i96 %a, %b
  ret i96 %r
}
