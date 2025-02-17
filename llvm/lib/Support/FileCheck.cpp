//===- FileCheck.cpp - Check that File's Contents match what is expected --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// FileCheck does a line-by line check of a file that validates whether it
// contains the expected content.  This is useful for regression tests etc.
//
// This file implements most of the API that will be used by the FileCheck utility
// as well as various unittests.
//===----------------------------------------------------------------------===//

#include "llvm/Support/FileCheck.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include <cstdint>
#include <list>
#include <map>
#include <tuple>
#include <utility>

using namespace llvm;

void FileCheckNumericVariable::setValue(uint64_t NewValue) {
  assert(!Value && "Overwriting numeric variable's value is not allowed");
  Value = NewValue;
}

void FileCheckNumericVariable::clearValue() {
  if (!Value)
    return;
  Value = None;
}

Expected<uint64_t> FileCheckNumericVariableUse::eval() const {
  Optional<uint64_t> Value = NumericVariable->getValue();
  if (Value)
    return *Value;
  return make_error<FileCheckUndefVarError>(Name);
}

Expected<uint64_t> FileCheckASTBinop::eval() const {
  Expected<uint64_t> LeftOp = LeftOperand->eval();
  Expected<uint64_t> RightOp = RightOperand->eval();

  // Bubble up any error (e.g. undefined variables) in the recursive
  // evaluation.
  if (!LeftOp || !RightOp) {
    Error Err = Error::success();
    if (!LeftOp)
      Err = joinErrors(std::move(Err), LeftOp.takeError());
    if (!RightOp)
      Err = joinErrors(std::move(Err), RightOp.takeError());
    return std::move(Err);
  }

  return EvalBinop(*LeftOp, *RightOp);
}

Expected<std::string> FileCheckNumericSubstitution::getResult() const {
  Expected<uint64_t> EvaluatedValue = ExpressionAST->eval();
  if (!EvaluatedValue)
    return EvaluatedValue.takeError();
  return utostr(*EvaluatedValue);
}

Expected<std::string> FileCheckStringSubstitution::getResult() const {
  // Look up the value and escape it so that we can put it into the regex.
  Expected<StringRef> VarVal = Context->getPatternVarValue(FromStr);
  if (!VarVal)
    return VarVal.takeError();
  return Regex::escape(*VarVal);
}

bool FileCheckPattern::isValidVarNameStart(char C) {
  return C == '_' || isalpha(C);
}

Expected<FileCheckPattern::VariableProperties>
FileCheckPattern::parseVariable(StringRef &Str, const SourceMgr &SM) {
  if (Str.empty())
    return FileCheckErrorDiagnostic::get(SM, Str, "empty variable name");

  bool ParsedOneChar = false;
  unsigned I = 0;
  bool IsPseudo = Str[0] == '@';

  // Global vars start with '$'.
  if (Str[0] == '$' || IsPseudo)
    ++I;

  for (unsigned E = Str.size(); I != E; ++I) {
    if (!ParsedOneChar && !isValidVarNameStart(Str[I]))
      return FileCheckErrorDiagnostic::get(SM, Str, "invalid variable name");

    // Variable names are composed of alphanumeric characters and underscores.
    if (Str[I] != '_' && !isalnum(Str[I]))
      break;
    ParsedOneChar = true;
  }

  StringRef Name = Str.take_front(I);
  Str = Str.substr(I);
  return VariableProperties {Name, IsPseudo};
}

// StringRef holding all characters considered as horizontal whitespaces by
// FileCheck input canonicalization.
StringRef SpaceChars = " \t";

// Parsing helper function that strips the first character in S and returns it.
static char popFront(StringRef &S) {
  char C = S.front();
  S = S.drop_front();
  return C;
}

char FileCheckUndefVarError::ID = 0;
char FileCheckErrorDiagnostic::ID = 0;
char FileCheckNotFoundError::ID = 0;

Expected<FileCheckNumericVariable *>
FileCheckPattern::parseNumericVariableDefinition(
    StringRef &Expr, FileCheckPatternContext *Context, size_t LineNumber,
    const SourceMgr &SM) {
  Expected<VariableProperties> ParseVarResult = parseVariable(Expr, SM);
  if (!ParseVarResult)
    return ParseVarResult.takeError();
  StringRef Name = ParseVarResult->Name;

  if (ParseVarResult->IsPseudo)
    return FileCheckErrorDiagnostic::get(
        SM, Name, "definition of pseudo numeric variable unsupported");

  // Detect collisions between string and numeric variables when the latter
  // is created later than the former.
  if (Context->DefinedVariableTable.find(Name) !=
      Context->DefinedVariableTable.end())
    return FileCheckErrorDiagnostic::get(
        SM, Name, "string variable with name '" + Name + "' already exists");

  Expr = Expr.ltrim(SpaceChars);
  if (!Expr.empty())
    return FileCheckErrorDiagnostic::get(
        SM, Expr, "unexpected characters after numeric variable name");

  FileCheckNumericVariable *DefinedNumericVariable;
  auto VarTableIter = Context->GlobalNumericVariableTable.find(Name);
  if (VarTableIter != Context->GlobalNumericVariableTable.end())
    DefinedNumericVariable = VarTableIter->second;
  else
    DefinedNumericVariable = Context->makeNumericVariable(LineNumber, Name);

  return DefinedNumericVariable;
}

Expected<std::unique_ptr<FileCheckNumericVariableUse>>
FileCheckPattern::parseNumericVariableUse(StringRef Name, bool IsPseudo,
                                          const SourceMgr &SM) const {
  if (IsPseudo && !Name.equals("@LINE"))
    return FileCheckErrorDiagnostic::get(
        SM, Name, "invalid pseudo numeric variable '" + Name + "'");

  // Numeric variable definitions and uses are parsed in the order in which
  // they appear in the CHECK patterns. For each definition, the pointer to the
  // class instance of the corresponding numeric variable definition is stored
  // in GlobalNumericVariableTable in parsePattern. Therefore, if the pointer
  // we get below is null, it means no such variable was defined before. When
  // that happens, we create a dummy variable so that parsing can continue. All
  // uses of undefined variables, whether string or numeric, are then diagnosed
  // in printSubstitutions() after failing to match.
  auto VarTableIter = Context->GlobalNumericVariableTable.find(Name);
  FileCheckNumericVariable *NumericVariable;
  if (VarTableIter != Context->GlobalNumericVariableTable.end())
    NumericVariable = VarTableIter->second;
  else {
    NumericVariable = Context->makeNumericVariable(0, Name);
    Context->GlobalNumericVariableTable[Name] = NumericVariable;
  }

  if (!IsPseudo && NumericVariable->getDefLineNumber() == LineNumber)
    return FileCheckErrorDiagnostic::get(
        SM, Name,
        "numeric variable '" + Name + "' defined on the same line as used");

  return llvm::make_unique<FileCheckNumericVariableUse>(Name, NumericVariable);
}

Expected<std::unique_ptr<FileCheckExpressionAST>>
FileCheckPattern::parseNumericOperand(StringRef &Expr, AllowedOperand AO,
                                      const SourceMgr &SM) const {
  if (AO == AllowedOperand::LineVar || AO == AllowedOperand::Any) {
    // Try to parse as a numeric variable use.
    Expected<FileCheckPattern::VariableProperties> ParseVarResult =
        parseVariable(Expr, SM);
    if (ParseVarResult)
      return parseNumericVariableUse(ParseVarResult->Name,
                                     ParseVarResult->IsPseudo, SM);
    if (AO == AllowedOperand::LineVar)
      return ParseVarResult.takeError();
    // Ignore the error and retry parsing as a literal.
    consumeError(ParseVarResult.takeError());
  }

  // Otherwise, parse it as a literal.
  uint64_t LiteralValue;
  if (!Expr.consumeInteger(/*Radix=*/10, LiteralValue))
    return llvm::make_unique<FileCheckExpressionLiteral>(LiteralValue);

  return FileCheckErrorDiagnostic::get(SM, Expr,
                                       "invalid operand format '" + Expr + "'");
}

static uint64_t add(uint64_t LeftOp, uint64_t RightOp) {
  return LeftOp + RightOp;
}

static uint64_t sub(uint64_t LeftOp, uint64_t RightOp) {
  return LeftOp - RightOp;
}

Expected<std::unique_ptr<FileCheckExpressionAST>>
FileCheckPattern::parseBinop(StringRef &Expr,
                             std::unique_ptr<FileCheckExpressionAST> LeftOp,
                             bool IsLegacyLineExpr, const SourceMgr &SM) const {
  Expr = Expr.ltrim(SpaceChars);
  if (Expr.empty())
    return std::move(LeftOp);

  // Check if this is a supported operation and select a function to perform
  // it.
  SMLoc OpLoc = SMLoc::getFromPointer(Expr.data());
  char Operator = popFront(Expr);
  binop_eval_t EvalBinop;
  switch (Operator) {
  case '+':
    EvalBinop = add;
    break;
  case '-':
    EvalBinop = sub;
    break;
  default:
    return FileCheckErrorDiagnostic::get(
        SM, OpLoc, Twine("unsupported operation '") + Twine(Operator) + "'");
  }

  // Parse right operand.
  Expr = Expr.ltrim(SpaceChars);
  if (Expr.empty())
    return FileCheckErrorDiagnostic::get(SM, Expr,
                                         "missing operand in expression");
  // The second operand in a legacy @LINE expression is always a literal.
  AllowedOperand AO =
      IsLegacyLineExpr ? AllowedOperand::Literal : AllowedOperand::Any;
  Expected<std::unique_ptr<FileCheckExpressionAST>> RightOpResult =
      parseNumericOperand(Expr, AO, SM);
  if (!RightOpResult)
    return RightOpResult;

  Expr = Expr.ltrim(SpaceChars);
  return llvm::make_unique<FileCheckASTBinop>(EvalBinop, std::move(LeftOp),
                                              std::move(*RightOpResult));
}

