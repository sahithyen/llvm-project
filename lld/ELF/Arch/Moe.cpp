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
  // Two relocations reach here, one per addressing form the assembler emits
  // (MoeELFStreamer.cpp). R_MOE_RELATIVE is the linker's own and is never seen
  // on input.
  switch (type) {
  case R_MOE_PCREL28:
  case R_MOE_PCREL32:
    return R_PC;
  case R_MOE_32:
  default:
    return R_ABS;
  }
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
  // What survives is only what is in *data*: a pointer in .data, and the
  // constant-pool word that holds a global's address, which the compiler now
  // puts in .data.rel.ro. Code needs nothing, because a position-independent
  // function reaches every address as a displacement from IA (R_MOE_PCREL28),
  // which the linker resolves once and for all.
  //
  // It read differently not long ago. JUMP/LOAD/STORE carry their operand in a
  // trailing word inside .text, so when the compiler emitted absolute addresses
  // there, a PIE's relocations landed in an executable section - DT_TEXTREL, and
  // `-z notext` to permit it. Nothing about the encoding forced that; what
  // forced it was that the backend had no PC-relative relocation to emit.
  return type == R_MOE_32 ? R_MOE_32 : 0;
}

void Moe::relocate(uint8_t *loc, const Relocation &rel, uint64_t val) const {
  switch (rel.type) {
  case R_MOE_32:
  case R_MOE_RELATIVE:
  case R_MOE_PCREL32:
    write32le(loc, val);
    break;
  case R_MOE_PCREL28: {
    // Bits 31-4 only: the low nibble is the base register select (IA), which
    // the assembler put there and which this must leave standing. Writing the
    // whole word - the obvious thing, and what R_MOE_32 above does - would
    // zero it and turn every PC-relative reference into a GP0-relative one.
    checkInt(ctx, loc, static_cast<int64_t>(val), 28, rel);
    uint32_t base = read32le(loc) & 0xf;
    write32le(loc, (static_cast<uint32_t>(val) << 4) | base);
    break;
  }
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }
}

void elf::setMoeTargetInfo(Ctx &ctx) { ctx.target.reset(new Moe(ctx)); }
