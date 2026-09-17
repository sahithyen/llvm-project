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
  RelType getDynRel(RelType type) const override;
  int64_t getImplicitAddend(const uint8_t *buf, RelType type) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
};
} // namespace

Moe::Moe(Ctx &ctx) : TargetInfo(ctx) {
  // No hardware trap instruction exists (all 16 opcodes are real
  // instructions - see CLAUDE.md's "no opcode sharing" note); zero-fill
  // matches MoeAsmBackend::writeNopData's reasoning for the same regions.
  trapInstr = {0, 0, 0, 0};

  // What makes a position-independent executable linkable. An absolute
  // reference inside a PIE cannot be resolved at link time because the load
  // address is not known then - so the linker turns it into a dynamic
  // relocation saying "add the load bias to this word", and something at
  // startup applies it. `symbolicRel` names the absolute relocation that may be
  // converted; `relativeRel` names what it converts into.
  //
  // Moe needs no PC-relative relocation to do this, which is the surprising
  // part: the *addresses* stay absolute and get biased at load time. Where
  // position independence does need a PC-relative address - the startup code
  // finding its own relocation table before any of them are valid - the ISA
  // supplies it directly, because a register-indirect access may name IA.
  relativeRel = R_MOE_RELATIVE;
  symbolicRel = R_MOE_32;
}

RelExpr Moe::getRelExpr(RelType type, const Symbol &s,
                        const uint8_t *loc) const {
  // R_MOE_32 is the only relocation the assembler emits: every relocatable
  // reference is a flat 32-bit absolute address (see MoeMCCodeEmitter.cpp and
  // MoeELFObjectWriter.cpp). R_MOE_RELATIVE is the linker's own, and is never
  // seen on input.
  return R_ABS;
}

int64_t Moe::getImplicitAddend(const uint8_t *buf, RelType type) const {
  // 32-bit ELF uses REL, so a dynamic relocation's addend lives in the word it
  // points at rather than in the relocation entry. For R_MOE_RELATIVE that word
  // already holds the link-time address, and adding the load bias to it is the
  // whole operation - so the addend is simply what is there.
  switch (type) {
  case R_MOE_32:
  case R_MOE_RELATIVE:
    return read32le(buf);
  default:
    InternalErr(ctx, buf) << "cannot read addend for relocation " << type;
    return 0;
  }
}

RelType Moe::getDynRel(RelType type) const {
  // Which input relocations may survive as dynamic ones. Returning symbolicRel
  // here is what lets an absolute reference inside a position-independent
  // executable be rewritten as "add the load bias", rather than being rejected
  // for having no link-time answer.
  //
  // Moe needs it for *every* address, and that is a consequence of the
  // encoding rather than of the compiler: JUMP/LOAD/STORE carry their operand
  // in a trailing word inside .text, so a position-independent program's
  // relocations land in an executable section. That is what DT_TEXTREL means,
  // and it is why linking one needs `-z notext` until the backend can put
  // addresses in a GOT instead.
  return type == R_MOE_32 ? R_MOE_32 : 0;
}

void Moe::relocate(uint8_t *loc, const Relocation &rel, uint64_t val) const {
  switch (rel.type) {
  case R_MOE_32:
  case R_MOE_RELATIVE:
    write32le(loc, val);
    break;
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }
}

void elf::setMoeTargetInfo(Ctx &ctx) { ctx.target.reset(new Moe(ctx)); }