Expected<std::unique_ptr<FileCheckExpressionAST>>
FileCheckPattern::parseNumericSubstitutionBlock(
    StringRef Expr,
    Optional<FileCheckNumericVariable *> &DefinedNumericVariable,
    bool IsLegacyLineExpr, const SourceMgr &SM) const {
  // Parse the numeric variable definition.
  DefinedNumericVariable = None;
  size_t DefEnd = Expr.find(':');
  if (DefEnd != StringRef::npos) {
    StringRef DefExpr = Expr.substr(0, DefEnd);
    StringRef UseExpr = Expr.substr(DefEnd + 1);

    UseExpr = UseExpr.ltrim(SpaceChars);
    if (!UseExpr.empty())
      return FileCheckErrorDiagnostic::get(
          SM, UseExpr,
          "unexpected string after variable definition: '" + UseExpr + "'");

    DefExpr = DefExpr.ltrim(SpaceChars);
    Expected<FileCheckNumericVariable *> ParseResult =
        parseNumericVariableDefinition(DefExpr, Context, LineNumber, SM);
    if (!ParseResult)
      return ParseResult.takeError();
    DefinedNumericVariable = *ParseResult;

    return nullptr;
  }

  // Parse the expression itself.
  Expr = Expr.ltrim(SpaceChars);
  // The first operand in a legacy @LINE expression is always the @LINE pseudo
  // variable.
  AllowedOperand AO =
      IsLegacyLineExpr ? AllowedOperand::LineVar : AllowedOperand::Any;
  Expected<std::unique_ptr<FileCheckExpressionAST>> ParseResult =
      parseNumericOperand(Expr, AO, SM);
  while (ParseResult && !Expr.empty()) {
    ParseResult =
        parseBinop(Expr, std::move(*ParseResult), IsLegacyLineExpr, SM);
    // Legacy @LINE expressions only allow 2 operands.
    if (ParseResult && IsLegacyLineExpr && !Expr.empty())
      return FileCheckErrorDiagnostic::get(
          SM, Expr,
          "unexpected characters at end of expression '" + Expr + "'");
  }
  if (!ParseResult)
    return ParseResult;
  return std::move(*ParseResult);
}

bool FileCheckPattern::parsePattern(StringRef PatternStr, StringRef Prefix,
                                    SourceMgr &SM,
                                    const FileCheckRequest &Req) {
  bool MatchFullLinesHere = Req.MatchFullLines && CheckTy != Check::CheckNot;

  PatternLoc = SMLoc::getFromPointer(PatternStr.data());

  if (!(Req.NoCanonicalizeWhiteSpace && Req.MatchFullLines))
    // Ignore trailing whitespace.
    while (!PatternStr.empty() &&
           (PatternStr.back() == ' ' || PatternStr.back() == '\t'))
      PatternStr = PatternStr.substr(0, PatternStr.size() - 1);

  // Check that there is something on the line.
  if (PatternStr.empty() && CheckTy != Check::CheckEmpty) {
    SM.PrintMessage(PatternLoc, SourceMgr::DK_Error,
                    "found empty check string with prefix '" + Prefix + ":'");
    return true;
  }

  if (!PatternStr.empty() && CheckTy == Check::CheckEmpty) {
    SM.PrintMessage(
        PatternLoc, SourceMgr::DK_Error,
        "found non-empty check string for empty check with prefix '" + Prefix +
            ":'");
    return true;
  }

  if (CheckTy == Check::CheckEmpty) {
    RegExStr = "(\n$)";
    return false;
  }

  // Check to see if this is a fixed string, or if it has regex pieces.
  if (!MatchFullLinesHere &&
      (PatternStr.size() < 2 || (PatternStr.find("{{") == StringRef::npos &&
                                 PatternStr.find("[[") == StringRef::npos))) {
    FixedStr = PatternStr;
    return false;
  }

  if (MatchFullLinesHere) {
    RegExStr += '^';
    if (!Req.NoCanonicalizeWhiteSpace)
      RegExStr += " *";
  }

  // Paren value #0 is for the fully matched string.  Any new parenthesized
  // values add from there.
  unsigned CurParen = 1;

  // Otherwise, there is at least one regex piece.  Build up the regex pattern
  // by escaping scary characters in fixed strings, building up one big regex.
  while (!PatternStr.empty()) {
    // RegEx matches.
    if (PatternStr.startswith("{{")) {
      // This is the start of a regex match.  Scan for the }}.
      size_t End = PatternStr.find("}}");
      if (End == StringRef::npos) {
        SM.PrintMessage(SMLoc::getFromPointer(PatternStr.data()),
                        SourceMgr::DK_Error,
                        "found start of regex string with no end '}}'");
        return true;
      }

      // Enclose {{}} patterns in parens just like [[]] even though we're not
      // capturing the result for any purpose.  This is required in case the
      // expression contains an alternation like: CHECK:  abc{{x|z}}def.  We
      // want this to turn into: "abc(x|z)def" not "abcx|zdef".
      RegExStr += '(';
      ++CurParen;

      if (AddRegExToRegEx(PatternStr.substr(2, End - 2), CurParen, SM))
        return true;
      RegExStr += ')';

      PatternStr = PatternStr.substr(End + 2);
      continue;
    }

    // String and numeric substitution blocks. String substitution blocks come
    // in two forms: [[foo:.*]] and [[foo]]. The former matches .* (or some
    // other regex) and assigns it to the string variable 'foo'. The latter
    // substitutes foo's value. Numeric substitution blocks work the same way
    // as string ones, but start with a '#' sign after the double brackets.
    // Both string and numeric variable names must satisfy the regular
    // expression "[a-zA-Z_][0-9a-zA-Z_]*" to be valid, as this helps catch
    // some common errors.
    if (PatternStr.startswith("[[")) {
      StringRef UnparsedPatternStr = PatternStr.substr(2);
      // Find the closing bracket pair ending the match.  End is going to be an
      // offset relative to the beginning of the match string.
      size_t End = FindRegexVarEnd(UnparsedPatternStr, SM);
      StringRef MatchStr = UnparsedPatternStr.substr(0, End);
      bool IsNumBlock = MatchStr.consume_front("#");

      if (End == StringRef::npos) {
        SM.PrintMessage(SMLoc::getFromPointer(PatternStr.data()),
                        SourceMgr::DK_Error,
                        "Invalid substitution block, no ]] found");
        return true;
      }
      // Strip the substitution block we are parsing. End points to the start
      // of the "]]" closing the expression so account for it in computing the
      // index of the first unparsed character.
      PatternStr = UnparsedPatternStr.substr(End + 2);

      bool IsDefinition = false;
      // Whether the substitution block is a legacy use of @LINE with string
      // substitution block syntax.
      bool IsLegacyLineExpr = false;
      StringRef DefName;
      StringRef SubstStr;
      StringRef MatchRegexp;
      size_t SubstInsertIdx = RegExStr.size();

      // Parse string variable or legacy @LINE expression.
      if (!IsNumBlock) {
        size_t VarEndIdx = MatchStr.find(":");
        size_t SpacePos = MatchStr.substr(0, VarEndIdx).find_first_of(" \t");
        if (SpacePos != StringRef::npos) {
          SM.PrintMessage(SMLoc::getFromPointer(MatchStr.data() + SpacePos),
                          SourceMgr::DK_Error, "unexpected whitespace");
          return true;
        }

        // Get the name (e.g. "foo") and verify it is well formed.
        StringRef OrigMatchStr = MatchStr;
        Expected<FileCheckPattern::VariableProperties> ParseVarResult =
            parseVariable(MatchStr, SM);
        if (!ParseVarResult) {
          logAllUnhandledErrors(ParseVarResult.takeError(), errs());
          return true;
        }
        StringRef Name = ParseVarResult->Name;
        bool IsPseudo = ParseVarResult->IsPseudo;

        IsDefinition = (VarEndIdx != StringRef::npos);
        if (IsDefinition) {
          if ((IsPseudo || !MatchStr.consume_front(":"))) {
            SM.PrintMessage(SMLoc::getFromPointer(Name.data()),
                            SourceMgr::DK_Error,
                            "invalid name in string variable definition");
            return true;
          }

          // Detect collisions between string and numeric variables when the
          // former is created later than the latter.
          if (Context->GlobalNumericVariableTable.find(Name) !=
              Context->GlobalNumericVariableTable.end()) {
            SM.PrintMessage(
                SMLoc::getFromPointer(Name.data()), SourceMgr::DK_Error,
                "numeric variable with name '" + Name + "' already exists");
            return true;
          }
          DefName = Name;
          MatchRegexp = MatchStr;
        } else {
          if (IsPseudo) {
            MatchStr = OrigMatchStr;
            IsLegacyLineExpr = IsNumBlock = true;
          } else
            SubstStr = Name;
        }
      }

      // Parse numeric substitution block.
      std::unique_ptr<FileCheckExpressionAST> ExpressionAST;
      Optional<FileCheckNumericVariable *> DefinedNumericVariable;
      if (IsNumBlock) {
        Expected<std::unique_ptr<FileCheckExpressionAST>> ParseResult =
            parseNumericSubstitutionBlock(MatchStr, DefinedNumericVariable,
                                          IsLegacyLineExpr, SM);
        if (!ParseResult) {
          logAllUnhandledErrors(ParseResult.takeError(), errs());
          return true;
        }
        ExpressionAST = std::move(*ParseResult);
        if (DefinedNumericVariable) {
          IsDefinition = true;
          DefName = (*DefinedNumericVariable)->getName();
          MatchRegexp = StringRef("[0-9]+");
        } else
          SubstStr = MatchStr;
      }

      // Handle substitutions: [[foo]] and [[#<foo expr>]].
      if (!IsDefinition) {
        // Handle substitution of string variables that were defined earlier on
        // the same line by emitting a backreference. Expressions do not
        // support substituting a numeric variable defined on the same line.
        if (!IsNumBlock && VariableDefs.find(SubstStr) != VariableDefs.end()) {
          unsigned CaptureParenGroup = VariableDefs[SubstStr];
          if (CaptureParenGroup < 1 || CaptureParenGroup > 9) {
            SM.PrintMessage(SMLoc::getFromPointer(SubstStr.data()),
                            SourceMgr::DK_Error,
                            "Can't back-reference more than 9 variables");
            return true;
          }
          AddBackrefToRegEx(CaptureParenGroup);
        } else {
          // Handle substitution of string variables ([[<var>]]) defined in
          // previous CHECK patterns, and substitution of expressions.
          FileCheckSubstitution *Substitution =
              IsNumBlock
                  ? Context->makeNumericSubstitution(
                        SubstStr, std::move(ExpressionAST), SubstInsertIdx)
                  : Context->makeStringSubstitution(SubstStr, SubstInsertIdx);
          Substitutions.push_back(Substitution);
        }
        continue;
      }

      // Handle variable definitions: [[<def>:(...)]] and
      // [[#(...)<def>:(...)]].
      if (IsNumBlock) {
        FileCheckNumericVariableMatch NumericVariableDefinition = {
            *DefinedNumericVariable, CurParen};
        NumericVariableDefs[DefName] = NumericVariableDefinition;
        // This store is done here rather than in match() to allow
        // parseNumericVariableUse() to get the pointer to the class instance
        // of the right variable definition corresponding to a given numeric
        // variable use.
        Context->GlobalNumericVariableTable[DefName] = *DefinedNumericVariable;
      } else {
        VariableDefs[DefName] = CurParen;
        // Mark the string variable as defined to detect collisions between
        // string and numeric variables in parseNumericVariableUse() and
        // DefineCmdlineVariables() when the latter is created later than the
        // former. We cannot reuse GlobalVariableTable for this by populating
        // it with an empty string since we would then lose the ability to
        // detect the use of an undefined variable in match().
        Context->DefinedVariableTable[DefName] = true;
      }
      RegExStr += '(';
      ++CurParen;

      if (AddRegExToRegEx(MatchRegexp, CurParen, SM))
        return true;

      RegExStr += ')';
    }

    // Handle fixed string matches.
    // Find the end, which is the start of the next regex.
    size_t FixedMatchEnd = PatternStr.find("{{");
    FixedMatchEnd = std::min(FixedMatchEnd, PatternStr.find("[["));
    RegExStr += Regex::escape(PatternStr.substr(0, FixedMatchEnd));
    PatternStr = PatternStr.substr(FixedMatchEnd);
  }

  if (MatchFullLinesHere) {
    if (!Req.NoCanonicalizeWhiteSpace)
      RegExStr += " *";
    RegExStr += '$';
  }

  return false;
}

