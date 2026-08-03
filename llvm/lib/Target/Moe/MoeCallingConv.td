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
  CCIfType<[i32], CCAssignToReg<[GP0, GP1, GP2, GP3]>>,

  // Overflow args go on the stack, word-sized and word-aligned (PUSH/POP and
  // SP always move by exactly one word regardless of size - see 'Stack' in
  // the Encoding chapter).
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;
