; RUN: llc < %s -mtriple=moe -O2 | FileCheck %s
;
; A computed goto selects to BRIND, which encodes as MOVE -> IA. What this
; checks that the execution test (llvm-tests/run-brind-test.sh) cannot is the
; *absence* of an instruction: BRIND carries terminator and barrier flags, so
; block placement must not append a jump to the layout successor behind it -
; dead code that only ever costs bytes, but which is what a plain MOVE into IA
; gets you.

define i32 @dispatch(ptr %target) {
entry:
  indirectbr ptr %target, [label %a, label %b]

a:
  ret i32 1

b:
  ret i32 2
}

; CHECK-LABEL: dispatch:
; CHECK: move gp0 -> ia
; CHECK-NOT: jump.al
; CHECK: {{^.L}}