bool FileCheckPattern::AddRegExToRegEx(StringRef RS, unsigned &CurParen, SourceMgr &SM) {
  Regex R(RS);
  std::string Error;
  if (!R.isValid(Error)) {
    SM.PrintMessage(SMLoc::getFromPointer(RS.data()), SourceMgr::DK_Error,
                    "invalid regex: " + Error);
    return true;
  }

  RegExStr += RS.str();
  CurParen += R.getNumMatches();
  return false;
}

void FileCheckPattern::AddBackrefToRegEx(unsigned BackrefNum) {
  assert(BackrefNum >= 1 && BackrefNum <= 9 && "Invalid backref number");
  std::string Backref = std::string("\\") + std::string(1, '0' + BackrefNum);
  RegExStr += Backref;
}

Expected<size_t> FileCheckPattern::match(StringRef Buffer, size_t &MatchLen,
                                         const SourceMgr &SM) const {
  // If this is the EOF pattern, match it immediately.
  if (CheckTy == Check::CheckEOF) {
    MatchLen = 0;
    return Buffer.size();
  }

  // If this is a fixed string pattern, just match it now.
  if (!FixedStr.empty()) {
    MatchLen = FixedStr.size();
    size_t Pos = Buffer.find(FixedStr);
    if (Pos == StringRef::npos)
      return make_error<FileCheckNotFoundError>();
    return Pos;
  }

  // Regex match.

  // If there are substitutions, we need to create a temporary string with the
  // actual value.
  StringRef RegExToMatch = RegExStr;
  std::string TmpStr;
  if (!Substitutions.empty()) {
    TmpStr = RegExStr;
    Context->LineVariable->setValue(LineNumber);

    size_t InsertOffset = 0;
    // Substitute all string variables and expressions whose values are only
    // now known. Use of string variables defined on the same line are handled
    // by back-references.
    for (const auto &Substitution : Substitutions) {
      // Substitute and check for failure (e.g. use of undefined variable).
      Expected<std::string> Value = Substitution->getResult();
      if (!Value) {
        Context->LineVariable->clearValue();
        return Value.takeError();
      }

      // Plop it into the regex at the adjusted offset.
      TmpStr.insert(TmpStr.begin() + Substitution->getIndex() + InsertOffset,
                    Value->begin(), Value->end());
      InsertOffset += Value->size();
    }

    // Match the newly constructed regex.
    RegExToMatch = TmpStr;
    Context->LineVariable->clearValue();
  }

  SmallVector<StringRef, 4> MatchInfo;
  if (!Regex(RegExToMatch, Regex::Newline).match(Buffer, &MatchInfo))
    return make_error<FileCheckNotFoundError>();

  // Successful regex match.
  assert(!MatchInfo.empty() && "Didn't get any match");
  StringRef FullMatch = MatchInfo[0];

  // If this defines any string variables, remember their values.
  for (const auto &VariableDef : VariableDefs) {
    assert(VariableDef.second < MatchInfo.size() && "Internal paren error");
    Context->GlobalVariableTable[VariableDef.first] =
        MatchInfo[VariableDef.second];
  }

  // If this defines any numeric variables, remember their values.
  for (const auto &NumericVariableDef : NumericVariableDefs) {
    const FileCheckNumericVariableMatch &NumericVariableMatch =
        NumericVariableDef.getValue();
    unsigned CaptureParenGroup = NumericVariableMatch.CaptureParenGroup;
    assert(CaptureParenGroup < MatchInfo.size() && "Internal paren error");
    FileCheckNumericVariable *DefinedNumericVariable =
        NumericVariableMatch.DefinedNumericVariable;

    StringRef MatchedValue = MatchInfo[CaptureParenGroup];
    uint64_t Val;
    if (MatchedValue.getAsInteger(10, Val))
      return FileCheckErrorDiagnostic::get(SM, MatchedValue,
                                           "Unable to represent numeric value");
    DefinedNumericVariable->setValue(Val);
  }

  // Like CHECK-NEXT, CHECK-EMPTY's match range is considered to start after
  // the required preceding newline, which is consumed by the pattern in the
  // case of CHECK-EMPTY but not CHECK-NEXT.
  size_t MatchStartSkip = CheckTy == Check::CheckEmpty;
  MatchLen = FullMatch.size() - MatchStartSkip;
  return FullMatch.data() - Buffer.data() + MatchStartSkip;
}

unsigned FileCheckPattern::computeMatchDistance(StringRef Buffer) const {
  // Just compute the number of matching characters. For regular expressions, we
  // just compare against the regex itself and hope for the best.
  //
  // FIXME: One easy improvement here is have the regex lib generate a single
  // example regular expression which matches, and use that as the example
  // string.
  StringRef ExampleString(FixedStr);
  if (ExampleString.empty())
    ExampleString = RegExStr;

  // Only compare up to the first line in the buffer, or the string size.
  StringRef BufferPrefix = Buffer.substr(0, ExampleString.size());
  BufferPrefix = BufferPrefix.split('\n').first;
  return BufferPrefix.edit_distance(ExampleString);
}

void FileCheckPattern::printSubstitutions(const SourceMgr &SM, StringRef Buffer,
                                          SMRange MatchRange) const {
  // Print what we know about substitutions.
  if (!Substitutions.empty()) {
    for (const auto &Substitution : Substitutions) {
      SmallString<256> Msg;
      raw_svector_ostream OS(Msg);
      Expected<std::string> MatchedValue = Substitution->getResult();

      // Substitution failed or is not known at match time, print the undefined
      // variables it uses.
      if (!MatchedValue) {
        bool UndefSeen = false;
        handleAllErrors(MatchedValue.takeError(),
                        [](const FileCheckNotFoundError &E) {},
                        // Handled in PrintNoMatch().
                        [](const FileCheckErrorDiagnostic &E) {},
                        [&](const FileCheckUndefVarError &E) {
                          if (!UndefSeen) {
                            OS << "uses undefined variable(s):";
                            UndefSeen = true;
                          }
                          OS << " ";
                          E.log(OS);
                        });
      } else {
        // Substitution succeeded. Print substituted value.
        OS << "with \"";
        OS.write_escaped(Substitution->getFromString()) << "\" equal to \"";
        OS.write_escaped(*MatchedValue) << "\"";
      }

      if (MatchRange.isValid())
        SM.PrintMessage(MatchRange.Start, SourceMgr::DK_Note, OS.str(),
                        {MatchRange});
      else
        SM.PrintMessage(SMLoc::getFromPointer(Buffer.data()),
                        SourceMgr::DK_Note, OS.str());
    }
  }
}

