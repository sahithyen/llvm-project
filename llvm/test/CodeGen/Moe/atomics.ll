; RUN: llc < %s -mtriple=moe -O0 | FileCheck %s
;
; What the execution test (llvm-tests/run-atomic-test.sh) cannot check: which
; libcall each atomic picked, and the *absence* of any instruction for a fence.
;
; Both are things the execution test would pass over. A byte atomicrmw lowered
; to __atomic_fetch_add_4 would still compute the right answer for a word-
; aligned byte, and a fence that emitted a spurious instruction would still be
; correct - just wrong, and only visible here.
;
; The whole set exists because the ISA has no atomic instruction at any width,
; so MoeISelLowering's setMaxAtomicSizeInBitsSupported(0) sends every one of
; them to a libcall (runtime/atomic.ll implements them).

; CHECK-LABEL: rmw_byte:
; CHECK: jump.al __atomic_fetch_add_1
define i8 @rmw_byte(ptr %p) {
  %v = atomicrmw add ptr %p, i8 1 seq_cst
  ret i8 %v
}

; CHECK-LABEL: rmw_half:
; CHECK: jump.al __atomic_fetch_sub_2
define i16 @rmw_half(ptr %p) {
  %v = atomicrmw sub ptr %p, i16 1 seq_cst
  ret i16 %v
}

; CHECK-LABEL: rmw_word:
; CHECK: jump.al __atomic_fetch_or_4
define i32 @rmw_word(ptr %p) {
  %v = atomicrmw or ptr %p, i32 1 seq_cst
  ret i32 %v
}

; CHECK-LABEL: xchg_word:
; CHECK: jump.al __atomic_exchange_4
define i32 @xchg_word(ptr %p, i32 %v) {
  %r = atomicrmw xchg ptr %p, i32 %v seq_cst
  ret i32 %r
}

; CHECK-LABEL: cas_word:
; CHECK: jump.al __atomic_compare_exchange_4
define i32 @cas_word(ptr %p, i32 %old, i32 %new) {
  %pair = cmpxchg ptr %p, i32 %old, i32 %new seq_cst seq_cst
  %v = extractvalue { i32, i1 } %pair, 0
  ret i32 %v
}

; An atomic load and store are libcalls too, even though the machine could do
; either in one instruction: the decision is made once, by max atomic size, so
; that there is one place the atomics come from rather than two.
;
; CHECK-LABEL: load_word:
; CHECK: jump.al __atomic_load_4
define i32 @load_word(ptr %p) {
  %v = load atomic i32, ptr %p seq_cst, align 4
  ret i32 %v
}

; CHECK-LABEL: store_word:
; CHECK: jump.al __atomic_store_4
define void @store_word(ptr %p, i32 %v) {
  store atomic i32 %v, ptr %p seq_cst, align 4
  ret void
}

; A fence emits no instruction at all - one core, in order, nothing to tell
; the hardware. It must not become a call to __sync_synchronize either, which
; is what leaving ATOMIC_FENCE at its Expand default would have produced.
;
; CHECK-LABEL: just_a_fence:
; CHECK-NOT: jump.al
; CHECK-NOT: __sync_synchronize
; CHECK: pop gp4
define void @just_a_fence() {
  fence seq_cst
  ret void
}
