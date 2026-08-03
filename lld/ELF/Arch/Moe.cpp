//===- Moe.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "Target.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {
class Moe final : public TargetInfo {
public:
  Moe(Ctx &);
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
};
} // namespace

Moe::Moe(Ctx &ctx) : TargetInfo(ctx) {
  // No hardware trap instruction exists (all 16 opcodes are real
  // instructions - see CLAUDE.md's "no opcode sharing" note); zero-fill
  // matches MoeAsmBackend::writeNopData's reasoning for the same regions.
  trapInstr = {0, 0, 0, 0};
}

RelExpr Moe::getRelExpr(RelType type, const Symbol &s,
                        const uint8_t *loc) const {
  // R_MOE_32 is the only relocation type Milestone 2 emits (every
  // relocatable reference is a flat 32-bit absolute address - see
  // MoeMCCodeEmitter.cpp/MoeELFObjectWriter.cpp).
  return R_ABS;
}

void Moe::relocate(uint8_t *loc, const Relocation &rel, uint64_t val) const {
  switch (rel.type) {
  case R_MOE_32:
    write32le(loc, val);
    break;
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }
}

void elf::setMoeTargetInfo(Ctx &ctx) { ctx.target.reset(new Moe(ctx)); }
