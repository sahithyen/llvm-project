//===-- MoeMCAsmInfo.cpp - Moe asm properties ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declarations of the MoeMCAsmInfo properties.
//
//===----------------------------------------------------------------------===//

#include "MoeMCAsmInfo.h"
using namespace llvm;

void MoeMCAsmInfo::anchor() {}

MoeMCAsmInfo::MoeMCAsmInfo(const Triple &TT) {
  // 32-bit registers/pointers throughout - see 'Register selection' in the
  // Encoding chapter.
  CodePointerSize = 4;
  CalleeSaveStackSlotSize = 4;

  CommentString = ";";
  // With ';' taken by comments, statements on one line are separated by a
  // backtick instead - ARC's convention, for the same reason. The default
  // separator is also ';', which silently made everything after the first
  // statement a comment: Linux's ENTRY() macro joins `.globl name`, the
  // alignment and `name:` with a separator, and got only the `.globl`.
  SeparatorString = "`";

  AlignmentIsInBytes = false;
  UsesELFSectionDirectiveForBSS = true;

  SupportsDebugInformation = true;

  // No CFI/DWARF unwind directives for Milestone 1 - see the Milestone 1
  // plan's FrameLowering notes.
  ExceptionsType = ExceptionHandling::None;
}
