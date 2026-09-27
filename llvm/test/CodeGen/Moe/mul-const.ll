; A multiply by a constant is a shift-and-add sequence over the constant's
; non-adjacent form (candidate N5) wherever that is cheaper than MULDIV's MUL
; with the constant loaded (candidate L8); MOE_NO_SHIFT_ADD_MUL makes every one
; a MUL. llvm-tests/run-arith-loops-test.sh is what checks the answers - this
; checks the shape.
; RUN: llc -mtriple=moe -O2 -verify-machineinstrs < %s | FileCheck %s
; RUN: env MOE_NO_SHIFT_ADD_MUL=1 llc -mtriple=moe -O2 < %s \
; RUN:   | FileCheck %s --check-prefix=LOOP

; 0x01010101, which InstCombine makes out of memset's `p |= p << 8; p |= p << 16`.
; CHECK-LABEL: broadcast:
; CHECK-NOT: jump
; CHECK: shift.left.4 [[X:gp[0-9]]] -> [[T:gp[0-9]]]
; CHECK-NEXT: shift.left.4 [[T]] -> [[T]]
; CHECK-NEXT: add [[T]], [[X]] -> [[T]]
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
; LOOP-LABEL: broadcast:
; LOOP: mul
define i32 @broadcast(i32 %a) {
  %r = mul i32 %a, 16843009
  ret i32 %r
}

; 11 is 16 - 4 - 1 in non-adjacent form: two subtractions, no addition.
; CHECK-LABEL: times11:
; CHECK-NOT: add
; CHECK: sub
; CHECK: sub
; CHECK-NOT: jump
; CHECK: move gp4 -> ia
define i32 @times11(i32 %a) {
  %r = mul i32 %a, 11
  ret i32 %r
}

; -1 is 2^32 - 1, and the 2^32 digit vanishes: one negation.
; CHECK-LABEL: negate:
; CHECK: sub
; CHECK-NOT: shift
; CHECK: move gp4 -> ia
define i32 @negate(i32 %a) {
  %r = mul i32 %a, -1
  ret i32 %r
}

; 0x55555555 has sixteen nonzero digits, which costs more than a MUL does.
; CHECK-LABEL: dense:
; CHECK: mul
define i32 @dense(i32 %a) {
  %r = mul i32 %a, 1431655765
  ret i32 %r
}

; A variable multiply is a MUL.
; CHECK-LABEL: variable:
; CHECK: mul
define i32 @variable(i32 %a, i32 %b) {
  %r = mul i32 %a, %b
  ret i32 %r
}