static SMRange ProcessMatchResult(FileCheckDiag::MatchType MatchTy,
                                  const SourceMgr &SM, SMLoc Loc,
                                  Check::FileCheckType CheckTy,
                                  StringRef Buffer, size_t Pos, size_t Len,
                                  std::vector<FileCheckDiag> *Diags,
                                  bool AdjustPrevDiag = false) {
  SMLoc Start = SMLoc::getFromPointer(Buffer.data() + Pos);
  SMLoc End = SMLoc::getFromPointer(Buffer.data() + Pos + Len);
  SMRange Range(Start, End);
  if (Diags) {
    if (AdjustPrevDiag)
      Diags->rbegin()->MatchTy = MatchTy;
    else
      Diags->emplace_back(SM, CheckTy, Loc, MatchTy, Range);
  }
  return Range;
}

void FileCheckPattern::printFuzzyMatch(
    const SourceMgr &SM, StringRef Buffer,
    std::vector<FileCheckDiag> *Diags) const {
  // Attempt to find the closest/best fuzzy match.  Usually an error happens
  // because some string in the output didn't exactly match. In these cases, we
  // would like to show the user a best guess at what "should have" matched, to
  // save them having to actually check the input manually.
  size_t NumLinesForward = 0;
  size_t Best = StringRef::npos;
  double BestQuality = 0;

  // Use an arbitrary 4k limit on how far we will search.
  for (size_t i = 0, e = std::min(size_t(4096), Buffer.size()); i != e; ++i) {
    if (Buffer[i] == '\n')
      ++NumLinesForward;

    // Patterns have leading whitespace stripped, so skip whitespace when
    // looking for something which looks like a pattern.
    if (Buffer[i] == ' ' || Buffer[i] == '\t')
      continue;

    // Compute the "quality" of this match as an arbitrary combination of the
    // match distance and the number of lines skipped to get to this match.
    unsigned Distance = computeMatchDistance(Buffer.substr(i));
    double Quality = Distance + (NumLinesForward / 100.);

    if (Quality < BestQuality || Best == StringRef::npos) {
      Best = i;
      BestQuality = Quality;
    }
  }

  // Print the "possible intended match here" line if we found something
  // reasonable and not equal to what we showed in the "scanning from here"
  // line.
  if (Best && Best != StringRef::npos && BestQuality < 50) {
    SMRange MatchRange =
        ProcessMatchResult(FileCheckDiag::MatchFuzzy, SM, getLoc(),
                           getCheckTy(), Buffer, Best, 0, Diags);
    SM.PrintMessage(MatchRange.Start, SourceMgr::DK_Note,
                    "possible intended match here");

    // FIXME: If we wanted to be really friendly we would show why the match
    // failed, as it can be hard to spot simple one character differences.
  }
}

Expected<StringRef>
FileCheckPatternContext::getPatternVarValue(StringRef VarName) {
  auto VarIter = GlobalVariableTable.find(VarName);
  if (VarIter == GlobalVariableTable.end())
    return make_error<FileCheckUndefVarError>(VarName);

  return VarIter->second;
}

template <class... Types>
FileCheckNumericVariable *
FileCheckPatternContext::makeNumericVariable(Types... args) {
  NumericVariables.push_back(
      llvm::make_unique<FileCheckNumericVariable>(args...));
  return NumericVariables.back().get();
}

FileCheckSubstitution *
FileCheckPatternContext::makeStringSubstitution(StringRef VarName,
                                                size_t InsertIdx) {
  Substitutions.push_back(
      llvm::make_unique<FileCheckStringSubstitution>(this, VarName, InsertIdx));
  return Substitutions.back().get();
}

FileCheckSubstitution *FileCheckPatternContext::makeNumericSubstitution(
    StringRef ExpressionStr,
    std::unique_ptr<FileCheckExpressionAST> ExpressionAST, size_t InsertIdx) {
  Substitutions.push_back(llvm::make_unique<FileCheckNumericSubstitution>(
      this, ExpressionStr, std::move(ExpressionAST), InsertIdx));
  return Substitutions.back().get();
}

size_t FileCheckPattern::FindRegexVarEnd(StringRef Str, SourceMgr &SM) {
  // Offset keeps track of the current offset within the input Str
  size_t Offset = 0;
  // [...] Nesting depth
  size_t BracketDepth = 0;

  while (!Str.empty()) {
    if (Str.startswith("]]") && BracketDepth == 0)
      return Offset;
    if (Str[0] == '\\') {
      // Backslash escapes the next char within regexes, so skip them both.
      Str = Str.substr(2);
      Offset += 2;
    } else {
      switch (Str[0]) {
      default:
        break;
      case '[':
        BracketDepth++;
        break;
      case ']':
        if (BracketDepth == 0) {
          SM.PrintMessage(SMLoc::getFromPointer(Str.data()),
                          SourceMgr::DK_Error,
                          "missing closing \"]\" for regex variable");
          exit(1);
        }
        BracketDepth--;
        break;
      }
      Str = Str.substr(1);
      Offset++;
    }
  }

  return StringRef::npos;
}

StringRef FileCheck::CanonicalizeFile(MemoryBuffer &MB,
                                      SmallVectorImpl<char> &OutputBuffer) {
  OutputBuffer.reserve(MB.getBufferSize());

  for (const char *Ptr = MB.getBufferStart(), *End = MB.getBufferEnd();
       Ptr != End; ++Ptr) {
    // Eliminate trailing dosish \r.
    if (Ptr <= End - 2 && Ptr[0] == '\r' && Ptr[1] == '\n') {
      continue;
    }

    // If current char is not a horizontal whitespace or if horizontal
    // whitespace canonicalization is disabled, dump it to output as is.
    if (Req.NoCanonicalizeWhiteSpace || (*Ptr != ' ' && *Ptr != '\t')) {
      OutputBuffer.push_back(*Ptr);
      continue;
    }

    // Otherwise, add one space and advance over neighboring space.
    OutputBuffer.push_back(' ');
    while (Ptr + 1 != End && (Ptr[1] == ' ' || Ptr[1] == '\t'))
      ++Ptr;
  }

  // Add a null byte and then return all but that byte.
  OutputBuffer.push_back('\0');
  return StringRef(OutputBuffer.data(), OutputBuffer.size() - 1);
}

FileCheckDiag::FileCheckDiag(const SourceMgr &SM,
                             const Check::FileCheckType &CheckTy,
                             SMLoc CheckLoc, MatchType MatchTy,
                             SMRange InputRange)
    : CheckTy(CheckTy), MatchTy(MatchTy) {
  auto Start = SM.getLineAndColumn(InputRange.Start);
  auto End = SM.getLineAndColumn(InputRange.End);
  InputStartLine = Start.first;
  InputStartCol = Start.second;
  InputEndLine = End.first;
  InputEndCol = End.second;
  Start = SM.getLineAndColumn(CheckLoc);
  CheckLine = Start.first;
  CheckCol = Start.second;
}

static bool IsPartOfWord(char c) {
  return (isalnum(c) || c == '-' || c == '_');
}

Check::FileCheckType &Check::FileCheckType::setCount(int C) {
  assert(Count > 0 && "zero and negative counts are not supported");
  assert((C == 1 || Kind == CheckPlain) &&
         "count supported only for plain CHECK directives");
  Count = C;
  return *this;
}

std::string Check::FileCheckType::getDescription(StringRef Prefix) const {
  switch (Kind) {
  case Check::CheckNone:
    return "invalid";
  case Check::CheckPlain:
    if (Count > 1)
      return Prefix.str() + "-COUNT";
    return Prefix;
  case Check::CheckNext:
    return Prefix.str() + "-NEXT";
  case Check::CheckSame:
    return Prefix.str() + "-SAME";
  case Check::CheckNot:
    return Prefix.str() + "-NOT";
  case Check::CheckDAG:
    return Prefix.str() + "-DAG";
  case Check::CheckLabel:
    return Prefix.str() + "-LABEL";
  case Check::CheckEmpty:
    return Prefix.str() + "-EMPTY";
  case Check::CheckEOF:
    return "implicit EOF";
  case Check::CheckBadNot:
    return "bad NOT";
  case Check::CheckBadCount:
    return "bad COUNT";
  }
  llvm_unreachable("unknown FileCheckType");
}

static std::pair<Check::FileCheckType, StringRef>
FindCheckType(StringRef Buffer, StringRef Prefix) {
  if (Buffer.size() <= Prefix.size())
    return {Check::CheckNone, StringRef()};

  char NextChar = Buffer[Prefix.size()];

  StringRef Rest = Buffer.drop_front(Prefix.size() + 1);
  // Verify that the : is present after the prefix.
  if (NextChar == ':')
    return {Check::CheckPlain, Rest};

  if (NextChar != '-')
    return {Check::CheckNone, StringRef()};

  if (Rest.consume_front("COUNT-")) {
    int64_t Count;
    if (Rest.consumeInteger(10, Count))
      // Error happened in parsing integer.
      return {Check::CheckBadCount, Rest};
    if (Count <= 0 || Count > INT32_MAX)
      return {Check::CheckBadCount, Rest};
    if (!Rest.consume_front(":"))
      return {Check::CheckBadCount, Rest};
    return {Check::FileCheckType(Check::CheckPlain).setCount(Count), Rest};
  }

  if (Rest.consume_front("NEXT:"))
    return {Check::CheckNext, Rest};

  if (Rest.consume_front("SAME:"))
    return {Check::CheckSame, Rest};

  if (Rest.consume_front("NOT:"))
    return {Check::CheckNot, Rest};

  if (Rest.consume_front("DAG:"))
    return {Check::CheckDAG, Rest};

  if (Rest.consume_front("LABEL:"))
    return {Check::CheckLabel, Rest};

  if (Rest.consume_front("EMPTY:"))
    return {Check::CheckEmpty, Rest};

  // You can't combine -NOT with another suffix.
  if (Rest.startswith("DAG-NOT:") || Rest.startswith("NOT-DAG:") ||
      Rest.startswith("NEXT-NOT:") || Rest.startswith("NOT-NEXT:") ||
      Rest.startswith("SAME-NOT:") || Rest.startswith("NOT-SAME:") ||
      Rest.startswith("EMPTY-NOT:") || Rest.startswith("NOT-EMPTY:"))
    return {Check::CheckBadNot, Rest};

  return {Check::CheckNone, Rest};
}

