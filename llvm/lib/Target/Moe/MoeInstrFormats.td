//===-- MoeInstrFormats.td - Moe Instruction Formats ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bit layouts here mirror specification/src/instruction_set.md exactly -
// see that file's per-opcode tables. Every real (non-pseudo) instruction is
// a single 16-bit halfword; JUMP/LOAD/STORE additionally consume a trailing
// word-aligned operand word from the instruction stream (see 'Addressing
// mode' in the Encoding chapter), which is not part of this 16-bit Inst
// encoding - MoeAsmPrinter/MoeMCInstLower emit it separately. Milestone 1
// does not wire up -gen-emitter/-gen-disassembler (asm-text output only, see
// the Milestone 1 plan), so these bit encodings aren't exercised by the
// build yet, but are defined precisely now to match the spec since they'll
// be needed as soon as real MC-layer encoding is added.
//
//===----------------------------------------------------------------------===//

class MoeInst<dag outs, dag ins, string asmstr, list<dag> pattern>
    : Instruction {
  field bits<16> Inst;

  let Namespace = "Moe";
  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
  let Size = 2;
}

// Pseudo-instructions: not part of the real ISA, expanded away before
// MC emission (or, for Milestone 1's asm-text-only scope, printed via a
// custom EmitInstruction path in MoeAsmPrinter). No Inst encoding.
class MoePseudo<dag outs, dag ins, string asmstr, list<dag> pattern>
    : Instruction {
  let Namespace = "Moe";
  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
  let isPseudo = 1;
}

//===----------------------------------------------------------------------===//
// Condition selection (JUMP) - see 'Condition selection' in the Encoding
// chapter. Numeric values match the spec exactly.
//===----------------------------------------------------------------------===//

class MoeCC<bits<4> val> {
  bits<4> Value = val;
}
def MOECC_AL : MoeCC<0>;
def MOECC_NV : MoeCC<1>;
def MOECC_EQ : MoeCC<2>;
def MOECC_NE : MoeCC<3>;
def MOECC_CS : MoeCC<4>;
def MOECC_CC : MoeCC<5>;
def MOECC_MI : MoeCC<6>;
def MOECC_PL : MoeCC<7>;
def MOECC_VS : MoeCC<8>;
def MOECC_VC : MoeCC<9>;
def MOECC_HI : MoeCC<10>;
def MOECC_LS : MoeCC<11>;
def MOECC_GE : MoeCC<12>;
def MOECC_LT : MoeCC<13>;
def MOECC_GT : MoeCC<14>;
def MOECC_LE : MoeCC<15>;

//===----------------------------------------------------------------------===//
// Per-opcode formats. Field names match specification/src/instruction_set.md
// exactly. "Addressing mode" (JUMP/LOAD/STORE, 2 bits) is a plain literal
// (0 = Absolute, 1 = Page-relative, 2 = Register-indirect, 3 = Reserved)
// rather than a named enum like MoeCC, since only two format classes
// (JumpForm, LoadStoreForm) ever need it and each instruction def below
// picks a fixed value - see 'Addressing mode' in the Encoding chapter.
//===----------------------------------------------------------------------===//

// JUMP (opcode 0b0000)
class JumpForm<bits<2> addrmode, dag outs, dag ins, string asmstr,
               list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bits<4> cond;
  let Inst{15-12} = 0b0000;
  let Inst{11-10} = 0; // n/a
  let Inst{9-8}   = addrmode;
  let Inst{7-4}   = 0; // n/a
  let Inst{3-0}   = cond;
}

// SYSCALL (0b0001) / IRET (0b0010) / INVALIDATE (0b0011): no operand bits.
class NoOperandForm<bits<4> opcode, string asmstr, list<dag> pattern>
    : MoeInst<(outs), (ins), asmstr, pattern> {
  let Inst{15-12} = opcode;
  let Inst{11-0}  = 0; // n/a
}

// MOVE (opcode 0b0100)
class MoveForm<dag outs, dag ins, string asmstr, list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bits<4> mask;
  bits<4> from;
  bits<4> to;
  let Inst{15-12} = 0b0100;
  let Inst{11-8}  = mask;
  let Inst{7-4}   = from;
  let Inst{3-0}   = to;
}

// LOAD (0b0101) / STORE (0b0110)
class LoadStoreForm<bits<4> opcode, bits<2> addrmode, dag outs, dag ins,
                     string asmstr, list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bits<2> bytesel = 0b00;
  bits<2> size = 0b00;
  bits<4> reg;
  let Inst{15-12} = opcode;
  let Inst{11-10} = 0; // n/a
  let Inst{9-8}   = addrmode;
  let Inst{7-6}   = bytesel;
  let Inst{5-4}   = size;
  let Inst{3-0}   = reg;
}

// PUSH (0b0111) / POP (0b1000)
class PushPopForm<bits<4> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bits<2> bytesel = 0b00;
  bits<2> size = 0b00;
  bits<4> reg;
  let Inst{15-12} = opcode;
  let Inst{11-8}  = 0; // n/a
  let Inst{7-6}   = bytesel;
  let Inst{5-4}   = size;
  let Inst{3-0}   = reg;
}

// ADD (0b1001) / SUB (0b1010)
class AddSubForm<bits<4> opcode, dag outs, dag ins, string asmstr,
                  list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bit carryin = 0;
  bits<2> size = 0b00;
  bits<3> b;
  bits<3> a;
  bits<3> o;
  let Inst{15-12} = opcode;
  let Inst{11}    = carryin;
  let Inst{10-9}  = size;
  let Inst{8-6}   = b;
  let Inst{5-3}   = a;
  let Inst{2-0}   = o;
}

// INCREMENT (0b1011)
class IncrementForm<dag outs, dag ins, string asmstr, list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bits<2> size = 0b00;
  bits<6> value;
  bits<4> o;
  let Inst{15-12} = 0b1011;
  let Inst{11-10} = size;
  let Inst{9-4}   = value;
  let Inst{3-0}   = o;
}

// AND (0b1100) / OR (0b1101) / XOR (0b1110)
class AluTestForm<bits<4> opcode, dag outs, dag ins, string asmstr,
                   list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bit test = 0;
  bits<2> size = 0b00;
  bits<3> b;
  bits<3> a;
  bits<3> o;
  let Inst{15-12} = opcode;
  let Inst{11}    = test;
  let Inst{10-9}  = size;
  let Inst{8-6}   = b;
  let Inst{5-3}   = a;
  let Inst{2-0}   = o;
}

// SHIFT (0b1111)
class ShiftForm<dag outs, dag ins, string asmstr, list<dag> pattern>
    : MoeInst<outs, ins, asmstr, pattern> {
  bit test = 0;
  bits<2> size = 0b00;
  bit carryin = 0;
  bit right;
  bit arithmetic = 0;
  bits<3> a;
  bits<3> o;
  let Inst{15-12} = 0b1111;
  let Inst{11}    = test;
  let Inst{10-9}  = size;
  let Inst{8}     = carryin;
  let Inst{7}     = right;
  let Inst{6}     = arithmetic;
  let Inst{5-3}   = a;
  let Inst{2-0}   = o;
}
