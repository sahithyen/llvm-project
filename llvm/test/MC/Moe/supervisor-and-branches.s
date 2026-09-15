; RUN: llvm-mc -triple=moe -show-encoding %s | FileCheck %s
; RUN: llvm-mc -triple=moe -filetype=obj %s -o %t.o
; RUN: llvm-objdump -s -j .text %t.o | FileCheck --check-prefix=BYTES %s
;
; MC-level coverage for the constructs hand-written supervisor assembly needs.
; llvm-tests/supervisor_asm_test.s proves they do the right thing at runtime;
; this proves the exact bits, which is the part a TableGen bit-layout typo
; would silently get wrong while still executing "plausibly".
;
; Note -show-encoding only shows the 16-bit opcode halfword for JUMP/LOAD/
; STORE. Their trailing operand word is emitted by MoeELFStreamer, which
; MCAsmStreamer never reaches - see MoeELFStreamer.cpp. The BYTES checks below
; are what cover the trailing word, the alignment padding in front of it, and
; the literal-vs-symbol distinction.

  .text

; The three operand-less control-transfer instructions. Opcodes 0b0001 /
; 0b0010 / 0b0011 in bits 15-12, all other bits n/a and encoded 0 (see
; 'SYSCALL'/'IRET'/'INVALIDATE' in the Instruction set chapter).
; CHECK: syscall
; CHECK-SAME: encoding: [0x00,0x10]
  syscall
; CHECK: iret
; CHECK-SAME: encoding: [0x00,0x20]
  iret
; CHECK: invalidate
; CHECK-SAME: encoding: [0x00,0x30]
  invalidate

; Conditional jumps. Opcode 0b0000, addressing mode 0 (Absolute) in bits 9-8,
; condition in bits 3-0 - so the low byte is the condition and the high byte
; is 0x00. Values from 'Condition selection' in the Encoding chapter.
; CHECK: jump.EQ
; CHECK-SAME: encoding: [0x02,0x00]
  jump.eq target
; CHECK: jump.NE
; CHECK-SAME: encoding: [0x03,0x00]
  jump.ne target
; CHECK: jump.LE
; CHECK-SAME: encoding: [0x0f,0x00]
  jump.le target

; Uppercase is what llc itself emits (printCCOperand prints the condition in
; upper case), so it has to assemble to the identical encoding.
; CHECK: jump.CS
; CHECK-SAME: encoding: [0x04,0x00]
  jump.CS target

target:
  jump.al target

; A literal absolute trailing operand - the trap vector, the reset vector, an
; MMIO register. This used to abort llvm-mc on an assertion rather than
; assemble or diagnose.
  jump.al 0x400

; .word is 32 bits on Moe.
  .word 0xAABBCCDD

; BYTES: Contents of section .text:
; Walking the layout, which is the point of these checks - a trailing operand
; word lands on the next word-aligned address after its 16-bit opcode
; halfword, so whether two bytes of padding appear in between depends on where
; that halfword sits (see 'Addressing mode' in the Encoding chapter):
;
;   0x00 syscall, 0x02 iret, 0x04 invalidate      - no trailing word each
;   0x06 jump.eq  -> operand at 0x08, no padding  (0x08 is already aligned)
;   0x0c jump.ne  -> padding 0x0e, operand 0x10
;   0x14 jump.le  -> padding 0x16, operand 0x18
;   0x1c jump.CS  -> padding 0x1e, operand 0x20
;   0x24 jump.al target -> padding 0x26, operand 0x28
;   0x2c jump.al 0x400  -> padding 0x2e, operand 0x30 = 00 04 00 00
;   0x34 .word 0xAABBCCDD -> dd cc bb aa (little-endian, 32 bits)
;
; The alternation between "padded" and "not padded" is exactly what a fixed
; assumption about the instruction's position gets wrong.
;
; Targets read 00000000 here because they are unapplied R_MOE_32 relocations
; in a .o; the literal 0x400 is not a relocation at all, which is the
; distinction check 9 of llvm-tests/supervisor_asm_test.s exercises at runtime.
; BYTES-NEXT: 0000 00100020 00300200 00000000 03000000
; BYTES-NEXT: 0010 00000000 0f000000 00000000 04000000
; BYTES-NEXT: 0020 00000000 00000000 00000000 00000000
; BYTES-NEXT: 0030 00040000 ddccbbaa
