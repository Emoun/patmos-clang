//===--- SemaStmtAttr.cpp - Statement Attribute Handling ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements stmt-related attribute processing.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaInternal.h"
#include "TargetAttributesSema.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/DelayedDiagnostic.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/ScopeInfo.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace sema;

static Attr *handleFallThroughAttr(Sema &S, Stmt *St, const AttributeList &A,
                                   SourceRange Range) {
  if (!isa<NullStmt>(St)) {
    S.Diag(A.getRange().getBegin(), diag::err_fallthrough_attr_wrong_target)
        << St->getLocStart();
    if (isa<SwitchCase>(St)) {
      SourceLocation L = Lexer::getLocForEndOfToken(Range.getEnd(), 0,
                                  S.getSourceManager(), S.getLangOpts());
      S.Diag(L, diag::note_fallthrough_insert_semi_fixit)
          << FixItHint::CreateInsertion(L, ";");
    }
    return 0;
  }
  if (S.getCurFunction()->SwitchStack.empty()) {
    S.Diag(A.getRange().getBegin(), diag::err_fallthrough_attr_outside_switch);
    return 0;
  }
  return ::new (S.Context) FallThroughAttr(A.getRange(), S.Context);
}

static Attr *handleLoopboundAttr(Sema &S, Stmt *St, const AttributeList &A,
                                 SourceRange Range) {
  IdentifierLoc *PragmaNameLoc = A.getArgAsIdent(0);
  Expr *MinExpr = A.getArgAsExpr(1);
  Expr *MaxExpr = A.getArgAsExpr(2);

  if (St->getStmtClass() != Stmt::DoStmtClass &&
      St->getStmtClass() != Stmt::ForStmtClass &&
      St->getStmtClass() != Stmt::CXXForRangeStmtClass &&
      St->getStmtClass() != Stmt::WhileStmtClass) {
    S.Diag(St->getLocStart(), diag::err_pragma_loop_precedes_nonloop)
       << "#pragma loopbound";
    return 0;
  }

  llvm::APSInt MinAPS, MaxAPS;
  assert(MinExpr != NULL && MaxExpr != NULL);
  if (!MinExpr->isIntegerConstantExpr(MinAPS, S.Context) ||
      !MaxExpr->isIntegerConstantExpr(MaxAPS, S.Context)) {
    S.Diag(A.getLoc(), diag::err_pragma_loopbound_invalid_values);
    return 0;
  }

  int MinInt = MinAPS.getSExtValue();
  int MaxInt = MaxAPS.getSExtValue();

  if ( MinInt < 0 || MaxInt < 0 || MinInt > MaxInt) {
    S.Diag(A.getLoc(), diag::err_pragma_loopbound_invalid_values);
    return 0;
  }

  return ::new (S.Context) LoopboundAttr(A.getRange(), S.Context,
      MinInt, MaxInt);
}

static Attr *ProcessStmtAttribute(Sema &S, Stmt *St, const AttributeList &A,
                                  SourceRange Range) {
  switch (A.getKind()) {
  case AttributeList::UnknownAttribute:
    S.Diag(A.getLoc(), A.isDeclspecAttribute() ?
           diag::warn_unhandled_ms_attribute_ignored :
           diag::warn_unknown_attribute_ignored) << A.getName();
    return 0;
  case AttributeList::AT_FallThrough:
    return handleFallThroughAttr(S, St, A, Range);
  case AttributeList::AT_Loopbound:
    return handleLoopboundAttr(S, St, A, Range);
  default:
    // if we're here, then we parsed a known attribute, but didn't recognize
    // it as a statement attribute => it is declaration attribute
    S.Diag(A.getRange().getBegin(), diag::err_attribute_invalid_on_stmt)
        << A.getName() << St->getLocStart();
    return 0;
  }
}

StmtResult Sema::ProcessStmtAttributes(Stmt *S, AttributeList *AttrList,
                                       SourceRange Range) {
  SmallVector<const Attr*, 8> Attrs;
  for (const AttributeList* l = AttrList; l; l = l->getNext()) {
    if (Attr *a = ProcessStmtAttribute(*this, S, *l, Range))
      Attrs.push_back(a);
  }

  if (Attrs.empty())
    return S;

  return ActOnAttributedStmt(Range.getBegin(), Attrs, S);
}