// From the given position, find the next character after the word.
static size_t SkipWord(StringRef Str, size_t Loc) {
  while (Loc < Str.size() && IsPartOfWord(Str[Loc]))
    ++Loc;
  return Loc;
}

/// Searches the buffer for the first prefix in the prefix regular expression.
///
/// This searches the buffer using the provided regular expression, however it
/// enforces constraints beyond that:
/// 1) The found prefix must not be a suffix of something that looks like
///    a valid prefix.
/// 2) The found prefix must be followed by a valid check type suffix using \c
///    FindCheckType above.
///
/// \returns a pair of StringRefs into the Buffer, which combines:
///   - the first match of the regular expression to satisfy these two is
///   returned,
///     otherwise an empty StringRef is returned to indicate failure.
///   - buffer rewound to the location right after parsed suffix, for parsing
///     to continue from
///
/// If this routine returns a valid prefix, it will also shrink \p Buffer to
/// start at the beginning of the returned prefix, increment \p LineNumber for
/// each new line consumed from \p Buffer, and set \p CheckTy to the type of
/// check found by examining the suffix.
///
/// If no valid prefix is found, the state of Buffer, LineNumber, and CheckTy
/// is unspecified.
static std::pair<StringRef, StringRef>
FindFirstMatchingPrefix(Regex &PrefixRE, StringRef &Buffer,
                        unsigned &LineNumber, Check::FileCheckType &CheckTy) {
  SmallVector<StringRef, 2> Matches;

  while (!Buffer.empty()) {
    // Find the first (longest) match using the RE.
    if (!PrefixRE.match(Buffer, &Matches))
      // No match at all, bail.
      return {StringRef(), StringRef()};

    StringRef Prefix = Matches[0];
    Matches.clear();

    assert(Prefix.data() >= Buffer.data() &&
           Prefix.data() < Buffer.data() + Buffer.size() &&
           "Prefix doesn't start inside of buffer!");
    size_t Loc = Prefix.data() - Buffer.data();
    StringRef Skipped = Buffer.substr(0, Loc);
    Buffer = Buffer.drop_front(Loc);
    LineNumber += Skipped.count('\n');

    // Check that the matched prefix isn't a suffix of some other check-like
    // word.
    // FIXME: This is a very ad-hoc check. it would be better handled in some
    // other way. Among other things it seems hard to distinguish between
    // intentional and unintentional uses of this feature.
    if (Skipped.empty() || !IsPartOfWord(Skipped.back())) {
      // Now extract the type.
      StringRef AfterSuffix;
      std::tie(CheckTy, AfterSuffix) = FindCheckType(Buffer, Prefix);

      // If we've found a valid check type for this prefix, we're done.
      if (CheckTy != Check::CheckNone)
        return {Prefix, AfterSuffix};
    }

    // If we didn't successfully find a prefix, we need to skip this invalid
    // prefix and continue scanning. We directly skip the prefix that was
    // matched and any additional parts of that check-like word.
    Buffer = Buffer.drop_front(SkipWord(Buffer, Prefix.size()));
  }

  // We ran out of buffer while skipping partial matches so give up.
  return {StringRef(), StringRef()};
}

void FileCheckPatternContext::createLineVariable() {
  assert(!LineVariable && "@LINE pseudo numeric variable already created");
  StringRef LineName = "@LINE";
  LineVariable = makeNumericVariable(0, LineName);
  GlobalNumericVariableTable[LineName] = LineVariable;
}

bool FileCheck::ReadCheckFile(SourceMgr &SM, StringRef Buffer, Regex &PrefixRE,
                              std::vector<FileCheckString> &CheckStrings) {
  Error DefineError =
      PatternContext.defineCmdlineVariables(Req.GlobalDefines, SM);
  if (DefineError) {
    logAllUnhandledErrors(std::move(DefineError), errs());
    return true;
  }

  PatternContext.createLineVariable();

  std::vector<FileCheckPattern> ImplicitNegativeChecks;
  for (const auto &PatternString : Req.ImplicitCheckNot) {
    // Create a buffer with fake command line content in order to display the
    // command line option responsible for the specific implicit CHECK-NOT.
    std::string Prefix = "-implicit-check-not='";
    std::string Suffix = "'";
    std::unique_ptr<MemoryBuffer> CmdLine = MemoryBuffer::getMemBufferCopy(
        Prefix + PatternString + Suffix, "command line");

    StringRef PatternInBuffer =
        CmdLine->getBuffer().substr(Prefix.size(), PatternString.size());
    SM.AddNewSourceBuffer(std::move(CmdLine), SMLoc());

    ImplicitNegativeChecks.push_back(
        FileCheckPattern(Check::CheckNot, &PatternContext, 0));
    ImplicitNegativeChecks.back().parsePattern(PatternInBuffer,
                                               "IMPLICIT-CHECK", SM, Req);
  }

  std::vector<FileCheckPattern> DagNotMatches = ImplicitNegativeChecks;

  // LineNumber keeps track of the line on which CheckPrefix instances are
  // found.
  unsigned LineNumber = 1;

  while (1) {
    Check::FileCheckType CheckTy;

    // See if a prefix occurs in the memory buffer.
    StringRef UsedPrefix;
    StringRef AfterSuffix;
    std::tie(UsedPrefix, AfterSuffix) =
        FindFirstMatchingPrefix(PrefixRE, Buffer, LineNumber, CheckTy);
    if (UsedPrefix.empty())
      break;
    assert(UsedPrefix.data() == Buffer.data() &&
           "Failed to move Buffer's start forward, or pointed prefix outside "
           "of the buffer!");
    assert(AfterSuffix.data() >= Buffer.data() &&
           AfterSuffix.data() < Buffer.data() + Buffer.size() &&
           "Parsing after suffix doesn't start inside of buffer!");

    // Location to use for error messages.
    const char *UsedPrefixStart = UsedPrefix.data();

    // Skip the buffer to the end of parsed suffix (or just prefix, if no good
    // suffix was processed).
    Buffer = AfterSuffix.empty() ? Buffer.drop_front(UsedPrefix.size())
                                 : AfterSuffix;

    // Complain about useful-looking but unsupported suffixes.
    if (CheckTy == Check::CheckBadNot) {
      SM.PrintMessage(SMLoc::getFromPointer(Buffer.data()), SourceMgr::DK_Error,
                      "unsupported -NOT combo on prefix '" + UsedPrefix + "'");
      return true;
    }

    // Complain about invalid count specification.
    if (CheckTy == Check::CheckBadCount) {
      SM.PrintMessage(SMLoc::getFromPointer(Buffer.data()), SourceMgr::DK_Error,
                      "invalid count in -COUNT specification on prefix '" +
                          UsedPrefix + "'");
      return true;
    }

    // Okay, we found the prefix, yay. Remember the rest of the line, but ignore
    // leading whitespace.
    if (!(Req.NoCanonicalizeWhiteSpace && Req.MatchFullLines))
      Buffer = Buffer.substr(Buffer.find_first_not_of(" \t"));

    // Scan ahead to the end of line.
    size_t EOL = Buffer.find_first_of("\n\r");

    // Remember the location of the start of the pattern, for diagnostics.
    SMLoc PatternLoc = SMLoc::getFromPointer(Buffer.data());

    // Parse the pattern.
    FileCheckPattern P(CheckTy, &PatternContext, LineNumber);
    if (P.parsePattern(Buffer.substr(0, EOL), UsedPrefix, SM, Req))
      return true;

    // Verify that CHECK-LABEL lines do not define or use variables
    if ((CheckTy == Check::CheckLabel) && P.hasVariable()) {
      SM.PrintMessage(
          SMLoc::getFromPointer(UsedPrefixStart), SourceMgr::DK_Error,
          "found '" + UsedPrefix + "-LABEL:'"
                                   " with variable definition or use");
      return true;
    }

    Buffer = Buffer.substr(EOL);

    // Verify that CHECK-NEXT/SAME/EMPTY lines have at least one CHECK line before them.
    if ((CheckTy == Check::CheckNext || CheckTy == Check::CheckSame ||
         CheckTy == Check::CheckEmpty) &&
        CheckStrings.empty()) {
      StringRef Type = CheckTy == Check::CheckNext
                           ? "NEXT"
                           : CheckTy == Check::CheckEmpty ? "EMPTY" : "SAME";
      SM.PrintMessage(SMLoc::getFromPointer(UsedPrefixStart),
                      SourceMgr::DK_Error,
                      "found '" + UsedPrefix + "-" + Type +
                          "' without previous '" + UsedPrefix + ": line");
      return true;
    }

    // Handle CHECK-DAG/-NOT.
    if (CheckTy == Check::CheckDAG || CheckTy == Check::CheckNot) {
      DagNotMatches.push_back(P);
      continue;
    }

    // Okay, add the string we captured to the output vector and move on.
    CheckStrings.emplace_back(P, UsedPrefix, PatternLoc);
    std::swap(DagNotMatches, CheckStrings.back().DagNotStrings);
    DagNotMatches = ImplicitNegativeChecks;
  }

  // Add an EOF pattern for any trailing CHECK-DAG/-NOTs, and use the first
  // prefix as a filler for the error message.
  if (!DagNotMatches.empty()) {
    CheckStrings.emplace_back(
        FileCheckPattern(Check::CheckEOF, &PatternContext, LineNumber + 1),
        *Req.CheckPrefixes.begin(), SMLoc::getFromPointer(Buffer.data()));
    std::swap(DagNotMatches, CheckStrings.back().DagNotStrings);
  }

  if (CheckStrings.empty()) {
    errs() << "error: no check strings found with prefix"
           << (Req.CheckPrefixes.size() > 1 ? "es " : " ");
    auto I = Req.CheckPrefixes.begin();
    auto E = Req.CheckPrefixes.end();
    if (I != E) {
      errs() << "\'" << *I << ":'";
      ++I;
    }
    for (; I != E; ++I)
      errs() << ", \'" << *I << ":'";

    errs() << '\n';
    return true;
  }

  return false;
}

