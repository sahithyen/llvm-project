//==- MoeCallingConv.td - Calling Conventions for Moe -*- tablegen -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Moe's ISA defines no calling convention at all (confirmed absent from the
// spec and emulator) - this is the from-scratch strawman ABI chosen for the
// backend: GP0-GP3 for arguments (GP0 doubles as the return value), GP4-GP5
// reserved as caller-saved scratch for literal-pool/call-sequence
// temporaries (never assigned by this calling convention), GP6-GP7
// callee-saved.
//===----------------------------------------------------------------------===//

def RetCC_Moe : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[GP0]>>
]>;

def CC_Moe : CallingConv<[
  // A variadic function passes EVERY argument on the stack, named ones
  // included, so that the unnamed arguments sit contiguously right after the
  // named ones and va_list can be a plain pointer walking forward.
  //
  // The obvious alternative - keep GP0-GP3 for the first four arguments and
  // have the callee spill whichever were unconsumed into a register-save area
  // - does not work on Moe, and the reason is structural rather than an
  // oversight. A register-save area lives in the callee's own frame, while the
  // caller's overflow arguments sit above entry SP, and between the two lies
  // the return address: Moe has no hardware call instruction, so the caller
  // pushes the return address last and the callee's epilogue POPs it. Those
  // four bytes cannot be walked over by a uniform pointer-plus-size va_arg.
  // That was the previous design, and it silently produced garbage for the
  // fifth and later argument of any variadic call - which is every other
  // printk.
  //
  // The cost is that a variadic call passes its first four arguments through
  // memory rather than registers. On a machine that will run at a few hundred
  // kIPS that is a handful of extra instructions on calls that are already
  // dominated by formatting, and it buys a va_list that is one word and a
  // va_arg that is one load and one add.
  CCIfVarArg<CCIfType<[i32], CCAssignToStack<4, 4>>>,

  CCIfType<[i32], CCAssignToReg<[GP0, GP1, GP2, GP3]>>,

  // Overflow args go on the stack, word-sized and word-aligned (PUSH/POP and
  // SP always move by exactly one word regardless of size - see 'Stack' in
  // the Encoding chapter).
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;
