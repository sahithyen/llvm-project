; RUN: llc < %s -mtriple=moe -O2 | FileCheck %s
;
; A constant offset from a pointer is folded into Register-indirect's operand,
; not materialized. This ISA has no immediate, so LowerConstantPool turns every
; constant into a pool LOAD during legalization - before SelectAddr looks at an
; address - and for as long as SelectAddr only recognised a ConstantSDNode, no
; `p[k]` with k != 0 ever folded: each was a pool LOAD, an ADD and an access
; through [reg+0]. See poolConstantOffset in MoeISelDAGToDAG.cpp.

; A field load: one LOAD at [base+8], no pool load and no ADD.
; CHECK-LABEL: field:
; CHECK-NOT: add
; CHECK: load.w.s [gp0+8] -> gp0
; CHECK-NOT: add
; CHECK: move gp4 -> ia
define i32 @field(ptr %p) {
  %a = getelementptr i8, ptr %p, i32 8
  %v = load i32, ptr %a
  ret i32 %v
}

; A store, and a negative offset, which the short form's twelve signed bits
; also carry.
; CHECK-LABEL: store_neg:
; CHECK-NOT: add
; (The printer writes a negative offset as `+-12`, which the assembler reads.)
; CHECK: store.w.s gp1 -> [gp0+-12]
define void @store_neg(ptr %p, i32 %v) {
  %a = getelementptr i8, ptr %p, i32 -12
  store i32 %v, ptr %a
  ret void
}

; An offset too wide for the short form still folds, into the word form.
; CHECK-LABEL: wide:
; CHECK-NOT: add
; CHECK: load.w [gp0+40000] -> gp0
define i32 @wide(ptr %p) {
  %a = getelementptr i8, ptr %p, i32 40000
  %v = load i32, ptr %a
  ret i32 %v
}

; And one too wide for the word form's 28 bits does not: it stays a pool
; constant and an ADD.
; CHECK-LABEL: too_wide:
; CHECK: load.w .LCPI{{[0-9_]+}} -> [[R:gp[0-9]]]
; CHECK: add gp0, [[R]] -> [[A:gp[0-9]]]
; CHECK: load.w.s [[[A]]+0] -> gp0
define i32 @too_wide(ptr %p) {
  %a = getelementptr i8, ptr %p, i32 268435456
  %v = load i32, ptr %a
  ret i32 %v
}

; Byte and halfword accesses take the same pattern.
; CHECK-LABEL: narrow:
; CHECK-DAG: store.b.s gp1 -> [gp0+3]
; CHECK-DAG: store.h.s gp1 -> [gp0+6]
define void @narrow(ptr %p, i32 %v) {
  %b = getelementptr i8, ptr %p, i32 3
  %t8 = trunc i32 %v to i8
  store i8 %t8, ptr %b
  %h = getelementptr i8, ptr %p, i32 6
  %t16 = trunc i32 %v to i16
  store i16 %t16, ptr %h
  ret void
}