static void PrintMatch(bool ExpectedMatch, const SourceMgr &SM,
                       StringRef Prefix, SMLoc Loc, const FileCheckPattern &Pat,
                       int MatchedCount, StringRef Buffer, size_t MatchPos,
                       size_t MatchLen, const FileCheckRequest &Req,
                       std::vector<FileCheckDiag> *Diags) {
  bool PrintDiag = true;
  if (ExpectedMatch) {
    if (!Req.Verbose)
      return;
    if (!Req.VerboseVerbose && Pat.getCheckTy() == Check::CheckEOF)
      return;
    // Due to their verbosity, we don't print verbose diagnostics here if we're
    // gathering them for a different rendering, but we always print other
    // diagnostics.
    PrintDiag = !Diags;
  }
  SMRange MatchRange = ProcessMatchResult(
      ExpectedMatch ? FileCheckDiag::MatchFoundAndExpected
                    : FileCheckDiag::MatchFoundButExcluded,
      SM, Loc, Pat.getCheckTy(), Buffer, MatchPos, MatchLen, Diags);
  if (!PrintDiag)
    return;

  std::string Message = formatv("{0}: {1} string found in input",
                                Pat.getCheckTy().getDescription(Prefix),
                                (ExpectedMatch ? "expected" : "excluded"))
                            .str();
  if (Pat.getCount() > 1)
    Message += formatv(" ({0} out of {1})", MatchedCount, Pat.getCount()).str();

  SM.PrintMessage(
      Loc, ExpectedMatch ? SourceMgr::DK_Remark : SourceMgr::DK_Error, Message);
  SM.PrintMessage(MatchRange.Start, SourceMgr::DK_Note, "found here",
                  {MatchRange});
  Pat.printSubstitutions(SM, Buffer, MatchRange);
}

static void PrintMatch(bool ExpectedMatch, const SourceMgr &SM,
                       const FileCheckString &CheckStr, int MatchedCount,
                       StringRef Buffer, size_t MatchPos, size_t MatchLen,
                       FileCheckRequest &Req,
                       std::vector<FileCheckDiag> *Diags) {
  PrintMatch(ExpectedMatch, SM, CheckStr.Prefix, CheckStr.Loc, CheckStr.Pat,
             MatchedCount, Buffer, MatchPos, MatchLen, Req, Diags);
}

static void PrintNoMatch(bool ExpectedMatch, const SourceMgr &SM,
                         StringRef Prefix, SMLoc Loc,
                         const FileCheckPattern &Pat, int MatchedCount,
                         StringRef Buffer, bool VerboseVerbose,
                         std::vector<FileCheckDiag> *Diags, Error MatchErrors) {
  assert(MatchErrors && "Called on successful match");
  bool PrintDiag = true;
  if (!ExpectedMatch) {
    if (!VerboseVerbose) {
      consumeError(std::move(MatchErrors));
      return;
    }
    // Due to their verbosity, we don't print verbose diagnostics here if we're
    // gathering them for a different rendering, but we always print other
    // diagnostics.
    PrintDiag = !Diags;
  }

  // If the current position is at the end of a line, advance to the start of
  // the next line.
  Buffer = Buffer.substr(Buffer.find_first_not_of(" \t\n\r"));
  SMRange SearchRange = ProcessMatchResult(
      ExpectedMatch ? FileCheckDiag::MatchNoneButExpected
                    : FileCheckDiag::MatchNoneAndExcluded,
      SM, Loc, Pat.getCheckTy(), Buffer, 0, Buffer.size(), Diags);
  if (!PrintDiag) {
    consumeError(std::move(MatchErrors));
    return;
  }

  MatchErrors =
      handleErrors(std::move(MatchErrors),
                   [](const FileCheckErrorDiagnostic &E) { E.log(errs()); });

  // No problem matching the string per se.
  if (!MatchErrors)
    return;
  consumeError(std::move(MatchErrors));

  // Print "not found" diagnostic.
  std::string Message = formatv("{0}: {1} string not found in input",
                                Pat.getCheckTy().getDescription(Prefix),
                                (ExpectedMatch ? "expected" : "excluded"))
                            .str();
  if (Pat.getCount() > 1)
    Message += formatv(" ({0} out of {1})", MatchedCount, Pat.getCount()).str();
  SM.PrintMessage(
      Loc, ExpectedMatch ? SourceMgr::DK_Error : SourceMgr::DK_Remark, Message);

  // Print the "scanning from here" line.
  SM.PrintMessage(SearchRange.Start, SourceMgr::DK_Note, "scanning from here");

  // Allow the pattern to print additional information if desired.
  Pat.printSubstitutions(SM, Buffer);

  if (ExpectedMatch)
    Pat.printFuzzyMatch(SM, Buffer, Diags);
}

static void PrintNoMatch(bool ExpectedMatch, const SourceMgr &SM,
                         const FileCheckString &CheckStr, int MatchedCount,
                         StringRef Buffer, bool VerboseVerbose,
                         std::vector<FileCheckDiag> *Diags, Error MatchErrors) {
  PrintNoMatch(ExpectedMatch, SM, CheckStr.Prefix, CheckStr.Loc, CheckStr.Pat,
               MatchedCount, Buffer, VerboseVerbose, Diags,
               std::move(MatchErrors));
}

/// Counts the number of newlines in the specified range.
static unsigned CountNumNewlinesBetween(StringRef Range,
                                        const char *&FirstNewLine) {
  unsigned NumNewLines = 0;
  while (1) {
    // Scan for newline.
    Range = Range.substr(Range.find_first_of("\n\r"));
    if (Range.empty())
      return NumNewLines;

    ++NumNewLines;

    // Handle \n\r and \r\n as a single newline.
    if (Range.size() > 1 && (Range[1] == '\n' || Range[1] == '\r') &&
        (Range[0] != Range[1]))
      Range = Range.substr(1);
    Range = Range.substr(1);

    if (NumNewLines == 1)
      FirstNewLine = Range.begin();
  }
}

size_t FileCheckString::Check(const SourceMgr &SM, StringRef Buffer,
                              bool IsLabelScanMode, size_t &MatchLen,
                              FileCheckRequest &Req,
                              std::vector<FileCheckDiag> *Diags) const {
  size_t LastPos = 0;
  std::vector<const FileCheckPattern *> NotStrings;

  // IsLabelScanMode is true when we are scanning forward to find CHECK-LABEL
  // bounds; we have not processed variable definitions within the bounded block
  // yet so cannot handle any final CHECK-DAG yet; this is handled when going
  // over the block again (including the last CHECK-LABEL) in normal mode.
  if (!IsLabelScanMode) {
    // Match "dag strings" (with mixed "not strings" if any).
    LastPos = CheckDag(SM, Buffer, NotStrings, Req, Diags);
    if (LastPos == StringRef::npos)
      return StringRef::npos;
  }

  // Match itself from the last position after matching CHECK-DAG.
  size_t LastMatchEnd = LastPos;
  size_t FirstMatchPos = 0;
  // Go match the pattern Count times. Majority of patterns only match with
  // count 1 though.
  assert(Pat.getCount() != 0 && "pattern count can not be zero");
  for (int i = 1; i <= Pat.getCount(); i++) {
    StringRef MatchBuffer = Buffer.substr(LastMatchEnd);
    size_t CurrentMatchLen;
    // get a match at current start point
    Expected<size_t> MatchResult = Pat.match(MatchBuffer, CurrentMatchLen, SM);

    // report
    if (!MatchResult) {
      PrintNoMatch(true, SM, *this, i, MatchBuffer, Req.VerboseVerbose, Diags,
                   MatchResult.takeError());
      return StringRef::npos;
    }
    size_t MatchPos = *MatchResult;
    PrintMatch(true, SM, *this, i, MatchBuffer, MatchPos, CurrentMatchLen, Req,
               Diags);
    if (i == 1)
      FirstMatchPos = LastPos + MatchPos;

    // move start point after the match
    LastMatchEnd += MatchPos + CurrentMatchLen;
  }
  // Full match len counts from first match pos.
  MatchLen = LastMatchEnd - FirstMatchPos;

  // Similar to the above, in "label-scan mode" we can't yet handle CHECK-NEXT
  // or CHECK-NOT
  if (!IsLabelScanMode) {
    size_t MatchPos = FirstMatchPos - LastPos;
    StringRef MatchBuffer = Buffer.substr(LastPos);
    StringRef SkippedRegion = Buffer.substr(LastPos, MatchPos);

    // If this check is a "CHECK-NEXT", verify that the previous match was on
    // the previous line (i.e. that there is one newline between them).
    if (CheckNext(SM, SkippedRegion)) {
      ProcessMatchResult(FileCheckDiag::MatchFoundButWrongLine, SM, Loc,
                         Pat.getCheckTy(), MatchBuffer, MatchPos, MatchLen,
                         Diags, Req.Verbose);
      return StringRef::npos;
    }

    // If this check is a "CHECK-SAME", verify that the previous match was on
    // the same line (i.e. that there is no newline between them).
    if (CheckSame(SM, SkippedRegion)) {
      ProcessMatchResult(FileCheckDiag::MatchFoundButWrongLine, SM, Loc,
                         Pat.getCheckTy(), MatchBuffer, MatchPos, MatchLen,
                         Diags, Req.Verbose);
      return StringRef::npos;
    }

    // If this match had "not strings", verify that they don't exist in the
    // skipped region.
    if (CheckNot(SM, SkippedRegion, NotStrings, Req, Diags))
      return StringRef::npos;
  }

  return FirstMatchPos;
}

