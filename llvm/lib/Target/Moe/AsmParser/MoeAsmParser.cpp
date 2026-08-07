//===- MoeAsmParser.cpp - Parse Moe assembly to MCInst instructions -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Milestone 15: standalone `.s` assembly (llvm-mc) and inline asm both need
// this - Clang's inline-asm codegen path assembles inline asm text through
// the target's AsmParser at the integrated-assembler stage (-filetype=obj,
// the only mode this project uses - there's no external `as`), so inline
// asm was never independently reachable before this existed.
//
// Scope, documented rather than silently assumed: covers register/immediate/
// [reg+offset]-memory operand instructions (MOVE, ADD, SUB, AND, OR, XOR,
// INCREMENT, LOAD/STORE's register-indirect form) via the standard
// TableGen-generated matcher - modeled on MSP430AsmParser.cpp, the closest
// real in-tree precedent for a small, regular instruction set. Conditional/
// unconditional jump mnemonics (JMP/JCC's shared "jump.$cond $dst" printed
// form) are NOT covered: unlike every other instruction's fixed-literal
// mnemonic, that one embeds a substituted operand directly in the mnemonic
// text itself, which - confirmed by reading MSP430's own AsmParser, a real,
// mature in-tree target that hits the exact same shape for its own "jXX"
// conditional jumps - needs hand-written mnemonic-splitting special-case
// parsing, not something the generic matcher handles automatically. Left
// for a follow-up rather than expanding this milestone's scope further.
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MoeMCTargetDesc.h"
#include "TargetInfo/MoeTargetInfo.h"

#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define DEBUG_TYPE "moe-asm-parser"

namespace {

class MoeOperand : public MCParsedAsmOperand {
  enum KindTy { k_Imm, k_Reg, k_Tok } Kind;

  union {
    const MCExpr *Imm;
    MCRegister Reg;
    StringRef Tok;
  };

  SMLoc Start, End;

public:
  MoeOperand(StringRef Tok, SMLoc S) : Kind(k_Tok), Tok(Tok), Start(S), End(S) {}
  MoeOperand(MCRegister Reg, SMLoc S, SMLoc E)
      : Kind(k_Reg), Reg(Reg), Start(S), End(E) {}
  MoeOperand(const MCExpr *Imm, SMLoc S, SMLoc E)
      : Kind(k_Imm), Imm(Imm), Start(S), End(E) {}

  bool isReg() const override { return Kind == k_Reg; }
  bool isImm() const override { return Kind == k_Imm; }
  bool isToken() const override { return Kind == k_Tok; }
  // moemem's two sub-operands are pushed as independent Reg/Imm operands
  // (see parseOperand's bracket-syntax comment) - this target never
  // produces a genuinely compound "memory" MCParsedAsmOperand.
  bool isMem() const override { return false; }

  MCRegister getReg() const override {
    assert(Kind == k_Reg);
    return Reg;
  }

  StringRef getToken() const {
    assert(Kind == k_Tok);
    return Tok;
  }

  SMLoc getStartLoc() const override { return Start; }
  SMLoc getEndLoc() const override { return End; }

  void addExprOperand(MCInst &Inst, const MCExpr *Expr) const {
    if (const MCConstantExpr *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && Kind == k_Reg);
    Inst.addOperand(MCOperand::createReg(Reg));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && Kind == k_Imm);
    addExprOperand(Inst, Imm);
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case k_Tok:
      OS << "Token " << Tok;
      break;
    case k_Reg:
      OS << "Register " << Reg.id();
      break;
    case k_Imm:
      OS << "Immediate ";
      MAI.printExpr(OS, *Imm);
      break;
    }
  }

  static std::unique_ptr<MoeOperand> CreateToken(StringRef Str, SMLoc S) {
    return std::make_unique<MoeOperand>(Str, S);
  }
  static std::unique_ptr<MoeOperand> CreateReg(MCRegister Reg, SMLoc S, SMLoc E) {
    return std::make_unique<MoeOperand>(Reg, S, E);
  }
  static std::unique_ptr<MoeOperand> CreateImm(const MCExpr *Val, SMLoc S,
                                                SMLoc E) {
    return std::make_unique<MoeOperand>(Val, S, E);
  }
};

class MoeAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                OperandVector &Operands, MCStreamer &Out,
                                uint64_t &ErrorInfo,
                                bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  bool parseOperand(OperandVector &Operands);

  MCAsmParser &getParser() const { return Parser; }
  AsmLexer &getLexer() const { return Parser.getLexer(); }

#define GET_ASSEMBLER_HEADER
#include "MoeGenAsmMatcher.inc"

public:
  MoeAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
               const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

} // end anonymous namespace

// Auto-generated by TableGen from MoeRegisterInfo.td's MoeReg names
// (gp0-gp7, ia, sp, tt, f, im, ifs, ra, rf).
static MCRegister MatchRegisterName(StringRef Name);

bool MoeAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                  SMLoc &EndLoc) {
  ParseStatus Res = tryParseRegister(Reg, StartLoc, EndLoc);
  if (Res.isFailure())
    return Error(StartLoc, "invalid register name");
  if (Res.isSuccess())
    return false;
  return true;
}