bool FileCheckString::CheckNext(const SourceMgr &SM, StringRef Buffer) const {
  if (Pat.getCheckTy() != Check::CheckNext &&
      Pat.getCheckTy() != Check::CheckEmpty)
    return false;

  Twine CheckName =
      Prefix +
      Twine(Pat.getCheckTy() == Check::CheckEmpty ? "-EMPTY" : "-NEXT");

  // Count the number of newlines between the previous match and this one.
  const char *FirstNewLine = nullptr;
  unsigned NumNewLines = CountNumNewlinesBetween(Buffer, FirstNewLine);

  if (NumNewLines == 0) {
    SM.PrintMessage(Loc, SourceMgr::DK_Error,
                    CheckName + ": is on the same line as previous match");
    SM.PrintMessage(SMLoc::getFromPointer(Buffer.end()), SourceMgr::DK_Note,
                    "'next' match was here");
    SM.PrintMessage(SMLoc::getFromPointer(Buffer.data()), SourceMgr::DK_Note,
                    "previous match ended here");
    return true;
  }

  if (NumNewLines != 1) {
    SM.PrintMessage(Loc, SourceMgr::DK_Error,
                    CheckName +
                        ": is not on the line after the previous match");
    SM.PrintMessage(SMLoc::getFromPointer(Buffer.end()), SourceMgr::DK_Note,
                    "'next' match was here");
    SM.PrintMessage(SMLoc::getFromPointer(Buffer.data()), SourceMgr::DK_Note,
                    "previous match ended here");
    SM.PrintMessage(SMLoc::getFromPointer(FirstNewLine), SourceMgr::DK_Note,
                    "non-matching line after previous match is here");
    return true;
  }

  return false;
}

bool FileCheckString::CheckSame(const SourceMgr &SM, StringRef Buffer) const {
  if (Pat.getCheckTy() != Check::CheckSame)
    return false;

  // Count the number of newlines between the previous match and this one.
  const char *FirstNewLine = nullptr;
  unsigned NumNewLines = CountNumNewlinesBetween(Buffer, FirstNewLine);

  if (NumNewLines != 0) {
    SM.PrintMessage(Loc, SourceMgr::DK_Error,
                    Prefix +
                        "-SAME: is not on the same line as the previous match");
    SM.PrintMessage(SMLoc::getFromPointer(Buffer.end()), SourceMgr::DK_Note,
                    "'next' match was here");
    SM.PrintMessage(SMLoc::getFromPointer(Buffer.data()), SourceMgr::DK_Note,
                    "previous match ended here");
    return true;
  }

  return false;
}

bool FileCheckString::CheckNot(
    const SourceMgr &SM, StringRef Buffer,
    const std::vector<const FileCheckPattern *> &NotStrings,
    const FileCheckRequest &Req, std::vector<FileCheckDiag> *Diags) const {
  for (const FileCheckPattern *Pat : NotStrings) {
    assert((Pat->getCheckTy() == Check::CheckNot) && "Expect CHECK-NOT!");

    size_t MatchLen = 0;
    Expected<size_t> MatchResult = Pat->match(Buffer, MatchLen, SM);

    if (!MatchResult) {
      PrintNoMatch(false, SM, Prefix, Pat->getLoc(), *Pat, 1, Buffer,
                   Req.VerboseVerbose, Diags, MatchResult.takeError());
      continue;
    }
    size_t Pos = *MatchResult;

    PrintMatch(false, SM, Prefix, Pat->getLoc(), *Pat, 1, Buffer, Pos, MatchLen,
               Req, Diags);

    return true;
  }

  return false;
}

size_t
FileCheckString::CheckDag(const SourceMgr &SM, StringRef Buffer,
                          std::vector<const FileCheckPattern *> &NotStrings,
                          const FileCheckRequest &Req,
                          std::vector<FileCheckDiag> *Diags) const {
  if (DagNotStrings.empty())
    return 0;

  // The start of the search range.
  size_t StartPos = 0;

  struct MatchRange {
    size_t Pos;
    size_t End;
  };
  // A sorted list of ranges for non-overlapping CHECK-DAG matches.  Match
  // ranges are erased from this list once they are no longer in the search
  // range.
  std::list<MatchRange> MatchRanges;

  // We need PatItr and PatEnd later for detecting the end of a CHECK-DAG
  // group, so we don't use a range-based for loop here.
  for (auto PatItr = DagNotStrings.begin(), PatEnd = DagNotStrings.end();
       PatItr != PatEnd; ++PatItr) {
    const FileCheckPattern &Pat = *PatItr;
    assert((Pat.getCheckTy() == Check::CheckDAG ||
            Pat.getCheckTy() == Check::CheckNot) &&
           "Invalid CHECK-DAG or CHECK-NOT!");

    if (Pat.getCheckTy() == Check::CheckNot) {
      NotStrings.push_back(&Pat);
      continue;
    }

    assert((Pat.getCheckTy() == Check::CheckDAG) && "Expect CHECK-DAG!");

    // CHECK-DAG always matches from the start.
    size_t MatchLen = 0, MatchPos = StartPos;

    // Search for a match that doesn't overlap a previous match in this
    // CHECK-DAG group.
    for (auto MI = MatchRanges.begin(), ME = MatchRanges.end(); true; ++MI) {
      StringRef MatchBuffer = Buffer.substr(MatchPos);
      Expected<size_t> MatchResult = Pat.match(MatchBuffer, MatchLen, SM);
      // With a group of CHECK-DAGs, a single mismatching means the match on
      // that group of CHECK-DAGs fails immediately.
      if (!MatchResult) {
        PrintNoMatch(true, SM, Prefix, Pat.getLoc(), Pat, 1, MatchBuffer,
                     Req.VerboseVerbose, Diags, MatchResult.takeError());
        return StringRef::npos;
      }
      size_t MatchPosBuf = *MatchResult;
      // Re-calc it as the offset relative to the start of the original string.
      MatchPos += MatchPosBuf;
      if (Req.VerboseVerbose)
        PrintMatch(true, SM, Prefix, Pat.getLoc(), Pat, 1, Buffer, MatchPos,
                   MatchLen, Req, Diags);
      MatchRange M{MatchPos, MatchPos + MatchLen};
      if (Req.AllowDeprecatedDagOverlap) {
        // We don't need to track all matches in this mode, so we just maintain
        // one match range that encompasses the current CHECK-DAG group's
        // matches.
        if (MatchRanges.empty())
          MatchRanges.insert(MatchRanges.end(), M);
        else {
          auto Block = MatchRanges.begin();
          Block->Pos = std::min(Block->Pos, M.Pos);
          Block->End = std::max(Block->End, M.End);
        }
        break;
      }
      // Iterate previous matches until overlapping match or insertion point.
      bool Overlap = false;
      for (; MI != ME; ++MI) {
        if (M.Pos < MI->End) {
          // !Overlap => New match has no overlap and is before this old match.
          // Overlap => New match overlaps this old match.
          Overlap = MI->Pos < M.End;
          break;
        }
      }
      if (!Overlap) {
        // Insert non-overlapping match into list.
        MatchRanges.insert(MI, M);
        break;
      }
      if (Req.VerboseVerbose) {
        // Due to their verbosity, we don't print verbose diagnostics here if
        // we're gathering them for a different rendering, but we always print
        // other diagnostics.
        if (!Diags) {
          SMLoc OldStart = SMLoc::getFromPointer(Buffer.data() + MI->Pos);
          SMLoc OldEnd = SMLoc::getFromPointer(Buffer.data() + MI->End);
          SMRange OldRange(OldStart, OldEnd);
          SM.PrintMessage(OldStart, SourceMgr::DK_Note,
                          "match discarded, overlaps earlier DAG match here",
                          {OldRange});
        } else
          Diags->rbegin()->MatchTy = FileCheckDiag::MatchFoundButDiscarded;
      }
      MatchPos = MI->End;
    }
    if (!Req.VerboseVerbose)
      PrintMatch(true, SM, Prefix, Pat.getLoc(), Pat, 1, Buffer, MatchPos,
                 MatchLen, Req, Diags);

    // Handle the end of a CHECK-DAG group.
    if (std::next(PatItr) == PatEnd ||
        std::next(PatItr)->getCheckTy() == Check::CheckNot) {
      if (!NotStrings.empty()) {
        // If there are CHECK-NOTs between two CHECK-DAGs or from CHECK to
        // CHECK-DAG, verify that there are no 'not' strings occurred in that
        // region.
        StringRef SkippedRegion =
            Buffer.slice(StartPos, MatchRanges.begin()->Pos);
        if (CheckNot(SM, SkippedRegion, NotStrings, Req, Diags))
          return StringRef::npos;
        // Clear "not strings".
        NotStrings.clear();
      }
      // All subsequent CHECK-DAGs and CHECK-NOTs should be matched from the
      // end of this CHECK-DAG group's match range.
      StartPos = MatchRanges.rbegin()->End;
      // Don't waste time checking for (impossible) overlaps before that.
      MatchRanges.clear();
    }
  }

  return StartPos;
}

// A check prefix must contain only alphanumeric, hyphens and underscores.
static bool ValidateCheckPrefix(StringRef CheckPrefix) {
  Regex Validator("^[a-zA-Z0-9_-]*$");
  return Validator.match(CheckPrefix);
}

bool FileCheck::ValidateCheckPrefixes() {
  StringSet<> PrefixSet;

  for (StringRef Prefix : Req.CheckPrefixes) {
    // Reject empty prefixes.
    if (Prefix == "")
      return false;

    if (!PrefixSet.insert(Prefix).second)
      return false;

    if (!ValidateCheckPrefix(Prefix))
      return false;
  }

  return true;
}

Regex FileCheck::buildCheckPrefixRegex() {
  // I don't think there's a way to specify an initial value for cl::list,
  // so if nothing was specified, add the default
  if (Req.CheckPrefixes.empty())
    Req.CheckPrefixes.push_back("CHECK");

  // We already validated the contents of CheckPrefixes so just concatenate
  // them as alternatives.
  SmallString<32> PrefixRegexStr;
  for (StringRef Prefix : Req.CheckPrefixes) {
    if (Prefix != Req.CheckPrefixes.front())
      PrefixRegexStr.push_back('|');

    PrefixRegexStr.append(Prefix);
  }

  return Regex(PrefixRegexStr);
}

Error FileCheckPatternContext::defineCmdlineVariables(
    std::vector<std::string> &CmdlineDefines, SourceMgr &SM) {
  assert(GlobalVariableTable.empty() && GlobalNumericVariableTable.empty() &&
         "Overriding defined variable with command-line variable definitions");

  if (CmdlineDefines.empty())
    return Error::success();

  // Create a string representing the vector of command-line definitions. Each
  // definition is on its own line and prefixed with a definition number to
  // clarify which definition a given diagnostic corresponds to.
  unsigned I = 0;
  Error Errs = Error::success();
  std::string CmdlineDefsDiag;
  StringRef Prefix1 = "Global define #";
  StringRef Prefix2 = ": ";
  for (StringRef CmdlineDef : CmdlineDefines)
    CmdlineDefsDiag +=
        (Prefix1 + Twine(++I) + Prefix2 + CmdlineDef + "\n").str();

  // Create a buffer with fake command line content in order to display
  // parsing diagnostic with location information and point to the
  // global definition with invalid syntax.
  std::unique_ptr<MemoryBuffer> CmdLineDefsDiagBuffer =
      MemoryBuffer::getMemBufferCopy(CmdlineDefsDiag, "Global defines");
  StringRef CmdlineDefsDiagRef = CmdLineDefsDiagBuffer->getBuffer();
  SM.AddNewSourceBuffer(std::move(CmdLineDefsDiagBuffer), SMLoc());

  SmallVector<StringRef, 4> CmdlineDefsDiagVec;
  CmdlineDefsDiagRef.split(CmdlineDefsDiagVec, '\n', -1 /*MaxSplit*/,
                           false /*KeepEmpty*/);
  for (StringRef CmdlineDefDiag : CmdlineDefsDiagVec) {
    unsigned DefStart = CmdlineDefDiag.find(Prefix2) + Prefix2.size();
    StringRef CmdlineDef = CmdlineDefDiag.substr(DefStart);
    size_t EqIdx = CmdlineDef.find('=');
    if (EqIdx == StringRef::npos) {
      Errs = joinErrors(
          std::move(Errs),
          FileCheckErrorDiagnostic::get(
              SM, CmdlineDef, "missing equal sign in global definition"));
      continue;
    }

    // Numeric variable definition.
    if (CmdlineDef[0] == '#') {
      StringRef CmdlineName = CmdlineDef.substr(1, EqIdx - 1);
      Expected<FileCheckNumericVariable *> ParseResult =
          FileCheckPattern::parseNumericVariableDefinition(CmdlineName, this, 0,
                                                           SM);
      if (!ParseResult) {
        Errs = joinErrors(std::move(Errs), ParseResult.takeError());
        continue;
      }

      StringRef CmdlineVal = CmdlineDef.substr(EqIdx + 1);
      uint64_t Val;
      if (CmdlineVal.getAsInteger(10, Val)) {
        Errs = joinErrors(std::move(Errs),
                          FileCheckErrorDiagnostic::get(
                              SM, CmdlineVal,
                              "invalid value in numeric variable definition '" +
                                  CmdlineVal + "'"));
        continue;
      }
      FileCheckNumericVariable *DefinedNumericVariable = *ParseResult;
      DefinedNumericVariable->setValue(Val);

      // Record this variable definition.
      GlobalNumericVariableTable[DefinedNumericVariable->getName()] =
          DefinedNumericVariable;
    } else {
      // String variable definition.
      std::pair<StringRef, StringRef> CmdlineNameVal = CmdlineDef.split('=');
      StringRef CmdlineName = CmdlineNameVal.first;
      StringRef OrigCmdlineName = CmdlineName;
      Expected<FileCheckPattern::VariableProperties> ParseVarResult =
          FileCheckPattern::parseVariable(CmdlineName, SM);
      if (!ParseVarResult) {
        Errs = joinErrors(std::move(Errs), ParseVarResult.takeError());
        continue;
      }
      // Check that CmdlineName does not denote a pseudo variable is only
      // composed of the parsed numeric variable. This catches cases like
      // "FOO+2" in a "FOO+2=10" definition.
      if (ParseVarResult->IsPseudo || !CmdlineName.empty()) {
        Errs = joinErrors(std::move(Errs),
                          FileCheckErrorDiagnostic::get(
                              SM, OrigCmdlineName,
                              "invalid name in string variable definition '" +
                                  OrigCmdlineName + "'"));
        continue;
      }
      StringRef Name = ParseVarResult->Name;

      // Detect collisions between string and numeric variables when the former
      // is created later than the latter.
      if (GlobalNumericVariableTable.find(Name) !=
          GlobalNumericVariableTable.end()) {
        Errs = joinErrors(std::move(Errs), FileCheckErrorDiagnostic::get(
                                               SM, Name,
                                               "numeric variable with name '" +
                                                   Name + "' already exists"));
        continue;
      }
      GlobalVariableTable.insert(CmdlineNameVal);
      // Mark the string variable as defined to detect collisions between
      // string and numeric variables in DefineCmdlineVariables when the latter
      // is created later than the former. We cannot reuse GlobalVariableTable
      // for this by populating it with an empty string since we would then
      // lose the ability to detect the use of an undefined variable in
      // match().
      DefinedVariableTable[Name] = true;
    }
  }

  return Errs;
}

void FileCheckPatternContext::clearLocalVars() {
  SmallVector<StringRef, 16> LocalPatternVars, LocalNumericVars;
  for (const StringMapEntry<StringRef> &Var : GlobalVariableTable)
    if (Var.first()[0] != '$')
      LocalPatternVars.push_back(Var.first());

  // Numeric substitution reads the value of a variable directly, not via
  // GlobalNumericVariableTable. Therefore, we clear local variables by
  // clearing their value which will lead to a numeric substitution failure. We
  // also mark the variable for removal from GlobalNumericVariableTable since
  // this is what defineCmdlineVariables checks to decide that no global
  // variable has been defined.
  for (const auto &Var : GlobalNumericVariableTable)
    if (Var.first()[0] != '$') {
      Var.getValue()->clearValue();
      LocalNumericVars.push_back(Var.first());
    }

  for (const auto &Var : LocalPatternVars)
    GlobalVariableTable.erase(Var);
  for (const auto &Var : LocalNumericVars)
    GlobalNumericVariableTable.erase(Var);
}

bool FileCheck::CheckInput(SourceMgr &SM, StringRef Buffer,
                           ArrayRef<FileCheckString> CheckStrings,
                           std::vector<FileCheckDiag> *Diags) {
  bool ChecksFailed = false;

  unsigned i = 0, j = 0, e = CheckStrings.size();
  while (true) {
    StringRef CheckRegion;
    if (j == e) {
      CheckRegion = Buffer;
    } else {
      const FileCheckString &CheckLabelStr = CheckStrings[j];
      if (CheckLabelStr.Pat.getCheckTy() != Check::CheckLabel) {
        ++j;
        continue;
      }

      // Scan to next CHECK-LABEL match, ignoring CHECK-NOT and CHECK-DAG
      size_t MatchLabelLen = 0;
      size_t MatchLabelPos =
          CheckLabelStr.Check(SM, Buffer, true, MatchLabelLen, Req, Diags);
      if (MatchLabelPos == StringRef::npos)
        // Immediately bail if CHECK-LABEL fails, nothing else we can do.
        return false;

      CheckRegion = Buffer.substr(0, MatchLabelPos + MatchLabelLen);
      Buffer = Buffer.substr(MatchLabelPos + MatchLabelLen);
      ++j;
    }

    // Do not clear the first region as it's the one before the first
    // CHECK-LABEL and it would clear variables defined on the command-line
    // before they get used.
    if (i != 0 && Req.EnableVarScope)
      PatternContext.clearLocalVars();

    for (; i != j; ++i) {
      const FileCheckString &CheckStr = CheckStrings[i];

      // Check each string within the scanned region, including a second check
      // of any final CHECK-LABEL (to verify CHECK-NOT and CHECK-DAG)
      size_t MatchLen = 0;
      size_t MatchPos =
          CheckStr.Check(SM, CheckRegion, false, MatchLen, Req, Diags);

      if (MatchPos == StringRef::npos) {
        ChecksFailed = true;
        i = j;
        break;
      }

      CheckRegion = CheckRegion.substr(MatchPos + MatchLen);
    }

    if (j == e)
      break;
  }

  // Success if no checks failed.
  return !ChecksFailed;
}