ParseStatus MoeAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                            SMLoc &EndLoc) {
  if (getLexer().getKind() != AsmToken::Identifier)
    return ParseStatus::NoMatch;

  StringRef Name = getLexer().getTok().getIdentifier().lower();
  Reg = MatchRegisterName(Name);
  if (!Reg)
    return ParseStatus::NoMatch;

  StartLoc = getLexer().getTok().getLoc();
  EndLoc = getLexer().getTok().getEndLoc();
  getLexer().Lex();
  return ParseStatus::Success;
}

// Operand forms: a bare register (gp0, sp, ...), a bare immediate/expression
// (constant or symbol), or [reg+offset] (LOAD/STORE's register-indirect
// addressing mode - see moemem's def comment in MoeInstrInfo.td).
bool MoeAsmParser::parseOperand(OperandVector &Operands) {
  SMLoc StartLoc = getLexer().getLoc();

  if (getLexer().is(AsmToken::LBrac)) {
    // moemem (LOAD/STORE's register-indirect addressing mode) has no
    // ParserMatchClass of its own in MoeInstrInfo.td - its MIOperandInfo =
    // (ops GPR, i32imm) makes TableGen's auto-generated matcher expect the
    // base register and offset immediate as two INDEPENDENT operands
    // (confirmed by reading the generated MoeGenAsmMatcher.inc: LOADrr's
    // entry lists { MCK_GPR, MCK_Imm, MCK__MINUS__GT_, MCK_GPR }, not one
    // combined memory-operand class), even though they're written together
    // as one bracketed [reg+offset] token from the user's perspective - so
    // this one syntactic operand pushes two real operands, not a combined
    // Mem kind.
    getLexer().Lex(); // eat '['
    MCRegister Base;
    SMLoc RegStart, RegEnd;
    if (parseRegister(Base, RegStart, RegEnd))
      return true;
    if (getLexer().isNot(AsmToken::Plus))
      return Error(getLexer().getLoc(), "expected '+' in memory operand");
    getLexer().Lex(); // eat '+'
    const MCExpr *Offset;
    if (getParser().parseExpression(Offset))
      return true;
    if (getLexer().isNot(AsmToken::RBrac))
      return Error(getLexer().getLoc(), "expected ']'");
    SMLoc EndLoc = getLexer().getTok().getEndLoc();
    getLexer().Lex(); // eat ']'
    Operands.push_back(MoeOperand::CreateReg(Base, RegStart, RegEnd));
    Operands.push_back(MoeOperand::CreateImm(Offset, StartLoc, EndLoc));
    return false;
  }

  MCRegister Reg;
  SMLoc RegStart, RegEnd;
  ParseStatus RegRes = tryParseRegister(Reg, RegStart, RegEnd);
  if (RegRes.isFailure())
    return true;
  if (RegRes.isSuccess()) {
    Operands.push_back(MoeOperand::CreateReg(Reg, RegStart, RegEnd));
    return false;
  }

  const MCExpr *Val;
  if (getParser().parseExpression(Val))
    return true;
  SMLoc EndLoc = getLexer().getLoc();
  Operands.push_back(MoeOperand::CreateImm(Val, StartLoc, EndLoc));
  return false;
}

bool MoeAsmParser::parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                                     SMLoc NameLoc, OperandVector &Operands) {
  Operands.push_back(MoeOperand::CreateToken(Name, NameLoc));

  if (getLexer().is(AsmToken::EndOfStatement))
    return false;

  if (parseOperand(Operands))
    return true;

  while (parseOptionalToken(AsmToken::Comma)) {
    if (parseOperand(Operands))
      return true;
  }

  // "sub $a, $b -> $o"-style AsmStrings (ADD/SUB/AND/OR/XOR) print a literal
  // "->" before the destination operand - the generic matcher handles this
  // as an ordinary literal token, but it still has to actually appear (and
  // be consumed) in the input stream between operands.
  if (getLexer().is(AsmToken::MinusGreater)) {
    Operands.push_back(MoeOperand::CreateToken("->", getLexer().getLoc()));
    getLexer().Lex();
    if (parseOperand(Operands))
      return true;
  }

  // INCREMENT's "$o += $value" - the lexer has no single "+=" token kind,
  // it's two separate tokens (Plus then Equal).
  if (getLexer().is(AsmToken::Plus)) {
    SMLoc Loc = getLexer().getLoc();
    getLexer().Lex();
    if (getLexer().isNot(AsmToken::Equal))
      return Error(getLexer().getLoc(), "expected '+='");
    getLexer().Lex();
    Operands.push_back(MoeOperand::CreateToken("+=", Loc));
    if (parseOperand(Operands))
      return true;
  }

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return Error(getLexer().getLoc(), "unexpected token in operand list");

  return false;
}

bool MoeAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                            OperandVector &Operands,
                                            MCStreamer &Out,
                                            uint64_t &ErrorInfo,
                                            bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned MatchResult =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  case Match_MnemonicFail:
    return Error(IDLoc, "invalid instruction mnemonic");
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL) {
      if (ErrorInfo >= Operands.size())
        return Error(ErrorLoc, "too few operands for instruction");
      ErrorLoc = ((MoeOperand &)*Operands[ErrorInfo]).getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = IDLoc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  default:
    return Error(IDLoc, "unrecognized instruction");
  }
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeMoeAsmParser() {
  RegisterMCAsmParser<MoeAsmParser> X(getTheMoeTarget());
}

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "MoeGenAsmMatcher.inc"
