//===--- ParseDeclCXX.cpp - C++ Declaration Parsing -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements the C++ Declaration portions of the Parser interfaces.
//
//===----------------------------------------------------------------------===//

#include "clang/Parse/Parser.h"
#include "RAIIObjectsForParser.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/Attributes.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/OperatorKinds.h"
#include "clang/Parse/ParseDiagnostic.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/ParsedTemplate.h"
#include "clang/Sema/PrettyDeclStackTrace.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "llvm/ADT/SmallString.h"
using namespace clang;

/// ParseNamespace - We know that the current token is a namespace keyword. This
/// may either be a top level namespace or a block-level namespace alias. If
/// there was an inline keyword, it has already been parsed.
///
///       namespace-definition: [C++ 7.3: basic.namespace]
///         named-namespace-definition
///         unnamed-namespace-definition
///
///       unnamed-namespace-definition:
///         'inline'[opt] 'namespace' attributes[opt] '{' namespace-body '}'
///
///       named-namespace-definition:
///         original-namespace-definition
///         extension-namespace-definition
///
///       original-namespace-definition:
///         'inline'[opt] 'namespace' identifier attributes[opt]
///             '{' namespace-body '}'
///
///       extension-namespace-definition:
///         'inline'[opt] 'namespace' original-namespace-name
///             '{' namespace-body '}'
///
///       namespace-alias-definition:  [C++ 7.3.2: namespace.alias]
///         'namespace' identifier '=' qualified-namespace-specifier ';'
///
Decl *Parser::ParseNamespace(unsigned Context,
                             SourceLocation &DeclEnd,
                             SourceLocation InlineLoc) {
  assert(Tok.is(tok::kw_namespace) && "Not a namespace!");
  SourceLocation NamespaceLoc = ConsumeToken();  // eat the 'namespace'.
  ObjCDeclContextSwitch ObjCDC(*this);
    
  if (Tok.is(tok::code_completion)) {
    Actions.CodeCompleteNamespaceDecl(getCurScope());
    cutOffParsing();
    return nullptr;
  }

  SourceLocation IdentLoc;
  IdentifierInfo *Ident = nullptr;
  std::vector<SourceLocation> ExtraIdentLoc;
  std::vector<IdentifierInfo*> ExtraIdent;
  std::vector<SourceLocation> ExtraNamespaceLoc;

  Token attrTok;

  if (Tok.is(tok::identifier)) {
    Ident = Tok.getIdentifierInfo();
    IdentLoc = ConsumeToken();  // eat the identifier.
    while (Tok.is(tok::coloncolon) && NextToken().is(tok::identifier)) {
      ExtraNamespaceLoc.push_back(ConsumeToken());
      ExtraIdent.push_back(Tok.getIdentifierInfo());
      ExtraIdentLoc.push_back(ConsumeToken());
    }
  }

  // Read label attributes, if present.
  ParsedAttributes attrs(AttrFactory);
  if (Tok.is(tok::kw___attribute)) {
    attrTok = Tok;
    ParseGNUAttributes(attrs);
  }

  if (Tok.is(tok::equal)) {
    if (!Ident) {
      Diag(Tok, diag::err_expected) << tok::identifier;
      // Skip to end of the definition and eat the ';'.
      SkipUntil(tok::semi);
      return nullptr;
    }
    if (!attrs.empty())
      Diag(attrTok, diag::err_unexpected_namespace_attributes_alias);
    if (InlineLoc.isValid())
      Diag(InlineLoc, diag::err_inline_namespace_alias)
          << FixItHint::CreateRemoval(InlineLoc);
    return ParseNamespaceAlias(NamespaceLoc, IdentLoc, Ident, DeclEnd);
  }


  BalancedDelimiterTracker T(*this, tok::l_brace);
  if (T.consumeOpen()) {
    if (!ExtraIdent.empty()) {
      Diag(ExtraNamespaceLoc[0], diag::err_nested_namespaces_with_double_colon)
          << SourceRange(ExtraNamespaceLoc.front(), ExtraIdentLoc.back());
    }

    if (Ident)
      Diag(Tok, diag::err_expected) << tok::l_brace;
    else
      Diag(Tok, diag::err_expected_either) << tok::identifier << tok::l_brace;

    return nullptr;
  }

  if (getCurScope()->isClassScope() || getCurScope()->isTemplateParamScope() || 
      getCurScope()->isInObjcMethodScope() || getCurScope()->getBlockParent() || 
      getCurScope()->getFnParent()) {
    if (!ExtraIdent.empty()) {
      Diag(ExtraNamespaceLoc[0], diag::err_nested_namespaces_with_double_colon)
          << SourceRange(ExtraNamespaceLoc.front(), ExtraIdentLoc.back());
    }
    Diag(T.getOpenLocation(), diag::err_namespace_nonnamespace_scope);
    SkipUntil(tok::r_brace);
    return nullptr;
  }

  if (!ExtraIdent.empty()) {
    TentativeParsingAction TPA(*this);
    SkipUntil(tok::r_brace, StopBeforeMatch);
    Token rBraceToken = Tok;
    TPA.Revert();

    if (!rBraceToken.is(tok::r_brace)) {
      Diag(ExtraNamespaceLoc[0], diag::err_nested_namespaces_with_double_colon)
          << SourceRange(ExtraNamespaceLoc.front(), ExtraIdentLoc.back());
    } else {
      std::string NamespaceFix;
      for (std::vector<IdentifierInfo*>::iterator I = ExtraIdent.begin(),
           E = ExtraIdent.end(); I != E; ++I) {
        NamespaceFix += " { namespace ";
        NamespaceFix += (*I)->getName();
      }

      std::string RBraces;
      for (unsigned i = 0, e = ExtraIdent.size(); i != e; ++i)
        RBraces +=  "} ";

      Diag(ExtraNamespaceLoc[0], diag::err_nested_namespaces_with_double_colon)
          << FixItHint::CreateReplacement(SourceRange(ExtraNamespaceLoc.front(),
                                                      ExtraIdentLoc.back()),
                                          NamespaceFix)
          << FixItHint::CreateInsertion(rBraceToken.getLocation(), RBraces);
    }
  }

  // If we're still good, complain about inline namespaces in non-C++0x now.
  if (InlineLoc.isValid())
    Diag(InlineLoc, getLangOpts().CPlusPlus11 ?
         diag::warn_cxx98_compat_inline_namespace : diag::ext_inline_namespace);

  // Enter a scope for the namespace.
  ParseScope NamespaceScope(this, Scope::DeclScope);

  Decl *NamespcDecl =
    Actions.ActOnStartNamespaceDef(getCurScope(), InlineLoc, NamespaceLoc,
                                   IdentLoc, Ident, T.getOpenLocation(), 
                                   attrs.getList());

  PrettyDeclStackTraceEntry CrashInfo(Actions, NamespcDecl, NamespaceLoc,
                                      "parsing namespace");

  // Parse the contents of the namespace.  This includes parsing recovery on 
  // any improperly nested namespaces.
  ParseInnerNamespace(ExtraIdentLoc, ExtraIdent, ExtraNamespaceLoc, 0,
                      InlineLoc, attrs, T);

  // Leave the namespace scope.
  NamespaceScope.Exit();

  DeclEnd = T.getCloseLocation();
  Actions.ActOnFinishNamespaceDef(NamespcDecl, DeclEnd);

  return NamespcDecl;
}

/// ParseInnerNamespace - Parse the contents of a namespace.
void Parser::ParseInnerNamespace(std::vector<SourceLocation>& IdentLoc,
                                 std::vector<IdentifierInfo*>& Ident,
                                 std::vector<SourceLocation>& NamespaceLoc,
                                 unsigned int index, SourceLocation& InlineLoc,
                                 ParsedAttributes& attrs,
                                 BalancedDelimiterTracker &Tracker) {
  if (index == Ident.size()) {
    while (Tok.isNot(tok::r_brace) && !isEofOrEom()) {
      ParsedAttributesWithRange attrs(AttrFactory);
      MaybeParseCXX11Attributes(attrs);
      MaybeParseMicrosoftAttributes(attrs);
      ParseExternalDeclaration(attrs);
    }

    // The caller is what called check -- we are simply calling
    // the close for it.
    Tracker.consumeClose();

    return;
  }

  // Parse improperly nested namespaces.
  ParseScope NamespaceScope(this, Scope::DeclScope);
  Decl *NamespcDecl =
    Actions.ActOnStartNamespaceDef(getCurScope(), SourceLocation(),
                                   NamespaceLoc[index], IdentLoc[index],
                                   Ident[index], Tracker.getOpenLocation(), 
                                   attrs.getList());

  ParseInnerNamespace(IdentLoc, Ident, NamespaceLoc, ++index, InlineLoc,
                      attrs, Tracker);

  NamespaceScope.Exit();

  Actions.ActOnFinishNamespaceDef(NamespcDecl, Tracker.getCloseLocation());
}

/// ParseNamespaceAlias - Parse the part after the '=' in a namespace
/// alias definition.
///
Decl *Parser::ParseNamespaceAlias(SourceLocation NamespaceLoc,
                                  SourceLocation AliasLoc,
                                  IdentifierInfo *Alias,
                                  SourceLocation &DeclEnd) {
  assert(Tok.is(tok::equal) && "Not equal token");

  ConsumeToken(); // eat the '='.

  if (Tok.is(tok::code_completion)) {
    Actions.CodeCompleteNamespaceAliasDecl(getCurScope());
    cutOffParsing();
    return nullptr;
  }

  CXXScopeSpec SS;
  // Parse (optional) nested-name-specifier.
  ParseOptionalCXXScopeSpecifier(SS, ParsedType(), /*EnteringContext=*/false);

  if (SS.isInvalid() || Tok.isNot(tok::identifier)) {
    Diag(Tok, diag::err_expected_namespace_name);
    // Skip to end of the definition and eat the ';'.
    SkipUntil(tok::semi);
    return nullptr;
  }

  // Parse identifier.
  IdentifierInfo *Ident = Tok.getIdentifierInfo();
  SourceLocation IdentLoc = ConsumeToken();

  // Eat the ';'.
  DeclEnd = Tok.getLocation();
  if (ExpectAndConsume(tok::semi, diag::err_expected_semi_after_namespace_name))
    SkipUntil(tok::semi);

  return Actions.ActOnNamespaceAliasDef(getCurScope(), NamespaceLoc, AliasLoc, Alias,
                                        SS, IdentLoc, Ident);
}

/// ParseLinkage - We know that the current token is a string_literal
/// and just before that, that extern was seen.
///
///       linkage-specification: [C++ 7.5p2: dcl.link]
///         'extern' string-literal '{' declaration-seq[opt] '}'
///         'extern' string-literal declaration
///
Decl *Parser::ParseLinkage(ParsingDeclSpec &DS, unsigned Context) {
  assert(isTokenStringLiteral() && "Not a string literal!");
  ExprResult Lang = ParseStringLiteralExpression(false);

  ParseScope LinkageScope(this, Scope::DeclScope);
  Decl *LinkageSpec =
      Lang.isInvalid()
          ? nullptr
          : Actions.ActOnStartLinkageSpecification(
                getCurScope(), DS.getSourceRange().getBegin(), Lang.get(),
                Tok.is(tok::l_brace) ? Tok.getLocation() : SourceLocation());

  ParsedAttributesWithRange attrs(AttrFactory);
  MaybeParseCXX11Attributes(attrs);
  MaybeParseMicrosoftAttributes(attrs);

  if (Tok.isNot(tok::l_brace)) {
    // Reset the source range in DS, as the leading "extern"
    // does not really belong to the inner declaration ...
    DS.SetRangeStart(SourceLocation());
    DS.SetRangeEnd(SourceLocation());
    // ... but anyway remember that such an "extern" was seen.
    DS.setExternInLinkageSpec(true);
    ParseExternalDeclaration(attrs, &DS);
    return LinkageSpec ? Actions.ActOnFinishLinkageSpecification(
                             getCurScope(), LinkageSpec, SourceLocation())
                       : nullptr;
  }

  DS.abort();

  ProhibitAttributes(attrs);

  BalancedDelimiterTracker T(*this, tok::l_brace);
  T.consumeOpen();

  unsigned NestedModules = 0;
  while (true) {
    switch (Tok.getKind()) {
    case tok::annot_module_begin:
      ++NestedModules;
      ParseTopLevelDecl();
      continue;

    case tok::annot_module_end:
      if (!NestedModules)
        break;
      --NestedModules;
      ParseTopLevelDecl();
      continue;

    case tok::annot_module_include:
      ParseTopLevelDecl();
      continue;

    case tok::eof:
      break;

    case tok::r_brace:
      if (!NestedModules)
        break;
      // Fall through.
    default:
      ParsedAttributesWithRange attrs(AttrFactory);
      MaybeParseCXX11Attributes(attrs);
      MaybeParseMicrosoftAttributes(attrs);
      ParseExternalDeclaration(attrs);
      continue;
    }

    break;
  }

  T.consumeClose();
  return LinkageSpec ? Actions.ActOnFinishLinkageSpecification(
                           getCurScope(), LinkageSpec, T.getCloseLocation())
                     : nullptr;
}

/// ParseUsingDirectiveOrDeclaration - Parse C++ using using-declaration or
/// using-directive. Assumes that current token is 'using'.
Decl *Parser::ParseUsingDirectiveOrDeclaration(unsigned Context,
                                         const ParsedTemplateInfo &TemplateInfo,
                                               SourceLocation &DeclEnd,
                                             ParsedAttributesWithRange &attrs,
                                               Decl **OwnedType) {
  assert(Tok.is(tok::kw_using) && "Not using token");
  ObjCDeclContextSwitch ObjCDC(*this);
  
  // Eat 'using'.
  SourceLocation UsingLoc = ConsumeToken();

  if (Tok.is(tok::code_completion)) {
    Actions.CodeCompleteUsing(getCurScope());
    cutOffParsing();
    return nullptr;
  }

  // 'using namespace' means this is a using-directive.
  if (Tok.is(tok::kw_namespace)) {
    // Template parameters are always an error here.
    if (TemplateInfo.Kind) {
      SourceRange R = TemplateInfo.getSourceRange();
      Diag(UsingLoc, diag::err_templated_using_directive)
        << R << FixItHint::CreateRemoval(R);
    }

    return ParseUsingDirective(Context, UsingLoc, DeclEnd, attrs);
  }

  // Otherwise, it must be a using-declaration or an alias-declaration.

  // Using declarations can't have attributes.
  ProhibitAttributes(attrs);

  return ParseUsingDeclaration(Context, TemplateInfo, UsingLoc, DeclEnd,
                                    AS_none, OwnedType);
}

/// ParseUsingDirective - Parse C++ using-directive, assumes
/// that current token is 'namespace' and 'using' was already parsed.
///
///       using-directive: [C++ 7.3.p4: namespace.udir]
///        'using' 'namespace' ::[opt] nested-name-specifier[opt]
///                 namespace-name ;
/// [GNU] using-directive:
///        'using' 'namespace' ::[opt] nested-name-specifier[opt]
///                 namespace-name attributes[opt] ;
///
Decl *Parser::ParseUsingDirective(unsigned Context,
                                  SourceLocation UsingLoc,
                                  SourceLocation &DeclEnd,
                                  ParsedAttributes &attrs) {
  assert(Tok.is(tok::kw_namespace) && "Not 'namespace' token");

  // Eat 'namespace'.
  SourceLocation NamespcLoc = ConsumeToken();

  if (Tok.is(tok::code_completion)) {
    Actions.CodeCompleteUsingDirective(getCurScope());
    cutOffParsing();
    return nullptr;
  }

  CXXScopeSpec SS;
  // Parse (optional) nested-name-specifier.
  ParseOptionalCXXScopeSpecifier(SS, ParsedType(), /*EnteringContext=*/false);

  IdentifierInfo *NamespcName = nullptr;
  SourceLocation IdentLoc = SourceLocation();

  // Parse namespace-name.
  if (SS.isInvalid() || Tok.isNot(tok::identifier)) {
    Diag(Tok, diag::err_expected_namespace_name);
    // If there was invalid namespace name, skip to end of decl, and eat ';'.
    SkipUntil(tok::semi);
    // FIXME: Are there cases, when we would like to call ActOnUsingDirective?
    return nullptr;
  }

  // Parse identifier.
  NamespcName = Tok.getIdentifierInfo();
  IdentLoc = ConsumeToken();

  // Parse (optional) attributes (most likely GNU strong-using extension).
  bool GNUAttr = false;
  if (Tok.is(tok::kw___attribute)) {
    GNUAttr = true;
    ParseGNUAttributes(attrs);
  }

  // Eat ';'.
  DeclEnd = Tok.getLocation();
  if (ExpectAndConsume(tok::semi,
                       GNUAttr ? diag::err_expected_semi_after_attribute_list
                               : diag::err_expected_semi_after_namespace_name))
    SkipUntil(tok::semi);

  return Actions.ActOnUsingDirective(getCurScope(), UsingLoc, NamespcLoc, SS,
                                     IdentLoc, NamespcName, attrs.getList());
}

/// ParseUsingDeclaration - Parse C++ using-declaration or alias-declaration.
/// Assumes that 'using' was already seen.
///
///     using-declaration: [C++ 7.3.p3: namespace.udecl]
///       'using' 'typename'[opt] ::[opt] nested-name-specifier
///               unqualified-id
///       'using' :: unqualified-id
///
///     alias-declaration: C++11 [dcl.dcl]p1
///       'using' identifier attribute-specifier-seq[opt] = type-id ;
///
Decl *Parser::ParseUsingDeclaration(unsigned Context,
                                    const ParsedTemplateInfo &TemplateInfo,
                                    SourceLocation UsingLoc,
                                    SourceLocation &DeclEnd,
                                    AccessSpecifier AS,
                                    Decl **OwnedType) {
  CXXScopeSpec SS;
  SourceLocation TypenameLoc;
  bool HasTypenameKeyword = false;

  // Check for misplaced attributes before the identifier in an
  // alias-declaration.
  ParsedAttributesWithRange MisplacedAttrs(AttrFactory);
  MaybeParseCXX11Attributes(MisplacedAttrs);

  // Ignore optional 'typename'.
  // FIXME: This is wrong; we should parse this as a typename-specifier.
  if (TryConsumeToken(tok::kw_typename, TypenameLoc))
    HasTypenameKeyword = true;

  // Parse nested-name-specifier.
  IdentifierInfo *LastII = nullptr;
  ParseOptionalCXXScopeSpecifier(SS, ParsedType(), /*EnteringContext=*/false,
                                 /*MayBePseudoDtor=*/nullptr,
                                 /*IsTypename=*/false,
                                 /*LastII=*/&LastII);

  // Check nested-name specifier.
  if (SS.isInvalid()) {
    SkipUntil(tok::semi);
    return nullptr;
  }

  SourceLocation TemplateKWLoc;
  UnqualifiedId Name;

  // Parse the unqualified-id. We allow parsing of both constructor and
  // destructor names and allow the action module to diagnose any semantic
  // errors.
  //
  // C++11 [class.qual]p2:
  //   [...] in a using-declaration that is a member-declaration, if the name
  //   specified after the nested-name-specifier is the same as the identifier
  //   or the simple-template-id's template-name in the last component of the
  //   nested-name-specifier, the name is [...] considered to name the
  //   constructor.
  if (getLangOpts().CPlusPlus11 && Context == Declarator::MemberContext &&
      Tok.is(tok::identifier) && NextToken().is(tok::semi) &&
      SS.isNotEmpty() && LastII == Tok.getIdentifierInfo() &&
      !SS.getScopeRep()->getAsNamespace() &&
      !SS.getScopeRep()->getAsNamespaceAlias()) {
    SourceLocation IdLoc = ConsumeToken();
    ParsedType Type = Actions.getInheritingConstructorName(SS, IdLoc, *LastII);
    Name.setConstructorName(Type, IdLoc, IdLoc);
  } else if (ParseUnqualifiedId(SS, /*EnteringContext=*/ false,
                                /*AllowDestructorName=*/ true,
                                /*AllowConstructorName=*/ true, ParsedType(),
                                TemplateKWLoc, Name)) {
    SkipUntil(tok::semi);
    return nullptr;
  }

  ParsedAttributesWithRange Attrs(AttrFactory);
  MaybeParseGNUAttributes(Attrs);
  MaybeParseCXX11Attributes(Attrs);

  // Maybe this is an alias-declaration.
  TypeResult TypeAlias;
  bool IsAliasDecl = Tok.is(tok::equal);
  if (IsAliasDecl) {
    // If we had any misplaced attributes from earlier, this is where they
    // should have been written.
    if (MisplacedAttrs.Range.isValid()) {
      Diag(MisplacedAttrs.Range.getBegin(), diag::err_attributes_not_allowed)
        << FixItHint::CreateInsertionFromRange(
               Tok.getLocation(),
               CharSourceRange::getTokenRange(MisplacedAttrs.Range))
        << FixItHint::CreateRemoval(MisplacedAttrs.Range);
      Attrs.takeAllFrom(MisplacedAttrs);
    }

    ConsumeToken();

    Diag(Tok.getLocation(), getLangOpts().CPlusPlus11 ?
         diag::warn_cxx98_compat_alias_declaration :
         diag::ext_alias_declaration);

    // Type alias templates cannot be specialized.
    int SpecKind = -1;
    if (TemplateInfo.Kind == ParsedTemplateInfo::Template &&
        Name.getKind() == UnqualifiedId::IK_TemplateId)
      SpecKind = 0;
    if (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitSpecialization)
      SpecKind = 1;
    if (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation)
      SpecKind = 2;
    if (SpecKind != -1) {
      SourceRange Range;
      if (SpecKind == 0)
        Range = SourceRange(Name.TemplateId->LAngleLoc,
                            Name.TemplateId->RAngleLoc);
      else
        Range = TemplateInfo.getSourceRange();
      Diag(Range.getBegin(), diag::err_alias_declaration_specialization)
        << SpecKind << Range;
      SkipUntil(tok::semi);
      return nullptr;
    }

    // Name must be an identifier.
    if (Name.getKind() != UnqualifiedId::IK_Identifier) {
      Diag(Name.StartLocation, diag::err_alias_declaration_not_identifier);
      // No removal fixit: can't recover from this.
      SkipUntil(tok::semi);
      return nullptr;
    } else if (HasTypenameKeyword)
      Diag(TypenameLoc, diag::err_alias_declaration_not_identifier)
        << FixItHint::CreateRemoval(SourceRange(TypenameLoc,
                             SS.isNotEmpty() ? SS.getEndLoc() : TypenameLoc));
    else if (SS.isNotEmpty())
      Diag(SS.getBeginLoc(), diag::err_alias_declaration_not_identifier)
        << FixItHint::CreateRemoval(SS.getRange());

    TypeAlias = ParseTypeName(nullptr, TemplateInfo.Kind ?
                              Declarator::AliasTemplateContext :
                              Declarator::AliasDeclContext, AS, OwnedType,
                              &Attrs);
  } else {
    // C++11 attributes are not allowed on a using-declaration, but GNU ones
    // are.
    ProhibitAttributes(MisplacedAttrs);
    ProhibitAttributes(Attrs);

    // Parse (optional) attributes (most likely GNU strong-using extension).
    MaybeParseGNUAttributes(Attrs);
  }

  // Eat ';'.
  DeclEnd = Tok.getLocation();
  if (ExpectAndConsume(tok::semi, diag::err_expected_after,
                       !Attrs.empty() ? "attributes list"
                                      : IsAliasDecl ? "alias declaration"
                                                    : "using declaration"))
    SkipUntil(tok::semi);

  // Diagnose an attempt to declare a templated using-declaration.
  // In C++11, alias-declarations can be templates:
  //   template <...> using id = type;
  if (TemplateInfo.Kind && !IsAliasDecl) {
    SourceRange R = TemplateInfo.getSourceRange();
    Diag(UsingLoc, diag::err_templated_using_declaration)
      << R << FixItHint::CreateRemoval(R);

    // Unfortunately, we have to bail out instead of recovering by
    // ignoring the parameters, just in case the nested name specifier
    // depends on the parameters.
    return nullptr;
  }

  // "typename" keyword is allowed for identifiers only,
  // because it may be a type definition.
  if (HasTypenameKeyword && Name.getKind() != UnqualifiedId::IK_Identifier) {
    Diag(Name.getSourceRange().getBegin(), diag::err_typename_identifiers_only)
      << FixItHint::CreateRemoval(SourceRange(TypenameLoc));
    // Proceed parsing, but reset the HasTypenameKeyword flag.
    HasTypenameKeyword = false;
  }

  if (IsAliasDecl) {
    TemplateParameterLists *TemplateParams = TemplateInfo.TemplateParams;
    MultiTemplateParamsArg TemplateParamsArg(
      TemplateParams ? TemplateParams->data() : nullptr,
      TemplateParams ? TemplateParams->size() : 0);
    return Actions.ActOnAliasDeclaration(getCurScope(), AS, TemplateParamsArg,
                                         UsingLoc, Name, Attrs.getList(),
                                         TypeAlias);
  }

  return Actions.ActOnUsingDeclaration(getCurScope(), AS,
                                       /* HasUsingKeyword */ true, UsingLoc,
                                       SS, Name, Attrs.getList(),
                                       HasTypenameKeyword, TypenameLoc);
}

/// ParseStaticAssertDeclaration - Parse C++0x or C11 static_assert-declaration.
///
/// [C++0x] static_assert-declaration:
///           static_assert ( constant-expression  ,  string-literal  ) ;
///
/// [C11]   static_assert-declaration:
///           _Static_assert ( constant-expression  ,  string-literal  ) ;
///
Decl *Parser::ParseStaticAssertDeclaration(SourceLocation &DeclEnd){
  assert((Tok.is(tok::kw_static_assert) || Tok.is(tok::kw__Static_assert)) &&
         "Not a static_assert declaration");

  if (Tok.is(tok::kw__Static_assert) && !getLangOpts().C11)
    Diag(Tok, diag::ext_c11_static_assert);
  if (Tok.is(tok::kw_static_assert))
    Diag(Tok, diag::warn_cxx98_compat_static_assert);

  SourceLocation StaticAssertLoc = ConsumeToken();

  BalancedDelimiterTracker T(*this, tok::l_paren);
  if (T.consumeOpen()) {
    Diag(Tok, diag::err_expected) << tok::l_paren;
    SkipMalformedDecl();
    return nullptr;
  }

  ExprResult AssertExpr(ParseConstantExpression());
  if (AssertExpr.isInvalid()) {
    SkipMalformedDecl();
    return nullptr;
  }

  ExprResult AssertMessage;
  if (Tok.is(tok::r_paren)) {
    Diag(Tok, getLangOpts().CPlusPlus1z
                  ? diag::warn_cxx1y_compat_static_assert_no_message
                  : diag::ext_static_assert_no_message)
      << (getLangOpts().CPlusPlus1z
              ? FixItHint()
              : FixItHint::CreateInsertion(Tok.getLocation(), ", \"\""));
  } else {
    if (ExpectAndConsume(tok::comma)) {
      SkipUntil(tok::semi);
      return nullptr;
    }

    if (!isTokenStringLiteral()) {
      Diag(Tok, diag::err_expected_string_literal)
        << /*Source='static_assert'*/1;
      SkipMalformedDecl();
      return nullptr;
    }

    AssertMessage = ParseStringLiteralExpression();
    if (AssertMessage.isInvalid()) {
      SkipMalformedDecl();
      return nullptr;
    }
  }

  T.consumeClose();

  DeclEnd = Tok.getLocation();
  ExpectAndConsumeSemi(diag::err_expected_semi_after_static_assert);

  return Actions.ActOnStaticAssertDeclaration(StaticAssertLoc,
                                              AssertExpr.get(),
                                              AssertMessage.get(),
                                              T.getCloseLocation());
}

/// ParseDecltypeSpecifier - Parse a C++11 decltype specifier.
///
/// 'decltype' ( expression )
/// 'decltype' ( 'auto' )      [C++1y]
///
SourceLocation Parser::ParseDecltypeSpecifier(DeclSpec &DS) {
  assert((Tok.is(tok::kw_decltype) || Tok.is(tok::annot_decltype))
           && "Not a decltype specifier");
  
  ExprResult Result;
  SourceLocation StartLoc = Tok.getLocation();
  SourceLocation EndLoc;

  if (Tok.is(tok::annot_decltype)) {
    Result = getExprAnnotation(Tok);
    EndLoc = Tok.getAnnotationEndLoc();
    ConsumeToken();
    if (Result.isInvalid()) {
      DS.SetTypeSpecError();
      return EndLoc;
    }
  } else {
    if (Tok.getIdentifierInfo()->isStr("decltype"))
      Diag(Tok, diag::warn_cxx98_compat_decltype);

    ConsumeToken();

    BalancedDelimiterTracker T(*this, tok::l_paren);
    if (T.expectAndConsume(diag::err_expected_lparen_after,
                           "decltype", tok::r_paren)) {
      DS.SetTypeSpecError();
      return T.getOpenLocation() == Tok.getLocation() ?
             StartLoc : T.getOpenLocation();
    }

    // Check for C++1y 'decltype(auto)'.
    if (Tok.is(tok::kw_auto)) {
      // No need to disambiguate here: an expression can't start with 'auto',
      // because the typename-specifier in a function-style cast operation can't
      // be 'auto'.
      Diag(Tok.getLocation(),
           getLangOpts().CPlusPlus1y
             ? diag::warn_cxx11_compat_decltype_auto_type_specifier
             : diag::ext_decltype_auto_type_specifier);
      ConsumeToken();
    } else {
      // Parse the expression

      // C++11 [dcl.type.simple]p4:
      //   The operand of the decltype specifier is an unevaluated operand.
      EnterExpressionEvaluationContext Unevaluated(Actions, Sema::Unevaluated,
                                                   nullptr,/*IsDecltype=*/true);
      Result = ParseExpression();
      if (Result.isInvalid()) {
        DS.SetTypeSpecError();
        if (SkipUntil(tok::r_paren, StopAtSemi | StopBeforeMatch)) {
          EndLoc = ConsumeParen();
        } else {
          if (PP.isBacktrackEnabled() && Tok.is(tok::semi)) {
            // Backtrack to get the location of the last token before the semi.
            PP.RevertCachedTokens(2);
            ConsumeToken(); // the semi.
            EndLoc = ConsumeAnyToken();
            assert(Tok.is(tok::semi));
          } else {
            EndLoc = Tok.getLocation();
          }
        }
        return EndLoc;
      }

      Result = Actions.ActOnDecltypeExpression(Result.get());
    }

    // Match the ')'
    T.consumeClose();
    if (T.getCloseLocation().isInvalid()) {
      DS.SetTypeSpecError();
      // FIXME: this should return the location of the last token
      //        that was consumed (by "consumeClose()")
      return T.getCloseLocation();
    }

    if (Result.isInvalid()) {
      DS.SetTypeSpecError();
      return T.getCloseLocation();
    }

    EndLoc = T.getCloseLocation();
  }
  assert(!Result.isInvalid());

  const char *PrevSpec = nullptr;
  unsigned DiagID;
  const PrintingPolicy &Policy = Actions.getASTContext().getPrintingPolicy();
  // Check for duplicate type specifiers (e.g. "int decltype(a)").
  if (Result.get()
        ? DS.SetTypeSpecType(DeclSpec::TST_decltype, StartLoc, PrevSpec,
                             DiagID, Result.get(), Policy)
        : DS.SetTypeSpecType(DeclSpec::TST_decltype_auto, StartLoc, PrevSpec,
                             DiagID, Policy)) {
    Diag(StartLoc, DiagID) << PrevSpec;
    DS.SetTypeSpecError();
  }
  return EndLoc;
}

void Parser::AnnotateExistingDecltypeSpecifier(const DeclSpec& DS, 
                                               SourceLocation StartLoc,
                                               SourceLocation EndLoc) {
  // make sure we have a token we can turn into an annotation token
  if (PP.isBacktrackEnabled())
    PP.RevertCachedTokens(1);
  else
    PP.EnterToken(Tok);

  Tok.setKind(tok::annot_decltype);
  setExprAnnotation(Tok,
                    DS.getTypeSpecType() == TST_decltype ? DS.getRepAsExpr() :
                    DS.getTypeSpecType() == TST_decltype_auto ? ExprResult() :
                    ExprError());
  Tok.setAnnotationEndLoc(EndLoc);
  Tok.setLocation(StartLoc);
  PP.AnnotateCachedTokens(Tok);
}

void Parser::ParseUnderlyingTypeSpecifier(DeclSpec &DS) {
  assert(Tok.is(tok::kw___underlying_type) &&
         "Not an underlying type specifier");

  SourceLocation StartLoc = ConsumeToken();
  BalancedDelimiterTracker T(*this, tok::l_paren);
  if (T.expectAndConsume(diag::err_expected_lparen_after,
                       "__underlying_type", tok::r_paren)) {
    return;
  }

  TypeResult Result = ParseTypeName();
  if (Result.isInvalid()) {
    SkipUntil(tok::r_paren, StopAtSemi);
    return;
  }

  // Match the ')'
  T.consumeClose();
  if (T.getCloseLocation().isInvalid())
    return;

  const char *PrevSpec = nullptr;
  unsigned DiagID;
  if (DS.SetTypeSpecType(DeclSpec::TST_underlyingType, StartLoc, PrevSpec,
                         DiagID, Result.get(),
                         Actions.getASTContext().getPrintingPolicy()))
    Diag(StartLoc, DiagID) << PrevSpec;
  DS.setTypeofParensRange(T.getRange());
}

/// ParseBaseTypeSpecifier - Parse a C++ base-type-specifier which is either a
/// class name or decltype-specifier. Note that we only check that the result 
/// names a type; semantic analysis will need to verify that the type names a 
/// class. The result is either a type or null, depending on whether a type 
/// name was found.
///
///       base-type-specifier: [C++11 class.derived]
///         class-or-decltype
///       class-or-decltype: [C++11 class.derived]
///         nested-name-specifier[opt] class-name
///         decltype-specifier
///       class-name: [C++ class.name]
///         identifier
///         simple-template-id
///
/// In C++98, instead of base-type-specifier, we have:
///
///         ::[opt] nested-name-specifier[opt] class-name
Parser::TypeResult Parser::ParseBaseTypeSpecifier(SourceLocation &BaseLoc,
                                                  SourceLocation &EndLocation) {
  // Ignore attempts to use typename
  if (Tok.is(tok::kw_typename)) {
    Diag(Tok, diag::err_expected_class_name_not_template)
      << FixItHint::CreateRemoval(Tok.getLocation());
    ConsumeToken();
  }

  // Parse optional nested-name-specifier
  CXXScopeSpec SS;
  ParseOptionalCXXScopeSpecifier(SS, ParsedType(), /*EnteringContext=*/false);

  BaseLoc = Tok.getLocation();

  // Parse decltype-specifier
  // tok == kw_decltype is just error recovery, it can only happen when SS 
  // isn't empty
  if (Tok.is(tok::kw_decltype) || Tok.is(tok::annot_decltype)) {
    if (SS.isNotEmpty())
      Diag(SS.getBeginLoc(), diag::err_unexpected_scope_on_base_decltype)
        << FixItHint::CreateRemoval(SS.getRange());
    // Fake up a Declarator to use with ActOnTypeName.
    DeclSpec DS(AttrFactory);

    EndLocation = ParseDecltypeSpecifier(DS);

    Declarator DeclaratorInfo(DS, Declarator::TypeNameContext);
    return Actions.ActOnTypeName(getCurScope(), DeclaratorInfo);
  }

  // Check whether we have a template-id that names a type.
  if (Tok.is(tok::annot_template_id)) {
    TemplateIdAnnotation *TemplateId = takeTemplateIdAnnotation(Tok);
    if (TemplateId->Kind == TNK_Type_template ||
        TemplateId->Kind == TNK_Dependent_template_name) {
      AnnotateTemplateIdTokenAsType();

      assert(Tok.is(tok::annot_typename) && "template-id -> type failed");
      ParsedType Type = getTypeAnnotation(Tok);
      EndLocation = Tok.getAnnotationEndLoc();
      ConsumeToken();

      if (Type)
        return Type;
      return true;
    }

    // Fall through to produce an error below.
  }

  if (Tok.isNot(tok::identifier)) {
    Diag(Tok, diag::err_expected_class_name);
    return true;
  }

  IdentifierInfo *Id = Tok.getIdentifierInfo();
  SourceLocation IdLoc = ConsumeToken();

  if (Tok.is(tok::less)) {
    // It looks the user intended to write a template-id here, but the
    // template-name was wrong. Try to fix that.
    TemplateNameKind TNK = TNK_Type_template;
    TemplateTy Template;
    if (!Actions.DiagnoseUnknownTemplateName(*Id, IdLoc, getCurScope(),
                                             &SS, Template, TNK)) {
      Diag(IdLoc, diag::err_unknown_template_name)
        << Id;
    }

    if (!Template) {
      TemplateArgList TemplateArgs;
      SourceLocation LAngleLoc, RAngleLoc;
      ParseTemplateIdAfterTemplateName(TemplateTy(), IdLoc, SS,
          true, LAngleLoc, TemplateArgs, RAngleLoc);
      return true;
    }

    // Form the template name
    UnqualifiedId TemplateName;
    TemplateName.setIdentifier(Id, IdLoc);

    // Parse the full template-id, then turn it into a type.
    if (AnnotateTemplateIdToken(Template, TNK, SS, SourceLocation(),
                                TemplateName, true))
      return true;
    if (TNK == TNK_Dependent_template_name)
      AnnotateTemplateIdTokenAsType();

    // If we didn't end up with a typename token, there's nothing more we
    // can do.
    if (Tok.isNot(tok::annot_typename))
      return true;

    // Retrieve the type from the annotation token, consume that token, and
    // return.
    EndLocation = Tok.getAnnotationEndLoc();
    ParsedType Type = getTypeAnnotation(Tok);
    ConsumeToken();
    return Type;
  }

  // We have an identifier; check whether it is actually a type.
  IdentifierInfo *CorrectedII = nullptr;
  ParsedType Type = Actions.getTypeName(*Id, IdLoc, getCurScope(), &SS, true,
                                        false, ParsedType(),
                                        /*IsCtorOrDtorName=*/false,
                                        /*NonTrivialTypeSourceInfo=*/true,
                                        &CorrectedII);
  if (!Type) {
    Diag(IdLoc, diag::err_expected_class_name);
    return true;
  }

  // Consume the identifier.
  EndLocation = IdLoc;

  // Fake up a Declarator to use with ActOnTypeName.
  DeclSpec DS(AttrFactory);
  DS.SetRangeStart(IdLoc);
  DS.SetRangeEnd(EndLocation);
  DS.getTypeSpecScope() = SS;

  const char *PrevSpec = nullptr;
  unsigned DiagID;
  DS.SetTypeSpecType(TST_typename, IdLoc, PrevSpec, DiagID, Type,
                     Actions.getASTContext().getPrintingPolicy());

  Declarator DeclaratorInfo(DS, Declarator::TypeNameContext);
  return Actions.ActOnTypeName(getCurScope(), DeclaratorInfo);
}

void Parser::ParseMicrosoftInheritanceClassAttributes(ParsedAttributes &attrs) {
  while (Tok.is(tok::kw___single_inheritance) ||
         Tok.is(tok::kw___multiple_inheritance) ||
         Tok.is(tok::kw___virtual_inheritance)) {
    IdentifierInfo *AttrName = Tok.getIdentifierInfo();
    SourceLocation AttrNameLoc = ConsumeToken();
    attrs.addNew(AttrName, AttrNameLoc, nullptr, AttrNameLoc, nullptr, 0,
                 AttributeList::AS_Keyword);
  }
}

/// Determine whether the following tokens are valid after a type-specifier
/// which could be a standalone declaration. This will conservatively return
/// true if there's any doubt, and is appropriate for insert-';' fixits.
bool Parser::isValidAfterTypeSpecifier(bool CouldBeBitfield) {
  // This switch enumerates the valid "follow" set for type-specifiers.
  switch (Tok.getKind()) {
  default: break;
  case tok::semi:               // struct foo {...} ;
  case tok::star:               // struct foo {...} *         P;
  case tok::amp:                // struct foo {...} &         R = ...
  case tok::ampamp:             // struct foo {...} &&        R = ...
  case tok::identifier:         // struct foo {...} V         ;
  case tok::r_paren:            //(struct foo {...} )         {4}
  case tok::annot_cxxscope:     // struct foo {...} a::       b;
  case tok::annot_typename:     // struct foo {...} a         ::b;
  case tok::annot_template_id:  // struct foo {...} a<int>    ::b;
  case tok::l_paren:            // struct foo {...} (         x);
  case tok::comma:              // __builtin_offsetof(struct foo{...} ,
  case tok::kw_operator:        // struct foo       operator  ++() {...}
  case tok::kw___declspec:      // struct foo {...} __declspec(...)
  case tok::l_square:           // void f(struct f  [         3])
  case tok::ellipsis:           // void f(struct f  ...       [Ns])
    return true;
  case tok::colon:
    return CouldBeBitfield;     // enum E { ... }   :         2;
  // Type qualifiers
  case tok::kw_const:           // struct foo {...} const     x;
  case tok::kw_volatile:        // struct foo {...} volatile  x;
  case tok::kw_restrict:        // struct foo {...} restrict  x;
  case tok::kw__Atomic:         // struct foo {...} _Atomic   x;
  // Function specifiers
  // Note, no 'explicit'. An explicit function must be either a conversion
  // operator or a constructor. Either way, it can't have a return type.
  case tok::kw_inline:          // struct foo       inline    f();
  case tok::kw_virtual:         // struct foo       virtual   f();
  case tok::kw_friend:          // struct foo       friend    f();
  // Storage-class specifiers
  case tok::kw_static:          // struct foo {...} static    x;
  case tok::kw_extern:          // struct foo {...} extern    x;
  case tok::kw_typedef:         // struct foo {...} typedef   x;
  case tok::kw_register:        // struct foo {...} register  x;
  case tok::kw_auto:            // struct foo {...} auto      x;
  case tok::kw_mutable:         // struct foo {...} mutable   x;
  case tok::kw_thread_local:    // struct foo {...} thread_local x;
  case tok::kw_constexpr:       // struct foo {...} constexpr x;
    // As shown above, type qualifiers and storage class specifiers absolutely
    // can occur after class specifiers according to the grammar.  However,
    // almost no one actually writes code like this.  If we see one of these,
    // it is much more likely that someone missed a semi colon and the
    // type/storage class specifier we're seeing is part of the *next*
    // intended declaration, as in:
    //
    //   struct foo { ... }
    //   typedef int X;
    //
    // We'd really like to emit a missing semicolon error instead of emitting
    // an error on the 'int' saying that you can't have two type specifiers in
    // the same declaration of X.  Because of this, we look ahead past this
    // token to see if it's a type specifier.  If so, we know the code is
    // otherwise invalid, so we can produce the expected semi error.
    if (!isKnownToBeTypeSpecifier(NextToken()))
      return true;
    break;
  case tok::r_brace:  // struct bar { struct foo {...} }
    // Missing ';' at end of struct is accepted as an extension in C mode.
    if (!getLangOpts().CPlusPlus)
      return true;
    break;
  case tok::greater:
    // template<class T = class X>
    return getLangOpts().CPlusPlus;
  }
  return false;
}

/// ParseClassSpecifier - Parse a C++ class-specifier [C++ class] or
/// elaborated-type-specifier [C++ dcl.type.elab]; we can't tell which
/// until we reach the start of a definition or see a token that
/// cannot start a definition.
///
///       class-specifier: [C++ class]
///         class-head '{' member-specification[opt] '}'
///         class-head '{' member-specification[opt] '}' attributes[opt]
///       class-head:
///         class-key identifier[opt] base-clause[opt]
///         class-key nested-name-specifier identifier base-clause[opt]
///         class-key nested-name-specifier[opt] simple-template-id
///                          base-clause[opt]
/// [GNU]   class-key attributes[opt] identifier[opt] base-clause[opt]
/// [GNU]   class-key attributes[opt] nested-name-specifier
///                          identifier base-clause[opt]
/// [GNU]   class-key attributes[opt] nested-name-specifier[opt]
///                          simple-template-id base-clause[opt]
///       class-key:
///         'class'
///         'struct'
///         'union'
///
///       elaborated-type-specifier: [C++ dcl.type.elab]
///         class-key ::[opt] nested-name-specifier[opt] identifier
///         class-key ::[opt] nested-name-specifier[opt] 'template'[opt]
///                          simple-template-id
///
///  Note that the C++ class-specifier and elaborated-type-specifier,
///  together, subsume the C99 struct-or-union-specifier:
///
///       struct-or-union-specifier: [C99 6.7.2.1]
///         struct-or-union identifier[opt] '{' struct-contents '}'
///         struct-or-union identifier
/// [GNU]   struct-or-union attributes[opt] identifier[opt] '{' struct-contents
///                                                         '}' attributes[opt]
/// [GNU]   struct-or-union attributes[opt] identifier
///       struct-or-union:
///         'struct'
///         'union'
void Parser::ParseClassSpecifier(tok::TokenKind TagTokKind,
                                 SourceLocation StartLoc, DeclSpec &DS,
                                 const ParsedTemplateInfo &TemplateInfo,
                                 AccessSpecifier AS, 
                                 bool EnteringContext, DeclSpecContext DSC, 
                                 ParsedAttributesWithRange &Attributes) {
  DeclSpec::TST TagType;
  if (TagTokKind == tok::kw_struct)
    TagType = DeclSpec::TST_struct;
  else if (TagTokKind == tok::kw___interface)
    TagType = DeclSpec::TST_interface;
  else if (TagTokKind == tok::kw_class)
    TagType = DeclSpec::TST_class;
  else {
    assert(TagTokKind == tok::kw_union && "Not a class specifier");
    TagType = DeclSpec::TST_union;
  }

  if (Tok.is(tok::code_completion)) {
    // Code completion for a struct, class, or union name.
    Actions.CodeCompleteTag(getCurScope(), TagType);
    return cutOffParsing();
  }

  // C++03 [temp.explicit] 14.7.2/8:
  //   The usual access checking rules do not apply to names used to specify
  //   explicit instantiations.
  //
  // As an extension we do not perform access checking on the names used to
  // specify explicit specializations either. This is important to allow
  // specializing traits classes for private types.
  //
  // Note that we don't suppress if this turns out to be an elaborated
  // type specifier.
  bool shouldDelayDiagsInTag =
    (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation ||
     TemplateInfo.Kind == ParsedTemplateInfo::ExplicitSpecialization);
  SuppressAccessChecks diagsFromTag(*this, shouldDelayDiagsInTag);

  ParsedAttributesWithRange attrs(AttrFactory);
  // If attributes exist after tag, parse them.
  MaybeParseGNUAttributes(attrs);

  // If declspecs exist after tag, parse them.
  while (Tok.is(tok::kw___declspec))
    ParseMicrosoftDeclSpec(attrs);

  // Parse inheritance specifiers.
  if (Tok.is(tok::kw___single_inheritance) ||
      Tok.is(tok::kw___multiple_inheritance) ||
      Tok.is(tok::kw___virtual_inheritance))
    ParseMicrosoftInheritanceClassAttributes(attrs);

  // If C++0x attributes exist here, parse them.
  // FIXME: Are we consistent with the ordering of parsing of different
  // styles of attributes?
  MaybeParseCXX11Attributes(attrs);

  // Source location used by FIXIT to insert misplaced
  // C++11 attributes
  SourceLocation AttrFixitLoc = Tok.getLocation();

  // GNU libstdc++ and libc++ use certain intrinsic names as the
  // name of struct templates, but some are keywords in GCC >= 4.3
  // MSVC and Clang. For compatibility, convert the token to an identifier
  // and issue a warning diagnostic.
  if (TagType == DeclSpec::TST_struct && !Tok.is(tok::identifier) &&
      !Tok.isAnnotation()) {
    const IdentifierInfo *II = Tok.getIdentifierInfo();
    // We rarely end up here so the following check is efficient.
    if (II && II->getName().startswith("__is_"))
      TryKeywordIdentFallback(true);
  }

  // Parse the (optional) nested-name-specifier.
  CXXScopeSpec &SS = DS.getTypeSpecScope();
  if (getLangOpts().CPlusPlus) {
    // "FOO : BAR" is not a potential typo for "FOO::BAR".  In this context it
    // is a base-specifier-list.
    ColonProtectionRAIIObject X(*this);

    if (ParseOptionalCXXScopeSpecifier(SS, ParsedType(), EnteringContext))
      DS.SetTypeSpecError();
    if (SS.isSet())
      if (Tok.isNot(tok::identifier) && Tok.isNot(tok::annot_template_id))
        Diag(Tok, diag::err_expected) << tok::identifier;
  }

  TemplateParameterLists *TemplateParams = TemplateInfo.TemplateParams;

  // Parse the (optional) class name or simple-template-id.
  IdentifierInfo *Name = nullptr;
  SourceLocation NameLoc;
  TemplateIdAnnotation *TemplateId = nullptr;
  if (Tok.is(tok::identifier)) {
    Name = Tok.getIdentifierInfo();
    NameLoc = ConsumeToken();

    if (Tok.is(tok::less) && getLangOpts().CPlusPlus) {
      // The name was supposed to refer to a template, but didn't.
      // Eat the template argument list and try to continue parsing this as
      // a class (or template thereof).
      TemplateArgList TemplateArgs;
      SourceLocation LAngleLoc, RAngleLoc;
      if (ParseTemplateIdAfterTemplateName(TemplateTy(), NameLoc, SS,
                                           true, LAngleLoc,
                                           TemplateArgs, RAngleLoc)) {
        // We couldn't parse the template argument list at all, so don't
        // try to give any location information for the list.
        LAngleLoc = RAngleLoc = SourceLocation();
      }

      Diag(NameLoc, diag::err_explicit_spec_non_template)
          << (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation)
          << TagTokKind << Name << SourceRange(LAngleLoc, RAngleLoc);

      // Strip off the last template parameter list if it was empty, since
      // we've removed its template argument list.
      if (TemplateParams && TemplateInfo.LastParameterListWasEmpty) {
        if (TemplateParams && TemplateParams->size() > 1) {
          TemplateParams->pop_back();
        } else {
          TemplateParams = nullptr;
          const_cast<ParsedTemplateInfo&>(TemplateInfo).Kind
            = ParsedTemplateInfo::NonTemplate;
        }
      } else if (TemplateInfo.Kind
                                == ParsedTemplateInfo::ExplicitInstantiation) {
        // Pretend this is just a forward declaration.
        TemplateParams = nullptr;
        const_cast<ParsedTemplateInfo&>(TemplateInfo).Kind
          = ParsedTemplateInfo::NonTemplate;
        const_cast<ParsedTemplateInfo&>(TemplateInfo).TemplateLoc
          = SourceLocation();
        const_cast<ParsedTemplateInfo&>(TemplateInfo).ExternLoc
          = SourceLocation();
      }
    }
  } else if (Tok.is(tok::annot_template_id)) {
    TemplateId = takeTemplateIdAnnotation(Tok);
    NameLoc = ConsumeToken();

    if (TemplateId->Kind != TNK_Type_template &&
        TemplateId->Kind != TNK_Dependent_template_name) {
      // The template-name in the simple-template-id refers to
      // something other than a class template. Give an appropriate
      // error message and skip to the ';'.
      SourceRange Range(NameLoc);
      if (SS.isNotEmpty())
        Range.setBegin(SS.getBeginLoc());

      // FIXME: Name may be null here.
      Diag(TemplateId->LAngleLoc, diag::err_template_spec_syntax_non_template)
        << TemplateId->Name << static_cast<int>(TemplateId->Kind) << Range;

      DS.SetTypeSpecError();
      SkipUntil(tok::semi, StopBeforeMatch);
      return;
    }
  }

  // There are four options here.
  //  - If we are in a trailing return type, this is always just a reference,
  //    and we must not try to parse a definition. For instance,
  //      [] () -> struct S { };
  //    does not define a type.
  //  - If we have 'struct foo {...', 'struct foo :...',
  //    'struct foo final :' or 'struct foo final {', then this is a definition.
  //  - If we have 'struct foo;', then this is either a forward declaration
  //    or a friend declaration, which have to be treated differently.
  //  - Otherwise we have something like 'struct foo xyz', a reference.
  //
  //  We also detect these erroneous cases to provide better diagnostic for
  //  C++11 attributes parsing.
  //  - attributes follow class name:
  //    struct foo [[]] {};
  //  - attributes appear before or after 'final':
  //    struct foo [[]] final [[]] {};
  //
  // However, in type-specifier-seq's, things look like declarations but are
  // just references, e.g.
  //   new struct s;
  // or
  //   &T::operator struct s;
  // For these, DSC is DSC_type_specifier or DSC_alias_declaration.

  // If there are attributes after class name, parse them.
  MaybeParseCXX11Attributes(Attributes);

  const PrintingPolicy &Policy = Actions.getASTContext().getPrintingPolicy();
  Sema::TagUseKind TUK;
  if (DSC == DSC_trailing)
    TUK = Sema::TUK_Reference;
  else if (Tok.is(tok::l_brace) ||
           (getLangOpts().CPlusPlus && Tok.is(tok::colon)) ||
           (isCXX11FinalKeyword() &&
            (NextToken().is(tok::l_brace) || NextToken().is(tok::colon)))) {
    if (DS.isFriendSpecified()) {
      // C++ [class.friend]p2:
      //   A class shall not be defined in a friend declaration.
      Diag(Tok.getLocation(), diag::err_friend_decl_defines_type)
        << SourceRange(DS.getFriendSpecLoc());

      // Skip everything up to the semicolon, so that this looks like a proper
      // friend class (or template thereof) declaration.
      SkipUntil(tok::semi, StopBeforeMatch);
      TUK = Sema::TUK_Friend;
    } else {
      // Okay, this is a class definition.
      TUK = Sema::TUK_Definition;
    }
  } else if (isCXX11FinalKeyword() && (NextToken().is(tok::l_square) ||
                                       NextToken().is(tok::kw_alignas))) {
    // We can't tell if this is a definition or reference
    // until we skipped the 'final' and C++11 attribute specifiers.
    TentativeParsingAction PA(*this);

    // Skip the 'final' keyword.
    ConsumeToken();

    // Skip C++11 attribute specifiers.
    while (true) {
      if (Tok.is(tok::l_square) && NextToken().is(tok::l_square)) {
        ConsumeBracket();
        if (!SkipUntil(tok::r_square, StopAtSemi))
          break;
      } else if (Tok.is(tok::kw_alignas) && NextToken().is(tok::l_paren)) {
        ConsumeToken();
        ConsumeParen();
        if (!SkipUntil(tok::r_paren, StopAtSemi))
          break;
      } else {
        break;
      }
    }

    if (Tok.is(tok::l_brace) || Tok.is(tok::colon))
      TUK = Sema::TUK_Definition;
    else
      TUK = Sema::TUK_Reference;

    PA.Revert();
  } else if (!isTypeSpecifier(DSC) &&
             (Tok.is(tok::semi) ||
              (Tok.isAtStartOfLine() && !isValidAfterTypeSpecifier(false)))) {
    TUK = DS.isFriendSpecified() ? Sema::TUK_Friend : Sema::TUK_Declaration;
    if (Tok.isNot(tok::semi)) {
      const PrintingPolicy &PPol = Actions.getASTContext().getPrintingPolicy();
      // A semicolon was missing after this declaration. Diagnose and recover.
      ExpectAndConsume(tok::semi, diag::err_expected_after,
                       DeclSpec::getSpecifierName(TagType, PPol));
      PP.EnterToken(Tok);
      Tok.setKind(tok::semi);
    }
  } else
    TUK = Sema::TUK_Reference;

  // Forbid misplaced attributes. In cases of a reference, we pass attributes
  // to caller to handle.
  if (TUK != Sema::TUK_Reference) {
    // If this is not a reference, then the only possible
    // valid place for C++11 attributes to appear here
    // is between class-key and class-name. If there are
    // any attributes after class-name, we try a fixit to move
    // them to the right place.
    SourceRange AttrRange = Attributes.Range;
    if (AttrRange.isValid()) {
      Diag(AttrRange.getBegin(), diag::err_attributes_not_allowed)
        << AttrRange
        << FixItHint::CreateInsertionFromRange(AttrFixitLoc,
                                               CharSourceRange(AttrRange, true))
        << FixItHint::CreateRemoval(AttrRange);

      // Recover by adding misplaced attributes to the attribute list
      // of the class so they can be applied on the class later.
      attrs.takeAllFrom(Attributes);
    }
  }

  // If this is an elaborated type specifier, and we delayed
  // diagnostics before, just merge them into the current pool.
  if (shouldDelayDiagsInTag) {
    diagsFromTag.done();
    if (TUK == Sema::TUK_Reference)
      diagsFromTag.redelay();
  }

  if (!Name && !TemplateId && (DS.getTypeSpecType() == DeclSpec::TST_error ||
                               TUK != Sema::TUK_Definition)) {
    if (DS.getTypeSpecType() != DeclSpec::TST_error) {
      // We have a declaration or reference to an anonymous class.
      Diag(StartLoc, diag::err_anon_type_definition)
        << DeclSpec::getSpecifierName(TagType, Policy);
    }

    // If we are parsing a definition and stop at a base-clause, continue on
    // until the semicolon.  Continuing from the comma will just trick us into
    // thinking we are seeing a variable declaration.
    if (TUK == Sema::TUK_Definition && Tok.is(tok::colon))
      SkipUntil(tok::semi, StopBeforeMatch);
    else
      SkipUntil(tok::comma, StopAtSemi);
    return;
  }

  // Create the tag portion of the class or class template.
  DeclResult TagOrTempResult = true; // invalid
  TypeResult TypeResult = true; // invalid

  bool Owned = false;
  if (TemplateId) {
    // Explicit specialization, class template partial specialization,
    // or explicit instantiation.
    ASTTemplateArgsPtr TemplateArgsPtr(TemplateId->getTemplateArgs(),
                                       TemplateId->NumArgs);
    if (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation &&
        TUK == Sema::TUK_Declaration) {
      // This is an explicit instantiation of a class template.
      ProhibitAttributes(attrs);

      TagOrTempResult
        = Actions.ActOnExplicitInstantiation(getCurScope(),
                                             TemplateInfo.ExternLoc,
                                             TemplateInfo.TemplateLoc,
                                             TagType,
                                             StartLoc,
                                             SS,
                                             TemplateId->Template,
                                             TemplateId->TemplateNameLoc,
                                             TemplateId->LAngleLoc,
                                             TemplateArgsPtr,
                                             TemplateId->RAngleLoc,
                                             attrs.getList());

    // Friend template-ids are treated as references unless
    // they have template headers, in which case they're ill-formed
    // (FIXME: "template <class T> friend class A<T>::B<int>;").
    // We diagnose this error in ActOnClassTemplateSpecialization.
    } else if (TUK == Sema::TUK_Reference ||
               (TUK == Sema::TUK_Friend &&
                TemplateInfo.Kind == ParsedTemplateInfo::NonTemplate)) {
      ProhibitAttributes(attrs);
      TypeResult = Actions.ActOnTagTemplateIdType(TUK, TagType, StartLoc,
                                                  TemplateId->SS,
                                                  TemplateId->TemplateKWLoc,
                                                  TemplateId->Template,
                                                  TemplateId->TemplateNameLoc,
                                                  TemplateId->LAngleLoc,
                                                  TemplateArgsPtr,
                                                  TemplateId->RAngleLoc);
    } else {
      // This is an explicit specialization or a class template
      // partial specialization.
      TemplateParameterLists FakedParamLists;
      if (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation) {
        // This looks like an explicit instantiation, because we have
        // something like
        //
        //   template class Foo<X>
        //
        // but it actually has a definition. Most likely, this was
        // meant to be an explicit specialization, but the user forgot
        // the '<>' after 'template'.
        // It this is friend declaration however, since it cannot have a
        // template header, it is most likely that the user meant to
        // remove the 'template' keyword.
        assert((TUK == Sema::TUK_Definition || TUK == Sema::TUK_Friend) &&
               "Expected a definition here");

        if (TUK == Sema::TUK_Friend) {
          Diag(DS.getFriendSpecLoc(), diag::err_friend_explicit_instantiation);
          TemplateParams = nullptr;
        } else {
          SourceLocation LAngleLoc =
              PP.getLocForEndOfToken(TemplateInfo.TemplateLoc);
          Diag(TemplateId->TemplateNameLoc,
               diag::err_explicit_instantiation_with_definition)
              << SourceRange(TemplateInfo.TemplateLoc)
              << FixItHint::CreateInsertion(LAngleLoc, "<>");

          // Create a fake template parameter list that contains only
          // "template<>", so that we treat this construct as a class
          // template specialization.
          FakedParamLists.push_back(Actions.ActOnTemplateParameterList(
              0, SourceLocation(), TemplateInfo.TemplateLoc, LAngleLoc, nullptr,
              0, LAngleLoc));
          TemplateParams = &FakedParamLists;
        }
      }

      // Build the class template specialization.
      TagOrTempResult = Actions.ActOnClassTemplateSpecialization(
          getCurScope(), TagType, TUK, StartLoc, DS.getModulePrivateSpecLoc(),
          *TemplateId, attrs.getList(),
          MultiTemplateParamsArg(TemplateParams ? &(*TemplateParams)[0]
                                                : nullptr,
                                 TemplateParams ? TemplateParams->size() : 0));
    }
  } else if (TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation &&
             TUK == Sema::TUK_Declaration) {
    // Explicit instantiation of a member of a class template
    // specialization, e.g.,
    //
    //   template struct Outer<int>::Inner;
    //
    ProhibitAttributes(attrs);

    TagOrTempResult
      = Actions.ActOnExplicitInstantiation(getCurScope(),
                                           TemplateInfo.ExternLoc,
                                           TemplateInfo.TemplateLoc,
                                           TagType, StartLoc, SS, Name,
                                           NameLoc, attrs.getList());
  } else if (TUK == Sema::TUK_Friend &&
             TemplateInfo.Kind != ParsedTemplateInfo::NonTemplate) {
    ProhibitAttributes(attrs);

    TagOrTempResult =
      Actions.ActOnTemplatedFriendTag(getCurScope(), DS.getFriendSpecLoc(),
                                      TagType, StartLoc, SS,
                                      Name, NameLoc, attrs.getList(),
                                      MultiTemplateParamsArg(
                                    TemplateParams? &(*TemplateParams)[0]
                                                  : nullptr,
                                 TemplateParams? TemplateParams->size() : 0));
  } else {
    if (TUK != Sema::TUK_Declaration && TUK != Sema::TUK_Definition)
      ProhibitAttributes(attrs);

    if (TUK == Sema::TUK_Definition &&
        TemplateInfo.Kind == ParsedTemplateInfo::ExplicitInstantiation) {
      // If the declarator-id is not a template-id, issue a diagnostic and
      // recover by ignoring the 'template' keyword.
      Diag(Tok, diag::err_template_defn_explicit_instantiation)
        << 1 << FixItHint::CreateRemoval(TemplateInfo.TemplateLoc);
      TemplateParams = nullptr;
    }

    bool IsDependent = false;

    // Don't pass down template parameter lists if this is just a tag
    // reference.  For example, we don't need the template parameters here:
    //   template <class T> class A *makeA(T t);
    MultiTemplateParamsArg TParams;
    if (TUK != Sema::TUK_Reference && TemplateParams)
      TParams =
        MultiTemplateParamsArg(&(*TemplateParams)[0], TemplateParams->size());

    // Declaration or definition of a class type
    TagOrTempResult = Actions.ActOnTag(getCurScope(), TagType, TUK, StartLoc,
                                       SS, Name, NameLoc, attrs.getList(), AS,
                                       DS.getModulePrivateSpecLoc(),
                                       TParams, Owned, IsDependent,
                                       SourceLocation(), false,
                                       clang::TypeResult(),
                                       DSC == DSC_type_specifier);

    // If ActOnTag said the type was dependent, try again with the
    // less common call.
    if (IsDependent) {
      assert(TUK == Sema::TUK_Reference || TUK == Sema::TUK_Friend);
      TypeResult = Actions.ActOnDependentTag(getCurScope(), TagType, TUK,
                                             SS, Name, StartLoc, NameLoc);
    }
  }

  // If there is a body, parse it and inform the actions module.
  if (TUK == Sema::TUK_Definition) {
    assert(Tok.is(tok::l_brace) ||
           (getLangOpts().CPlusPlus && Tok.is(tok::colon)) ||
           isCXX11FinalKeyword());
    if (getLangOpts().CPlusPlus)
      ParseCXXMemberSpecification(StartLoc, AttrFixitLoc, attrs, TagType,
                                  TagOrTempResult.get());
    else
      ParseStructUnionBody(StartLoc, TagType, TagOrTempResult.get());
  }

  const char *PrevSpec = nullptr;
  unsigned DiagID;
  bool Result;
  if (!TypeResult.isInvalid()) {
    Result = DS.SetTypeSpecType(DeclSpec::TST_typename, StartLoc,
                                NameLoc.isValid() ? NameLoc : StartLoc,
                                PrevSpec, DiagID, TypeResult.get(), Policy);
  } else if (!TagOrTempResult.isInvalid()) {
    Result = DS.SetTypeSpecType(TagType, StartLoc,
                                NameLoc.isValid() ? NameLoc : StartLoc,
                                PrevSpec, DiagID, TagOrTempResult.get(), Owned,
                                Policy);
  } else {
    DS.SetTypeSpecError();
    return;
  }

  if (Result)
    Diag(StartLoc, DiagID) << PrevSpec;

  // At this point, we've successfully parsed a class-specifier in 'definition'
  // form (e.g. "struct foo { int x; }".  While we could just return here, we're
  // going to look at what comes after it to improve error recovery.  If an
  // impossible token occurs next, we assume that the programmer forgot a ; at
  // the end of the declaration and recover that way.
  //
  // Also enforce C++ [temp]p3:
  //   In a template-declaration which defines a class, no declarator
  //   is permitted.
  //
  // After a type-specifier, we don't expect a semicolon. This only happens in
  // C, since definitions are not permitted in this context in C++.
  if (TUK == Sema::TUK_Definition &&
      (getLangOpts().CPlusPlus || !isTypeSpecifier(DSC)) &&
      (TemplateInfo.Kind || !isValidAfterTypeSpecifier(false))) {
    if (Tok.isNot(tok::semi)) {
      const PrintingPolicy &PPol = Actions.getASTContext().getPrintingPolicy();
      ExpectAndConsume(tok::semi, diag::err_expected_after,
                       DeclSpec::getSpecifierName(TagType, PPol));
      // Push this token back into the preprocessor and change our current token
      // to ';' so that the rest of the code recovers as though there were an
      // ';' after the definition.
      PP.EnterToken(Tok);
      Tok.setKind(tok::semi);
    }
  }
}

/// ParseBaseClause - Parse the base-clause of a C++ class [C++ class.derived].
///
///       base-clause : [C++ class.derived]
///         ':' base-specifier-list
///       base-specifier-list:
///         base-specifier '...'[opt]
///         base-specifier-list ',' base-specifier '...'[opt]
void Parser::ParseBaseClause(Decl *ClassDecl) {
  assert(Tok.is(tok::colon) && "Not a base clause");
  ConsumeToken();

  // Build up an array of parsed base specifiers.
  SmallVector<CXXBaseSpecifier *, 8> BaseInfo;

  while (true) {
    // Parse a base-specifier.
    BaseResult Result = ParseBaseSpecifier(ClassDecl);
    if (Result.isInvalid()) {
      // Skip the rest of this base specifier, up until the comma or
      // opening brace.
      SkipUntil(tok::comma, tok::l_brace, StopAtSemi | StopBeforeMatch);
    } else {
      // Add this to our array of base specifiers.
      BaseInfo.push_back(Result.get());
    }

    // If the next token is a comma, consume it and keep reading
    // base-specifiers.
    if (!TryConsumeToken(tok::comma))
      break;
  }

  // Attach the base specifiers
  Actions.ActOnBaseSpecifiers(ClassDecl, BaseInfo.data(), BaseInfo.size());
}

/// ParseBaseSpecifier - Parse a C++ base-specifier. A base-specifier is
/// one entry in the base class list of a class specifier, for example:
///    class foo : public bar, virtual private baz {
/// 'public bar' and 'virtual private baz' are each base-specifiers.
///
///       base-specifier: [C++ class.derived]
///         attribute-specifier-seq[opt] base-type-specifier
///         attribute-specifier-seq[opt] 'virtual' access-specifier[opt]
///                 base-type-specifier
///         attribute-specifier-seq[opt] access-specifier 'virtual'[opt]
///                 base-type-specifier
Parser::BaseResult Parser::ParseBaseSpecifier(Decl *ClassDecl) {
  bool IsVirtual = false;
  SourceLocation StartLoc = Tok.getLocation();

  ParsedAttributesWithRange Attributes(AttrFactory);
  MaybeParseCXX11Attributes(Attributes);

  // Parse the 'virtual' keyword.
  if (TryConsumeToken(tok::kw_virtual))
    IsVirtual = true;

  CheckMisplacedCXX11Attribute(Attributes, StartLoc);

  // Parse an (optional) access specifier.
  AccessSpecifier Access = getAccessSpecifierIfPresent();
  if (Access != AS_none)
    ConsumeToken();

  CheckMisplacedCXX11Attribute(Attributes, StartLoc);

  // Parse the 'virtual' keyword (again!), in case it came after the
  // access specifier.
  if (Tok.is(tok::kw_virtual))  {
    SourceLocation VirtualLoc = ConsumeToken();
    if (IsVirtual) {
      // Complain about duplicate 'virtual'
      Diag(VirtualLoc, diag::err_dup_virtual)
        << FixItHint::CreateRemoval(VirtualLoc);
    }

    IsVirtual = true;
  }

  CheckMisplacedCXX11Attribute(Attributes, StartLoc);

  // Parse the class-name.
  SourceLocation EndLocation;
  SourceLocation BaseLoc;
  TypeResult BaseType = ParseBaseTypeSpecifier(BaseLoc, EndLocation);
  if (BaseType.isInvalid())
    return true;

  // Parse the optional ellipsis (for a pack expansion). The ellipsis is 
  // actually part of the base-specifier-list grammar productions, but we
  // parse it here for convenience.
  SourceLocation EllipsisLoc;
  TryConsumeToken(tok::ellipsis, EllipsisLoc);

  // Find the complete source range for the base-specifier.
  SourceRange Range(StartLoc, EndLocation);

  // Notify semantic analysis that we have parsed a complete
  // base-specifier.
  return Actions.ActOnBaseSpecifier(ClassDecl, Range, Attributes, IsVirtual,
                                    Access, BaseType.get(), BaseLoc,
                                    EllipsisLoc);
}

/// getAccessSpecifierIfPresent - Determine whether the next token is
/// a C++ access-specifier.
///
///       access-specifier: [C++ class.derived]
///         'private'
///         'protected'
///         'public'
AccessSpecifier Parser::getAccessSpecifierIfPresent() const {
  switch (Tok.getKind()) {
  default: return AS_none;
  case tok::kw_private: return AS_private;
  case tok::kw_protected: return AS_protected;
  case tok::kw_public: return AS_public;
  }
}

/// \brief If the given declarator has any parts for which parsing has to be
/// delayed, e.g., default arguments, create a late-parsed method declaration
/// record to handle the parsing at the end of the class definition.
void Parser::HandleMemberFunctionDeclDelays(Declarator& DeclaratorInfo,
                                            Decl *ThisDecl) {
  // We just declared a member function. If this member function
  // has any default arguments, we'll need to parse them later.
  LateParsedMethodDeclaration *LateMethod = nullptr;
  DeclaratorChunk::FunctionTypeInfo &FTI
    = DeclaratorInfo.getFunctionTypeInfo();

  for (unsigned ParamIdx = 0; ParamIdx < FTI.NumParams; ++ParamIdx) {
    if (LateMethod || FTI.Params[ParamIdx].DefaultArgTokens) {
      if (!LateMethod) {
        // Push this method onto the stack of late-parsed method
        // declarations.
        LateMethod = new LateParsedMethodDeclaration(this, ThisDecl);
        getCurrentClass().LateParsedDeclarations.push_back(LateMethod);
        LateMethod->TemplateScope = getCurScope()->isTemplateParamScope();

        // Add all of the parameters prior to this one (they don't
        // have default arguments).
        LateMethod->DefaultArgs.reserve(FTI.NumParams);
        for (unsigned I = 0; I < ParamIdx; ++I)
          LateMethod->DefaultArgs.push_back(
              LateParsedDefaultArgument(FTI.Params[I].Param));
      }

      // Add this parameter to the list of parameters (it may or may
      // not have a default argument).
      LateMethod->DefaultArgs.push_back(LateParsedDefaultArgument(
          FTI.Params[ParamIdx].Param, FTI.Params[ParamIdx].DefaultArgTokens));
    }
  }
}

/// isCXX11VirtSpecifier - Determine whether the given token is a C++11
/// virt-specifier.
///
///       virt-specifier:
///         override
///         final
VirtSpecifiers::Specifier Parser::isCXX11VirtSpecifier(const Token &Tok) const {
  if (!getLangOpts().CPlusPlus || Tok.isNot(tok::identifier))
    return VirtSpecifiers::VS_None;

  IdentifierInfo *II = Tok.getIdentifierInfo();

  // Initialize the contextual keywords.
  if (!Ident_final) {
    Ident_final = &PP.getIdentifierTable().get("final");
    if (getLangOpts().MicrosoftExt)
      Ident_sealed = &PP.getIdentifierTable().get("sealed");
    Ident_override = &PP.getIdentifierTable().get("override");
  }

  if (II == Ident_override)
    return VirtSpecifiers::VS_Override;

  if (II == Ident_sealed)
    return VirtSpecifiers::VS_Sealed;

  if (II == Ident_final)
    return VirtSpecifiers::VS_Final;

  return VirtSpecifiers::VS_None;
}

/// ParseOptionalCXX11VirtSpecifierSeq - Parse a virt-specifier-seq.
///
///       virt-specifier-seq:
///         virt-specifier
///         virt-specifier-seq virt-specifier
void Parser::ParseOptionalCXX11VirtSpecifierSeq(VirtSpecifiers &VS,
                                                bool IsInterface,
                                                SourceLocation FriendLoc) {
  while (true) {
    VirtSpecifiers::Specifier Specifier = isCXX11VirtSpecifier();
    if (Specifier == VirtSpecifiers::VS_None)
      return;

    if (FriendLoc.isValid()) {
      Diag(Tok.getLocation(), diag::err_friend_decl_spec)
        << VirtSpecifiers::getSpecifierName(Specifier)
        << FixItHint::CreateRemoval(Tok.getLocation())
        << SourceRange(FriendLoc, FriendLoc);
      ConsumeToken();
      continue;
    }

    // C++ [class.mem]p8:
    //   A virt-specifier-seq shall contain at most one of each virt-specifier.
    const char *PrevSpec = nullptr;
    if (VS.SetSpecifier(Specifier, Tok.getLocation(), PrevSpec))
      Diag(Tok.getLocation(), diag::err_duplicate_virt_specifier)
        << PrevSpec
        << FixItHint::CreateRemoval(Tok.getLocation());

    if (IsInterface && (Specifier == VirtSpecifiers::VS_Final ||
                        Specifier == VirtSpecifiers::VS_Sealed)) {
      Diag(Tok.getLocation(), diag::err_override_control_interface)
        << VirtSpecifiers::getSpecifierName(Specifier);
    } else if (Specifier == VirtSpecifiers::VS_Sealed) {
      Diag(Tok.getLocation(), diag::ext_ms_sealed_keyword);
    } else {
      Diag(Tok.getLocation(),
           getLangOpts().CPlusPlus11
               ? diag::warn_cxx98_compat_override_control_keyword
               : diag::ext_override_control_keyword)
          << VirtSpecifiers::getSpecifierName(Specifier);
    }
    ConsumeToken();
  }
}

/// isCXX11FinalKeyword - Determine whether the next token is a C++11
/// 'final' or Microsoft 'sealed' contextual keyword.
bool Parser::isCXX11FinalKeyword() const {
  VirtSpecifiers::Specifier Specifier = isCXX11VirtSpecifier();
  return Specifier == VirtSpecifiers::VS_Final ||
         Specifier == VirtSpecifiers::VS_Sealed;
}

/// \brief Parse a C++ member-declarator up to, but not including, the optional
/// brace-or-equal-initializer or pure-specifier.
void Parser::ParseCXXMemberDeclaratorBeforeInitializer(
    Declarator &DeclaratorInfo, VirtSpecifiers &VS, ExprResult &BitfieldSize,
    LateParsedAttrList &LateParsedAttrs) {
  // member-declarator:
  //   declarator pure-specifier[opt]
  //   declarator brace-or-equal-initializer[opt]
  //   identifier[opt] ':' constant-expression
  if (Tok.isNot(tok::colon))
    ParseDeclarator(DeclaratorInfo);
  else
    DeclaratorInfo.SetIdentifier(nullptr, Tok.getLocation());

  if (!DeclaratorInfo.isFunctionDeclarator() && TryConsumeToken(tok::colon)) {
    assert(DeclaratorInfo.isPastIdentifier() &&
           "don't know where identifier would go yet?");
    BitfieldSize = ParseConstantExpression();
    if (BitfieldSize.isInvalid())
      SkipUntil(tok::comma, StopAtSemi | StopBeforeMatch);
  } else
    ParseOptionalCXX11VirtSpecifierSeq(
        VS, getCurrentClass().IsInterface,
        DeclaratorInfo.getDeclSpec().getFriendSpecLoc());

  // If a simple-asm-expr is present, parse it.
  if (Tok.is(tok::kw_asm)) {
    SourceLocation Loc;
    ExprResult AsmLabel(ParseSimpleAsm(&Loc));
    if (AsmLabel.isInvalid())
      SkipUntil(tok::comma, StopAtSemi | StopBeforeMatch);

    DeclaratorInfo.setAsmLabel(AsmLabel.get());
    DeclaratorInfo.SetRangeEnd(Loc);
  }

  // If attributes exist after the declarator, but before an '{', parse them.
  MaybeParseGNUAttributes(DeclaratorInfo, &LateParsedAttrs);

  // For compatibility with code written to older Clang, also accept a
  // virt-specifier *after* the GNU attributes.
  if (BitfieldSize.isUnset() && VS.isUnset()) {
    ParseOptionalCXX11VirtSpecifierSeq(
        VS, getCurrentClass().IsInterface,
        DeclaratorInfo.getDeclSpec().getFriendSpecLoc());
    if (!VS.isUnset()) {
      // If we saw any GNU-style attributes that are known to GCC followed by a
      // virt-specifier, issue a GCC-compat warning.
      const AttributeList *Attr = DeclaratorInfo.getAttributes();
      while (Attr) {
        if (Attr->isKnownToGCC() && !Attr->isCXX11Attribute())
          Diag(Attr->getLoc(), diag::warn_gcc_attribute_location);
        Attr = Attr->getNext();
      }
    }
  }
}

/// ParseCXXClassMemberDeclaration - Parse a C++ class member declaration.
///
///       member-declaration:
///         decl-specifier-seq[opt] member-declarator-list[opt] ';'
///         function-definition ';'[opt]
///         ::[opt] nested-name-specifier template[opt] unqualified-id ';'[TODO]
///         using-declaration                                            [TODO]
/// [C++0x] static_assert-declaration
///         template-declaration
/// [GNU]   '__extension__' member-declaration
///
///       member-declarator-list:
///         member-declarator
///         member-declarator-list ',' member-declarator
///
///       member-declarator:
///         declarator virt-specifier-seq[opt] pure-specifier[opt]
///         declarator constant-initializer[opt]
/// [C++11] declarator brace-or-equal-initializer[opt]
///         identifier[opt] ':' constant-expression
///
///       virt-specifier-seq:
///         virt-specifier
///         virt-specifier-seq virt-specifier
///
///       virt-specifier:
///         override
///         final
/// [MS]    sealed
/// 
///       pure-specifier:
///         '= 0'
///
///       constant-initializer:
///         '=' constant-expression
///
void Parser::ParseCXXClassMemberDeclaration(AccessSpecifier AS,
                                            AttributeList *AccessAttrs,
                                       const ParsedTemplateInfo &TemplateInfo,
                                       ParsingDeclRAIIObject *TemplateDiags) {
  if (Tok.is(tok::at)) {
    if (getLangOpts().ObjC1 && NextToken().isObjCAtKeyword(tok::objc_defs))
      Diag(Tok, diag::err_at_defs_cxx);
    else
      Diag(Tok, diag::err_at_in_class);

    ConsumeToken();
    SkipUntil(tok::r_brace, StopAtSemi);
    return;
  }

  // Turn on colon protection early, while parsing declspec, although there is
  // nothing to protect there. It prevents from false errors if error recovery
  // incorrectly determines where the declspec ends, as in the example:
  //   struct A { enum class B { C }; };
  //   const int C = 4;
  //   struct D { A::B : C; };
  ColonProtectionRAIIObject X(*this);

  // Access declarations.
  bool MalformedTypeSpec = false;
  if (!TemplateInfo.Kind &&
      (Tok.is(tok::identifier) || Tok.is(tok::coloncolon))) {
    if (TryAnnotateCXXScopeToken())
      MalformedTypeSpec = true;

    bool isAccessDecl;
    if (Tok.isNot(tok::annot_cxxscope))
      isAccessDecl = false;
    else if (NextToken().is(tok::identifier))
      isAccessDecl = GetLookAheadToken(2).is(tok::semi);
    else
      isAccessDecl = NextToken().is(tok::kw_operator);

    if (isAccessDecl) {
      // Collect the scope specifier token we annotated earlier.
      CXXScopeSpec SS;
      ParseOptionalCXXScopeSpecifier(SS, ParsedType(), 
                                     /*EnteringContext=*/false);

      // Try to parse an unqualified-id.
      SourceLocation TemplateKWLoc;
      UnqualifiedId Name;
      if (ParseUnqualifiedId(SS, false, true, true, ParsedType(),
                             TemplateKWLoc, Name)) {
        SkipUntil(tok::semi);
        return;
      }

      // TODO: recover from mistakenly-qualified operator declarations.
      if (ExpectAndConsume(tok::semi, diag::err_expected_after,
                           "access declaration")) {
        SkipUntil(tok::semi);
        return;
      }

      Actions.ActOnUsingDeclaration(getCurScope(), AS,
                                    /* HasUsingKeyword */ false,
                                    SourceLocation(),
                                    SS, Name,
                                    /* AttrList */ nullptr,
                                    /* HasTypenameKeyword */ false,
                                    SourceLocation());
      return;
    }
  }

  // static_assert-declaration. A templated static_assert declaration is
  // diagnosed in Parser::ParseSingleDeclarationAfterTemplate.
  if (!TemplateInfo.Kind &&
      (Tok.is(tok::kw_static_assert) || Tok.is(tok::kw__Static_assert))) {
    SourceLocation DeclEnd;
    ParseStaticAssertDeclaration(DeclEnd);
    return;
  }

  if (Tok.is(tok::kw_template)) {
    assert(!TemplateInfo.TemplateParams &&
           "Nested template improperly parsed?");
    SourceLocation DeclEnd;
    ParseDeclarationStartingWithTemplate(Declarator::MemberContext, DeclEnd,
                                         AS, AccessAttrs);
    return;
  }

  // Handle:  member-declaration ::= '__extension__' member-declaration
  if (Tok.is(tok::kw___extension__)) {
    // __extension__ silences extension warnings in the subexpression.
    ExtensionRAIIObject O(Diags);  // Use RAII to do this.
    ConsumeToken();
    return ParseCXXClassMemberDeclaration(AS, AccessAttrs,
                                          TemplateInfo, TemplateDiags);
  }

  ParsedAttributesWithRange attrs(AttrFactory);
  ParsedAttributesWithRange FnAttrs(AttrFactory);
  // Optional C++11 attribute-specifier
  MaybeParseCXX11Attributes(attrs);
  // We need to keep these attributes for future diagnostic
  // before they are taken over by declaration specifier.
  FnAttrs.addAll(attrs.getList());
  FnAttrs.Range = attrs.Range;

  MaybeParseMicrosoftAttributes(attrs);

  if (Tok.is(tok::kw_using)) {
    ProhibitAttributes(attrs);

    // Eat 'using'.
    SourceLocation UsingLoc = ConsumeToken();

    if (Tok.is(tok::kw_namespace)) {
      Diag(UsingLoc, diag::err_using_namespace_in_class);
      SkipUntil(tok::semi, StopBeforeMatch);
    } else {
      SourceLocation DeclEnd;
      // Otherwise, it must be a using-declaration or an alias-declaration.
      ParseUsingDeclaration(Declarator::MemberContext, TemplateInfo,
                            UsingLoc, DeclEnd, AS);
    }
    return;
  }

  // Hold late-parsed attributes so we can attach a Decl to them later.
  LateParsedAttrList CommonLateParsedAttrs;

  // decl-specifier-seq:
  // Parse the common declaration-specifiers piece.
  ParsingDeclSpec DS(*this, TemplateDiags);
  DS.takeAttributesFrom(attrs);
  if (MalformedTypeSpec)
    DS.SetTypeSpecError();

  ParseDeclarationSpecifiers(DS, TemplateInfo, AS, DSC_class,
                             &CommonLateParsedAttrs);

  // Turn off colon protection that was set for declspec.
  X.restore();

  // If we had a free-standing type definition with a missing semicolon, we
  // may get this far before the problem becomes obvious.
  if (DS.hasTagDefinition() &&
      TemplateInfo.Kind == ParsedTemplateInfo::NonTemplate &&
      DiagnoseMissingSemiAfterTagDefinition(DS, AS, DSC_class,
                                            &CommonLateParsedAttrs))
    return;

  MultiTemplateParamsArg TemplateParams(
      TemplateInfo.TemplateParams? TemplateInfo.TemplateParams->data()
                                 : nullptr,
      TemplateInfo.TemplateParams? TemplateInfo.TemplateParams->size() : 0);

  if (TryConsumeToken(tok::semi)) {
    if (DS.isFriendSpecified())
      ProhibitAttributes(FnAttrs);

    Decl *TheDecl =
      Actions.ParsedFreeStandingDeclSpec(getCurScope(), AS, DS, TemplateParams);
    DS.complete(TheDecl);
    return;
  }

  ParsingDeclarator DeclaratorInfo(*this, DS, Declarator::MemberContext);
  VirtSpecifiers VS;

  // Hold late-parsed attributes so we can attach a Decl to them later.
  LateParsedAttrList LateParsedAttrs;

  SourceLocation EqualLoc;
  bool HasInitializer = false;
  ExprResult Init;

  SmallVector<Decl *, 8> DeclsInGroup;
  ExprResult BitfieldSize;
  bool ExpectSemi = true;

  // Parse the first declarator.
  ParseCXXMemberDeclaratorBeforeInitializer(DeclaratorInfo, VS, BitfieldSize,
                                            LateParsedAttrs);

  // If this has neither a name nor a bit width, something has gone seriously
  // wrong. Skip until the semi-colon or }.
  if (!DeclaratorInfo.hasName() && BitfieldSize.isUnset()) {
    // If so, skip until the semi-colon or a }.
    SkipUntil(tok::r_brace, StopAtSemi | StopBeforeMatch);
    TryConsumeToken(tok::semi);
    return;
  }

  // Check for a member function definition.
  if (BitfieldSize.isUnset()) {
    // MSVC permits pure specifier on inline functions defined at class scope.
    // Hence check for =0 before checking for function definition.
    if (getLangOpts().MicrosoftExt && Tok.is(tok::equal) &&
        DeclaratorInfo.isFunctionDeclarator() &&
        NextToken().is(tok::numeric_constant)) {
      EqualLoc = ConsumeToken();
      Init = ParseInitializer();
      if (Init.isInvalid())
        SkipUntil(tok::comma, StopAtSemi | StopBeforeMatch);
      else
        HasInitializer = true;
    }

    FunctionDefinitionKind DefinitionKind = FDK_Declaration;
    // function-definition:
    //
    // In C++11, a non-function declarator followed by an open brace is a
    // braced-init-list for an in-class member initialization, not an
    // erroneous function definition.
    if (Tok.is(tok::l_brace) && !getLangOpts().CPlusPlus11) {
      DefinitionKind = FDK_Definition;
    } else if (DeclaratorInfo.isFunctionDeclarator()) {
      if (Tok.is(tok::l_brace) || Tok.is(tok::colon) || Tok.is(tok::kw_try)) {
        DefinitionKind = FDK_Definition;
      } else if (Tok.is(tok::equal)) {
        const Token &KW = NextToken();
        if (KW.is(tok::kw_default))
          DefinitionKind = FDK_Defaulted;
        else if (KW.is(tok::kw_delete))
          DefinitionKind = FDK_Deleted;
      }
    }

    // C++11 [dcl.attr.grammar] p4: If an attribute-specifier-seq appertains 
    // to a friend declaration, that declaration shall be a definition.
    if (DeclaratorInfo.isFunctionDeclarator() && 
        DefinitionKind != FDK_Definition && DS.isFriendSpecified()) {
      // Diagnose attributes that appear before decl specifier:
      // [[]] friend int foo();
      ProhibitAttributes(FnAttrs);
    }

    if (DefinitionKind) {
      if (!DeclaratorInfo.isFunctionDeclarator()) {
        Diag(DeclaratorInfo.getIdentifierLoc(), diag::err_func_def_no_params);
        ConsumeBrace();
        SkipUntil(tok::r_brace);

        // Consume the optional ';'
        TryConsumeToken(tok::semi);

        return;
      }

      if (DS.getStorageClassSpec() == DeclSpec::SCS_typedef) {
        Diag(DeclaratorInfo.getIdentifierLoc(),
             diag::err_function_declared_typedef);

        // Recover by treating the 'typedef' as spurious.
        DS.ClearStorageClassSpecs();
      }

      Decl *FunDecl =
        ParseCXXInlineMethodDef(AS, AccessAttrs, DeclaratorInfo, TemplateInfo,
                                VS, DefinitionKind, Init);

      if (FunDecl) {
        for (unsigned i = 0, ni = CommonLateParsedAttrs.size(); i < ni; ++i) {
          CommonLateParsedAttrs[i]->addDecl(FunDecl);
        }
        for (unsigned i = 0, ni = LateParsedAttrs.size(); i < ni; ++i) {
          LateParsedAttrs[i]->addDecl(FunDecl);
        }
      }
      LateParsedAttrs.clear();

      // Consume the ';' - it's optional unless we have a delete or default
      if (Tok.is(tok::semi))
        ConsumeExtraSemi(AfterMemberFunctionDefinition);

      return;
    }
  }

  // member-declarator-list:
  //   member-declarator
  //   member-declarator-list ',' member-declarator

  while (1) {
    InClassInitStyle HasInClassInit = ICIS_NoInit;
    if ((Tok.is(tok::equal) || Tok.is(tok::l_brace)) && !HasInitializer) {
      if (BitfieldSize.get()) {
        Diag(Tok, diag::err_bitfield_member_init);
        SkipUntil(tok::comma, StopAtSemi | StopBeforeMatch);
      } else {
        HasInitializer = true;
        if (!DeclaratorInfo.isDeclarationOfFunction() &&
            DeclaratorInfo.getDeclSpec().getStorageClassSpec()
              != DeclSpec::SCS_typedef)
          HasInClassInit = Tok.is(tok::equal) ? ICIS_CopyInit : ICIS_ListInit;
      }
    }

    // NOTE: If Sema is the Action module and declarator is an instance field,
    // this call will *not* return the created decl; It will return null.
    // See Sema::ActOnCXXMemberDeclarator for details.

    NamedDecl *ThisDecl = nullptr;
    if (DS.isFriendSpecified()) {
      // C++11 [dcl.attr.grammar] p4: If an attribute-specifier-seq appertains
      // to a friend declaration, that declaration shall be a definition.
      //
      // Diagnose attributes that appear in a friend member function declarator:
      //   friend int foo [[]] ();
      SmallVector<SourceRange, 4> Ranges;
      DeclaratorInfo.getCXX11AttributeRanges(Ranges);
      for (SmallVectorImpl<SourceRange>::iterator I = Ranges.begin(),
           E = Ranges.end(); I != E; ++I)
        Diag((*I).getBegin(), diag::err_attributes_not_allowed) << *I;

      ThisDecl = Actions.ActOnFriendFunctionDecl(getCurScope(), DeclaratorInfo,
                                                 TemplateParams);
    } else {
      ThisDecl = Actions.ActOnCXXMemberDeclarator(getCurScope(), AS,
                                                  DeclaratorInfo,
                                                  TemplateParams,
                                                  BitfieldSize.get(),
                                                  VS, HasInClassInit);

      if (VarTemplateDecl *VT =
              ThisDecl ? dyn_cast<VarTemplateDecl>(ThisDecl) : nullptr)
        // Re-direct this decl to refer to the templated decl so that we can
        // initialize it.
        ThisDecl = VT->getTemplatedDecl();

      if (ThisDecl && AccessAttrs)
        Actions.ProcessDeclAttributeList(getCurScope(), ThisDecl, AccessAttrs);
    }

    // Handle the initializer.
    if (HasInClassInit != ICIS_NoInit &&
        DeclaratorInfo.getDeclSpec().getStorageClassSpec() !=
        DeclSpec::SCS_static) {
      // The initializer was deferred; parse it and cache the tokens.
      Diag(Tok, getLangOpts().CPlusPlus11
                    ? diag::warn_cxx98_compat_nonstatic_member_init
                    : diag::ext_nonstatic_member_init);

      if (DeclaratorInfo.isArrayOfUnknownBound()) {
        // C++11 [dcl.array]p3: An array bound may also be omitted when the
        // declarator is followed by an initializer.
        //
        // A brace-or-equal-initializer for a member-declarator is not an
        // initializer in the grammar, so this is ill-formed.
        Diag(Tok, diag::err_incomplete_array_member_init);
        SkipUntil(tok::comma, StopAtSemi | StopBeforeMatch);

        // Avoid later warnings about a class member of incomplete type.
        if (ThisDecl)
          ThisDecl->setInvalidDecl();
      } else
        ParseCXXNonStaticMemberInitializer(ThisDecl);
    } else if (HasInitializer) {
      // Normal initializer.
      if (!Init.isUsable())
        Init = ParseCXXMemberInitializer(
            ThisDecl, DeclaratorInfo.isDeclarationOfFunction(), EqualLoc);

      if (Init.isInvalid())
        SkipUntil(tok::comma, StopAtSemi | StopBeforeMatch);
      else if (ThisDecl)
        Actions.AddInitializerToDecl(ThisDecl, Init.get(), EqualLoc.isInvalid(),
                                     DS.containsPlaceholderType());
    } else if (ThisDecl && DS.getStorageClassSpec() == DeclSpec::SCS_static)
      // No initializer.
      Actions.ActOnUninitializedDecl(ThisDecl, DS.containsPlaceholderType());

    if (ThisDecl) {
      if (!ThisDecl->isInvalidDecl()) {
        // Set the Decl for any late parsed attributes
        for (unsigned i = 0, ni = CommonLateParsedAttrs.size(); i < ni; ++i)
          CommonLateParsedAttrs[i]->addDecl(ThisDecl);

        for (unsigned i = 0, ni = LateParsedAttrs.size(); i < ni; ++i)
          LateParsedAttrs[i]->addDecl(ThisDecl);
      }
      Actions.FinalizeDeclaration(ThisDecl);
      DeclsInGroup.push_back(ThisDecl);

      if (DeclaratorInfo.isFunctionDeclarator() &&
          DeclaratorInfo.getDeclSpec().getStorageClassSpec() !=
              DeclSpec::SCS_typedef)
        HandleMemberFunctionDeclDelays(DeclaratorInfo, ThisDecl);
    }
    LateParsedAttrs.clear();

    DeclaratorInfo.complete(ThisDecl);

    // If we don't have a comma, it is either the end of the list (a ';')
    // or an error, bail out.
    SourceLocation CommaLoc;
    if (!TryConsumeToken(tok::comma, CommaLoc))
      break;

    if (Tok.isAtStartOfLine() &&
        !MightBeDeclarator(Declarator::MemberContext)) {
      // This comma was followed by a line-break and something which can't be
      // the start of a declarator. The comma was probably a typo for a
      // semicolon.
      Diag(CommaLoc, diag::err_expected_semi_declaration)
        << FixItHint::CreateReplacement(CommaLoc, ";");
      ExpectSemi = false;
      break;
    }

    // Parse the next declarator.
    DeclaratorInfo.clear();
    VS.clear();
    BitfieldSize = true;
    Init = true;
    HasInitializer = false;
    DeclaratorInfo.setCommaLoc(CommaLoc);

    // GNU attributes are allowed before the second and subsequent declarator.
    MaybeParseGNUAttributes(DeclaratorInfo);

    ParseCXXMemberDeclaratorBeforeInitializer(DeclaratorInfo, VS, BitfieldSize,
                                              LateParsedAttrs);
  }

  if (ExpectSemi &&
      ExpectAndConsume(tok::semi, diag::err_expected_semi_decl_list)) {
    // Skip to end of block or statement.
    SkipUntil(tok::r_brace, StopAtSemi | StopBeforeMatch);
    // If we stopped at a ';', eat it.
    TryConsumeToken(tok::semi);
    return;
  }

  Actions.FinalizeDeclaratorGroup(getCurScope(), DS, DeclsInGroup);
}

/// ParseCXXMemberInitializer - Parse the brace-or-equal-initializer or
/// pure-specifier. Also detect and reject any attempted defaulted/deleted
/// function definition. The location of the '=', if any, will be placed in
/// EqualLoc.
///
///   pure-specifier:
///     '= 0'
///
///   brace-or-equal-initializer:
///     '=' initializer-expression
///     braced-init-list
///
///   initializer-clause:
///     assignment-expression
///     braced-init-list
///
///   defaulted/deleted function-definition:
///     '=' 'default'
///     '=' 'delete'
///
/// Prior to C++0x, the assignment-expression in an initializer-clause must
/// be a constant-expression.
ExprResult Parser::ParseCXXMemberInitializer(Decl *D, bool IsFunction,
                                             SourceLocation &EqualLoc) {
  assert((Tok.is(tok::equal) || Tok.is(tok::l_brace))
         && "Data member initializer not starting with '=' or '{'");

  EnterExpressionEvaluationContext Context(Actions, 
                                           Sema::PotentiallyEvaluated,
                                           D);
  if (TryConsumeToken(tok::equal, EqualLoc)) {
    if (Tok.is(tok::kw_delete)) {
      // In principle, an initializer of '= delete p;' is legal, but it will
      // never type-check. It's better to diagnose it as an ill-formed expression
      // than as an ill-formed deleted non-function member.
      // An initializer of '= delete p, foo' will never be parsed, because
      // a top-level comma always ends the initializer expression.
      const Token &Next = NextToken();
      if (IsFunction || Next.is(tok::semi) || Next.is(tok::comma) ||
          Next.is(tok::eof)) {
        if (IsFunction)
          Diag(ConsumeToken(), diag::err_default_delete_in_multiple_declaration)
            << 1 /* delete */;
        else
          Diag(ConsumeToken(), diag::err_deleted_non_function);
        return ExprError();
      }
    } else if (Tok.is(tok::kw_default)) {
      if (IsFunction)
        Diag(Tok, diag::err_default_delete_in_multiple_declaration)
          << 0 /* default */;
      else
        Diag(ConsumeToken(), diag::err_default_special_members);
      return ExprError();
    }

  }
  return ParseInitializer();
}

/// ParseCXXMemberSpecification - Parse the class definition.
///
///       member-specification:
///         member-declaration member-specification[opt]
///         access-specifier ':' member-specification[opt]
///
void Parser::ParseCXXMemberSpecification(SourceLocation RecordLoc,
                                         SourceLocation AttrFixitLoc,
                                         ParsedAttributesWithRange &Attrs,
                                         unsigned TagType, Decl *TagDecl) {
  assert((TagType == DeclSpec::TST_struct ||
         TagType == DeclSpec::TST_interface ||
         TagType == DeclSpec::TST_union  ||
         TagType == DeclSpec::TST_class) && "Invalid TagType!");

  PrettyDeclStackTraceEntry CrashInfo(Actions, TagDecl, RecordLoc,
                                      "parsing struct/union/class body");

  // Determine whether this is a non-nested class. Note that local
  // classes are *not* considered to be nested classes.
  bool NonNestedClass = true;
  if (!ClassStack.empty()) {
    for (const Scope *S = getCurScope(); S; S = S->getParent()) {
      if (S->isClassScope()) {
        // We're inside a class scope, so this is a nested class.
        NonNestedClass = false;

        // The Microsoft extension __interface does not permit nested classes.
        if (getCurrentClass().IsInterface) {
          Diag(RecordLoc, diag::err_invalid_member_in_interface)
            << /*ErrorType=*/6
            << (isa<NamedDecl>(TagDecl)
                  ? cast<NamedDecl>(TagDecl)->getQualifiedNameAsString()
                  : "(anonymous)");
        }
        break;
      }

      if ((S->getFlags() & Scope::FnScope)) {
        // If we're in a function or function template declared in the
        // body of a class, then this is a local class rather than a
        // nested class.
        const Scope *Parent = S->getParent();
        if (Parent->isTemplateParamScope())
          Parent = Parent->getParent();
        if (Parent->isClassScope())
          break;
      }
    }
  }

  // Enter a scope for the class.
  ParseScope ClassScope(this, Scope::ClassScope|Scope::DeclScope);

  // Note that we are parsing a new (potentially-nested) class definition.
  ParsingClassDefinition ParsingDef(*this, TagDecl, NonNestedClass,
                                    TagType == DeclSpec::TST_interface);

  if (TagDecl)
    Actions.ActOnTagStartDefinition(getCurScope(), TagDecl);

  SourceLocation FinalLoc;
  bool IsFinalSpelledSealed = false;

  // Parse the optional 'final' keyword.
  if (getLangOpts().CPlusPlus && Tok.is(tok::identifier)) {
    VirtSpecifiers::Specifier Specifier = isCXX11VirtSpecifier(Tok);
    assert((Specifier == VirtSpecifiers::VS_Final ||
            Specifier == VirtSpecifiers::VS_Sealed) &&
           "not a class definition");
    FinalLoc = ConsumeToken();
    IsFinalSpelledSealed = Specifier == VirtSpecifiers::VS_Sealed;

    if (TagType == DeclSpec::TST_interface)
      Diag(FinalLoc, diag::err_override_control_interface)
        << VirtSpecifiers::getSpecifierName(Specifier);
    else if (Specifier == VirtSpecifiers::VS_Final)
      Diag(FinalLoc, getLangOpts().CPlusPlus11
                         ? diag::warn_cxx98_compat_override_control_keyword
                         : diag::ext_override_control_keyword)
        << VirtSpecifiers::getSpecifierName(Specifier);
    else if (Specifier == VirtSpecifiers::VS_Sealed)
      Diag(FinalLoc, diag::ext_ms_sealed_keyword);

    // Parse any C++11 attributes after 'final' keyword.
    // These attributes are not allowed to appear here,
    // and the only possible place for them to appertain
    // to the class would be between class-key and class-name.
    CheckMisplacedCXX11Attribute(Attrs, AttrFixitLoc);
  }

  if (Tok.is(tok::colon)) {
    ParseBaseClause(TagDecl);

    if (!Tok.is(tok::l_brace)) {
      Diag(Tok, diag::err_expected_lbrace_after_base_specifiers);

      if (TagDecl)
        Actions.ActOnTagDefinitionError(getCurScope(), TagDecl);
      return;
    }
  }

  assert(Tok.is(tok::l_brace));
  BalancedDelimiterTracker T(*this, tok::l_brace);
  T.consumeOpen();

  if (TagDecl)
    Actions.ActOnStartCXXMemberDeclarations(getCurScope(), TagDecl, FinalLoc,
                                            IsFinalSpelledSealed,
                                            T.getOpenLocation());

  // C++ 11p3: Members of a class defined with the keyword class are private
  // by default. Members of a class defined with the keywords struct or union
  // are public by default.
  AccessSpecifier CurAS;
  if (TagType == DeclSpec::TST_class)
    CurAS = AS_private;
  else
    CurAS = AS_public;
  ParsedAttributes AccessAttrs(AttrFactory);

  if (TagDecl) {
    // While we still have something to read, read the member-declarations.
    while (Tok.isNot(tok::r_brace) && !isEofOrEom()) {
      // Each iteration of this loop reads one member-declaration.

      if (getLangOpts().MicrosoftExt && (Tok.is(tok::kw___if_exists) ||
          Tok.is(tok::kw___if_not_exists))) {
        ParseMicrosoftIfExistsClassDeclaration((DeclSpec::TST)TagType, CurAS);
        continue;
      }

      // Check for extraneous top-level semicolon.
      if (Tok.is(tok::semi)) {
        ConsumeExtraSemi(InsideStruct, TagType);
        continue;
      }

      if (Tok.is(tok::annot_pragma_vis)) {
        HandlePragmaVisibility();
        continue;
      }

      if (Tok.is(tok::annot_pragma_pack)) {
        HandlePragmaPack();
        continue;
      }

      if (Tok.is(tok::annot_pragma_align)) {
        HandlePragmaAlign();
        continue;
      }

      if (Tok.is(tok::annot_pragma_openmp)) {
        ParseOpenMPDeclarativeDirective();
        continue;
      }

      if (Tok.is(tok::annot_pragma_ms_pointers_to_members)) {
        HandlePragmaMSPointersToMembers();
        continue;
      }

      if (Tok.is(tok::annot_pragma_ms_pragma)) {
        HandlePragmaMSPragma();
        continue;
      }

      // If we see a namespace here, a close brace was missing somewhere.
      if (Tok.is(tok::kw_namespace)) {
        DiagnoseUnexpectedNamespace(cast<NamedDecl>(TagDecl));
        break;
      }

      AccessSpecifier AS = getAccessSpecifierIfPresent();
      if (AS != AS_none) {
        // Current token is a C++ access specifier.
        CurAS = AS;
        SourceLocation ASLoc = Tok.getLocation();
        unsigned TokLength = Tok.getLength();
        ConsumeToken();
        AccessAttrs.clear();
        MaybeParseGNUAttributes(AccessAttrs);

        SourceLocation EndLoc;
        if (TryConsumeToken(tok::colon, EndLoc)) {
        } else if (TryConsumeToken(tok::semi, EndLoc)) {
          Diag(EndLoc, diag::err_expected)
              << tok::colon << FixItHint::CreateReplacement(EndLoc, ":");
        } else {
          EndLoc = ASLoc.getLocWithOffset(TokLength);
          Diag(EndLoc, diag::err_expected)
              << tok::colon << FixItHint::CreateInsertion(EndLoc, ":");
        }

        // The Microsoft extension __interface does not permit non-public
        // access specifiers.
        if (TagType == DeclSpec::TST_interface && CurAS != AS_public) {
          Diag(ASLoc, diag::err_access_specifier_interface)
            << (CurAS == AS_protected);
        }

        if (Actions.ActOnAccessSpecifier(AS, ASLoc, EndLoc,
                                         AccessAttrs.getList())) {
          // found another attribute than only annotations
          AccessAttrs.clear();
        }

        continue;
      }

      // Parse all the comma separated declarators.
      ParseCXXClassMemberDeclaration(CurAS, AccessAttrs.getList());
    }

    T.consumeClose();
  } else {
    SkipUntil(tok::r_brace);
  }

  // If attributes exist after class contents, parse them.
  ParsedAttributes attrs(AttrFactory);
  MaybeParseGNUAttributes(attrs);

  if (TagDecl)
    Actions.ActOnFinishCXXMemberSpecification(getCurScope(), RecordLoc, TagDecl,
                                              T.getOpenLocation(), 
                                              T.getCloseLocation(),
                                              attrs.getList());

  // C++11 [class.mem]p2:
  //   Within the class member-specification, the class is regarded as complete
  //   within function bodies, default arguments, and
  //   brace-or-equal-initializers for non-static data members (including such
  //   things in nested classes).
  if (TagDecl && NonNestedClass) {
    // We are not inside a nested class. This class and its nested classes
    // are complete and we can parse the delayed portions of method
    // declarations and the lexed inline method definitions, along with any
    // delayed attributes.
    SourceLocation SavedPrevTokLocation = PrevTokLocation;
    ParseLexedAttributes(getCurrentClass());
    ParseLexedMethodDeclarations(getCurrentClass());

    // We've finished with all pending member declarations.
    Actions.ActOnFinishCXXMemberDecls();

    ParseLexedMemberInitializers(getCurrentClass());
    ParseLexedMethodDefs(getCurrentClass());
    PrevTokLocation = SavedPrevTokLocation;
  }

  if (TagDecl)
    Actions.ActOnTagFinishDefinition(getCurScope(), TagDecl, 
                                     T.getCloseLocation());

  // Leave the class scope.
  ParsingDef.Pop();
  ClassScope.Exit();
}

void Parser::DiagnoseUnexpectedNamespace(NamedDecl *D) {
  assert(Tok.is(tok::kw_namespace));

  // FIXME: Suggest where the close brace should have gone by looking
  // at indentation changes within the definition body.
  Diag(D->getLocation(),
       diag::err_missing_end_of_definition) << D;
  Diag(Tok.getLocation(),
       diag::note_missing_end_of_definition_before) << D;

  // Push '};' onto the token stream to recover.
  PP.EnterToken(Tok);

  Tok.startToken();
  Tok.setLocation(PP.getLocForEndOfToken(PrevTokLocation));
  Tok.setKind(tok::semi);
  PP.EnterToken(Tok);

  Tok.setKind(tok::r_brace);
}

/// ParseConstructorInitializer - Parse a C++ constructor initializer,
/// which explicitly initializes the members or base classes of a
/// class (C++ [class.base.init]). For example, the three initializers
/// after the ':' in the Derived constructor below:
///
/// @code
/// class Base { };
/// class Derived : Base {
///   int x;
///   float f;
/// public:
///   Derived(float f) : Base(), x(17), f(f) { }
/// };
/// @endcode
///
/// [C++]  ctor-initializer:
///          ':' mem-initializer-list
///
/// [C++]  mem-initializer-list:
///          mem-initializer ...[opt]
///          mem-initializer ...[opt] , mem-initializer-list
void Parser::ParseConstructorInitializer(Decl *ConstructorDecl) {
  assert(Tok.is(tok::colon) && "Constructor initializer always starts with ':'");

  // Poison the SEH identifiers so they are flagged as illegal in constructor initializers
  PoisonSEHIdentifiersRAIIObject PoisonSEHIdentifiers(*this, true);
  SourceLocation ColonLoc = ConsumeToken();

  SmallVector<CXXCtorInitializer*, 4> MemInitializers;
  bool AnyErrors = false;

  do {
    if (Tok.is(tok::code_completion)) {
      Actions.CodeCompleteConstructorInitializer(ConstructorDecl,
                                                 MemInitializers);
      return cutOffParsing();
    } else {
      MemInitResult MemInit = ParseMemInitializer(ConstructorDecl);
      if (!MemInit.isInvalid())
        MemInitializers.push_back(MemInit.get());
      else
        AnyErrors = true;
    }
    
    if (Tok.is(tok::comma))
      ConsumeToken();
    else if (Tok.is(tok::l_brace))
      break;
    // If the next token looks like a base or member initializer, assume that
    // we're just missing a comma.
    else if (Tok.is(tok::identifier) || Tok.is(tok::coloncolon)) {
      SourceLocation Loc = PP.getLocForEndOfToken(PrevTokLocation);
      Diag(Loc, diag::err_ctor_init_missing_comma)
        << FixItHint::CreateInsertion(Loc, ", ");
    } else {
      // Skip over garbage, until we get to '{'.  Don't eat the '{'.
      Diag(Tok.getLocation(), diag::err_expected_either) << tok::l_brace
                                                         << tok::comma;
      SkipUntil(tok::l_brace, StopAtSemi | StopBeforeMatch);
      break;
    }
  } while (true);

  Actions.ActOnMemInitializers(ConstructorDecl, ColonLoc, MemInitializers,
                               AnyErrors);
}

/// ParseMemInitializer - Parse a C++ member initializer, which is
/// part of a constructor initializer that explicitly initializes one
/// member or base class (C++ [class.base.init]). See
/// ParseConstructorInitializer for an example.
///
/// [C++] mem-initializer:
///         mem-initializer-id '(' expression-list[opt] ')'
/// [C++0x] mem-initializer-id braced-init-list
///
/// [C++] mem-initializer-id:
///         '::'[opt] nested-name-specifier[opt] class-name
///         identifier
Parser::MemInitResult Parser::ParseMemInitializer(Decl *ConstructorDecl) {
  // parse '::'[opt] nested-name-specifier[opt]
  CXXScopeSpec SS;
  ParseOptionalCXXScopeSpecifier(SS, ParsedType(), /*EnteringContext=*/false);
  ParsedType TemplateTypeTy;
  if (Tok.is(tok::annot_template_id)) {
    TemplateIdAnnotation *TemplateId = takeTemplateIdAnnotation(Tok);
    if (TemplateId->Kind == TNK_Type_template ||
        TemplateId->Kind == TNK_Dependent_template_name) {
      AnnotateTemplateIdTokenAsType();
      assert(Tok.is(tok::annot_typename) && "template-id -> type failed");
      TemplateTypeTy = getTypeAnnotation(Tok);
    }
  }
  // Uses of decltype will already have been converted to annot_decltype by
  // ParseOptionalCXXScopeSpecifier at this point.
  if (!TemplateTypeTy && Tok.isNot(tok::identifier)
      && Tok.isNot(tok::annot_decltype)) {
    Diag(Tok, diag::err_expected_member_or_base_name);
    return true;
  }

  IdentifierInfo *II = nullptr;
  DeclSpec DS(AttrFactory);
  SourceLocation IdLoc = Tok.getLocation();
  if (Tok.is(tok::annot_decltype)) {
    // Get the decltype expression, if there is one.
    ParseDecltypeSpecifier(DS);
  } else {
    if (Tok.is(tok::identifier))
      // Get the identifier. This may be a member name or a class name,
      // but we'll let the semantic analysis determine which it is.
      II = Tok.getIdentifierInfo();
    ConsumeToken();
  }


  // Parse the '('.
  if (getLangOpts().CPlusPlus11 && Tok.is(tok::l_brace)) {
    Diag(Tok, diag::warn_cxx98_compat_generalized_initializer_lists);

    ExprResult InitList = ParseBraceInitializer();
    if (InitList.isInvalid())
      return true;

    SourceLocation EllipsisLoc;
    TryConsumeToken(tok::ellipsis, EllipsisLoc);

    return Actions.ActOnMemInitializer(ConstructorDecl, getCurScope(), SS, II,
                                       TemplateTypeTy, DS, IdLoc, 
                                       InitList.get(), EllipsisLoc);
  } else if(Tok.is(tok::l_paren)) {
    BalancedDelimiterTracker T(*this, tok::l_paren);
    T.consumeOpen();

    // Parse the optional expression-list.
    ExprVector ArgExprs;
    CommaLocsTy CommaLocs;
    if (Tok.isNot(tok::r_paren) && ParseExpressionList(ArgExprs, CommaLocs)) {
      SkipUntil(tok::r_paren, StopAtSemi);
      return true;
    }

    T.consumeClose();

    SourceLocation EllipsisLoc;
    TryConsumeToken(tok::ellipsis, EllipsisLoc);

    return Actions.ActOnMemInitializer(ConstructorDecl, getCurScope(), SS, II,
                                       TemplateTypeTy, DS, IdLoc,
                                       T.getOpenLocation(), ArgExprs,
                                       T.getCloseLocation(), EllipsisLoc);
  }

  if (getLangOpts().CPlusPlus11)
    return Diag(Tok, diag::err_expected_either) << tok::l_paren << tok::l_brace;
  else
    return Diag(Tok, diag::err_expected) << tok::l_paren;
}

/// \brief Parse a C++ exception-specification if present (C++0x [except.spec]).
///
///       exception-specification:
///         dynamic-exception-specification
///         noexcept-specification
///
///       noexcept-specification:
///         'noexcept'
///         'noexcept' '(' constant-expression ')'
ExceptionSpecificationType
Parser::tryParseExceptionSpecification(
                    SourceRange &SpecificationRange,
                    SmallVectorImpl<ParsedType> &DynamicExceptions,
                    SmallVectorImpl<SourceRange> &DynamicExceptionRanges,
                    ExprResult &NoexceptExpr) {
  ExceptionSpecificationType Result = EST_None;

  // See if there's a dynamic specification.
  if (Tok.is(tok::kw_throw)) {
    Result = ParseDynamicExceptionSpecification(SpecificationRange,
                                                DynamicExceptions,
                                                DynamicExceptionRanges);
    assert(DynamicExceptions.size() == DynamicExceptionRanges.size() &&
           "Produced different number of exception types and ranges.");
  }

  // If there's no noexcept specification, we're done.
  if (Tok.isNot(tok::kw_noexcept))
    return Result;

  Diag(Tok, diag::warn_cxx98_compat_noexcept_decl);

  // If we already had a dynamic specification, parse the noexcept for,
  // recovery, but emit a diagnostic and don't store the results.
  SourceRange NoexceptRange;
  ExceptionSpecificationType NoexceptType = EST_None;

  SourceLocation KeywordLoc = ConsumeToken();
  if (Tok.is(tok::l_paren)) {
    // There is an argument.
    BalancedDelimiterTracker T(*this, tok::l_paren);
    T.consumeOpen();
    NoexceptType = EST_ComputedNoexcept;
    NoexceptExpr = ParseConstantExpression();
    // The argument must be contextually convertible to bool. We use
    // ActOnBooleanCondition for this purpose.
    if (!NoexceptExpr.isInvalid())
      NoexceptExpr = Actions.ActOnBooleanCondition(getCurScope(), KeywordLoc,
                                                   NoexceptExpr.get());
    T.consumeClose();
    NoexceptRange = SourceRange(KeywordLoc, T.getCloseLocation());
  } else {
    // There is no argument.
    NoexceptType = EST_BasicNoexcept;
    NoexceptRange = SourceRange(KeywordLoc, KeywordLoc);
  }

  if (Result == EST_None) {
    SpecificationRange = NoexceptRange;
    Result = NoexceptType;

    // If there's a dynamic specification after a noexcept specification,
    // parse that and ignore the results.
    if (Tok.is(tok::kw_throw)) {
      Diag(Tok.getLocation(), diag::err_dynamic_and_noexcept_specification);
      ParseDynamicExceptionSpecification(NoexceptRange, DynamicExceptions,
                                         DynamicExceptionRanges);
    }
  } else {
    Diag(Tok.getLocation(), diag::err_dynamic_and_noexcept_specification);
  }

  return Result;
}

static void diagnoseDynamicExceptionSpecification(
    Parser &P, const SourceRange &Range, bool IsNoexcept) {
  if (P.getLangOpts().CPlusPlus11) {
    const char *Replacement = IsNoexcept ? "noexcept" : "noexcept(false)";
    P.Diag(Range.getBegin(), diag::warn_exception_spec_deprecated) << Range;
    P.Diag(Range.getBegin(), diag::note_exception_spec_deprecated)
      << Replacement << FixItHint::CreateReplacement(Range, Replacement);
  }
}

/// ParseDynamicExceptionSpecification - Parse a C++
/// dynamic-exception-specification (C++ [except.spec]).
///
///       dynamic-exception-specification:
///         'throw' '(' type-id-list [opt] ')'
/// [MS]    'throw' '(' '...' ')'
///
///       type-id-list:
///         type-id ... [opt]
///         type-id-list ',' type-id ... [opt]
///
ExceptionSpecificationType Parser::ParseDynamicExceptionSpecification(
                                  SourceRange &SpecificationRange,
                                  SmallVectorImpl<ParsedType> &Exceptions,
                                  SmallVectorImpl<SourceRange> &Ranges) {
  assert(Tok.is(tok::kw_throw) && "expected throw");

  SpecificationRange.setBegin(ConsumeToken());
  BalancedDelimiterTracker T(*this, tok::l_paren);
  if (T.consumeOpen()) {
    Diag(Tok, diag::err_expected_lparen_after) << "throw";
    SpecificationRange.setEnd(SpecificationRange.getBegin());
    return EST_DynamicNone;
  }

  // Parse throw(...), a Microsoft extension that means "this function
  // can throw anything".
  if (Tok.is(tok::ellipsis)) {
    SourceLocation EllipsisLoc = ConsumeToken();
    if (!getLangOpts().MicrosoftExt)
      Diag(EllipsisLoc, diag::ext_ellipsis_exception_spec);
    T.consumeClose();
    SpecificationRange.setEnd(T.getCloseLocation());
    diagnoseDynamicExceptionSpecification(*this, SpecificationRange, false);
    return EST_MSAny;
  }

  // Parse the sequence of type-ids.
  SourceRange Range;
  while (Tok.isNot(tok::r_paren)) {
    TypeResult Res(ParseTypeName(&Range));

    if (Tok.is(tok::ellipsis)) {
      // C++0x [temp.variadic]p5:
      //   - In a dynamic-exception-specification (15.4); the pattern is a 
      //     type-id.
      SourceLocation Ellipsis = ConsumeToken();
      Range.setEnd(Ellipsis);
      if (!Res.isInvalid())
        Res = Actions.ActOnPackExpansion(Res.get(), Ellipsis);
    }

    if (!Res.isInvalid()) {
      Exceptions.push_back(Res.get());
      Ranges.push_back(Range);
    }

    if (!TryConsumeToken(tok::comma))
      break;
  }

  T.consumeClose();
  SpecificationRange.setEnd(T.getCloseLocation());
  diagnoseDynamicExceptionSpecification(*this, SpecificationRange,
                                        Exceptions.empty());
  return Exceptions.empty() ? EST_DynamicNone : EST_Dynamic;
}

/// ParseTrailingReturnType - Parse a trailing return type on a new-style
/// function declaration.
TypeResult Parser::ParseTrailingReturnType(SourceRange &Range) {
  assert(Tok.is(tok::arrow) && "expected arrow");

  ConsumeToken();

  return ParseTypeName(&Range, Declarator::TrailingReturnContext);
}

/// \brief We have just started parsing the definition of a new class,
/// so push that class onto our stack of classes that is currently
/// being parsed.
Sema::ParsingClassState
Parser::PushParsingClass(Decl *ClassDecl, bool NonNestedClass,
                         bool IsInterface) {
  assert((NonNestedClass || !ClassStack.empty()) &&
         "Nested class without outer class");
  ClassStack.push(new ParsingClass(ClassDecl, NonNestedClass, IsInterface));
  return Actions.PushParsingClass();
}

/// \brief Deallocate the given parsed class and all of its nested
/// classes.
void Parser::DeallocateParsedClasses(Parser::ParsingClass *Class) {
  for (unsigned I = 0, N = Class->LateParsedDeclarations.size(); I != N; ++I)
    delete Class->LateParsedDeclarations[I];
  delete Class;
}

/// \brief Pop the top class of the stack of classes that are
/// currently being parsed.
///
/// This routine should be called when we have finished parsing the
/// definition of a class, but have not yet popped the Scope
/// associated with the class's definition.
void Parser::PopParsingClass(Sema::ParsingClassState state) {
  assert(!ClassStack.empty() && "Mismatched push/pop for class parsing");

  Actions.PopParsingClass(state);

  ParsingClass *Victim = ClassStack.top();
  ClassStack.pop();
  if (Victim->TopLevelClass) {
    // Deallocate all of the nested classes of this class,
    // recursively: we don't need to keep any of this information.
    DeallocateParsedClasses(Victim);
    return;
  }
  assert(!ClassStack.empty() && "Missing top-level class?");

  if (Victim->LateParsedDeclarations.empty()) {
    // The victim is a nested class, but we will not need to perform
    // any processing after the definition of this class since it has
    // no members whose handling was delayed. Therefore, we can just
    // remove this nested class.
    DeallocateParsedClasses(Victim);
    return;
  }

  // This nested class has some members that will need to be processed
  // after the top-level class is completely defined. Therefore, add
  // it to the list of nested classes within its parent.
  assert(getCurScope()->isClassScope() && "Nested class outside of class scope?");
  ClassStack.top()->LateParsedDeclarations.push_back(new LateParsedClass(this, Victim));
  Victim->TemplateScope = getCurScope()->getParent()->isTemplateParamScope();
}

/// \brief Try to parse an 'identifier' which appears within an attribute-token.
///
/// \return the parsed identifier on success, and 0 if the next token is not an
/// attribute-token.
///
/// C++11 [dcl.attr.grammar]p3:
///   If a keyword or an alternative token that satisfies the syntactic
///   requirements of an identifier is contained in an attribute-token,
///   it is considered an identifier.
IdentifierInfo *Parser::TryParseCXX11AttributeIdentifier(SourceLocation &Loc) {
  switch (Tok.getKind()) {
  default:
    // Identifiers and keywords have identifier info attached.
    if (IdentifierInfo *II = Tok.getIdentifierInfo()) {
      Loc = ConsumeToken();
      return II;
    }
    return nullptr;

  case tok::ampamp:       // 'and'
  case tok::pipe:         // 'bitor'
  case tok::pipepipe:     // 'or'
  case tok::caret:        // 'xor'
  case tok::tilde:        // 'compl'
  case tok::amp:          // 'bitand'
  case tok::ampequal:     // 'and_eq'
  case tok::pipeequal:    // 'or_eq'
  case tok::caretequal:   // 'xor_eq'
  case tok::exclaim:      // 'not'
  case tok::exclaimequal: // 'not_eq'
    // Alternative tokens do not have identifier info, but their spelling
    // starts with an alphabetical character.
    SmallString<8> SpellingBuf;
    StringRef Spelling = PP.getSpelling(Tok.getLocation(), SpellingBuf);
    if (isLetter(Spelling[0])) {
      Loc = ConsumeToken();
      return &PP.getIdentifierTable().get(Spelling);
    }
    return nullptr;
  }
}

static bool IsBuiltInOrStandardCXX11Attribute(IdentifierInfo *AttrName,
                                               IdentifierInfo *ScopeName) {
  switch (AttributeList::getKind(AttrName, ScopeName,
                                 AttributeList::AS_CXX11)) {
  case AttributeList::AT_CarriesDependency:
  case AttributeList::AT_Deprecated:
  case AttributeList::AT_FallThrough:
  case AttributeList::AT_CXX11NoReturn: {
    return true;
  }

  default:
    return false;
  }
}

/// ParseCXX11AttributeArgs -- Parse a C++11 attribute-argument-clause.
///
/// [C++11] attribute-argument-clause:
///         '(' balanced-token-seq ')'
///
/// [C++11] balanced-token-seq:
///         balanced-token
///         balanced-token-seq balanced-token
///
/// [C++11] balanced-token:
///         '(' balanced-token-seq ')'
///         '[' balanced-token-seq ']'
///         '{' balanced-token-seq '}'
///         any token but '(', ')', '[', ']', '{', or '}'
bool Parser::ParseCXX11AttributeArgs(IdentifierInfo *AttrName,
                                     SourceLocation AttrNameLoc,
                                     ParsedAttributes &Attrs,
                                     SourceLocation *EndLoc,
                                     IdentifierInfo *ScopeName,
                                     SourceLocation ScopeLoc) {
  assert(Tok.is(tok::l_paren) && "Not a C++11 attribute argument list");
  SourceLocation LParenLoc = Tok.getLocation();

  // If the attribute isn't known, we will not attempt to parse any
  // arguments.
  if (!hasAttribute(AttrSyntax::CXX, ScopeName, AttrName,
                    getTargetInfo().getTriple(), getLangOpts())) {
    // Eat the left paren, then skip to the ending right paren.
    ConsumeParen();
    SkipUntil(tok::r_paren);
    return false;
  }

  if (ScopeName && ScopeName->getName() == "gnu")
    // GNU-scoped attributes have some special cases to handle GNU-specific
    // behaviors.
    ParseGNUAttributeArgs(AttrName, AttrNameLoc, Attrs, EndLoc, ScopeName,
                          ScopeLoc, AttributeList::AS_CXX11, nullptr);
  else {
    unsigned NumArgs =
        ParseAttributeArgsCommon(AttrName, AttrNameLoc, Attrs, EndLoc,
                                 ScopeName, ScopeLoc, AttributeList::AS_CXX11);
    
    const AttributeList *Attr = Attrs.getList();
    if (Attr && IsBuiltInOrStandardCXX11Attribute(AttrName, ScopeName)) {
      // If the attribute is a standard or built-in attribute and we are
      // parsing an argument list, we need to determine whether this attribute
      // was allowed to have an argument list (such as [[deprecated]]), and how
      // many arguments were parsed (so we can diagnose on [[deprecated()]]).
      if (Attr->getMaxArgs() && !NumArgs) {
        // The attribute was allowed to have arguments, but none were provided
        // even though the attribute parsed successfully. This is an error.
        // FIXME: This is a good place for a fixit which removes the parens.
        Diag(LParenLoc, diag::err_attribute_requires_arguments) << AttrName;
        return false;
      } else if (!Attr->getMaxArgs()) {
        // The attribute parsed successfully, but was not allowed to have any
        // arguments. It doesn't matter whether any were provided -- the
        // presence of the argument list (even if empty) is diagnosed.
        Diag(LParenLoc, diag::err_cxx11_attribute_forbids_arguments)
            << AttrName;
        return false;
      }
    }
  }
  return true;
}

/// ParseCXX11AttributeSpecifier - Parse a C++11 attribute-specifier.
///
/// [C++11] attribute-specifier:
///         '[' '[' attribute-list ']' ']'
///         alignment-specifier
///
/// [C++11] attribute-list:
///         attribute[opt]
///         attribute-list ',' attribute[opt]
///         attribute '...'
///         attribute-list ',' attribute '...'
///
/// [C++11] attribute:
///         attribute-token attribute-argument-clause[opt]
///
/// [C++11] attribute-token:
///         identifier
///         attribute-scoped-token
///
/// [C++11] attribute-scoped-token:
///         attribute-namespace '::' identifier
///
/// [C++11] attribute-namespace:
///         identifier
void Parser::ParseCXX11AttributeSpecifier(ParsedAttributes &attrs,
                                          SourceLocation *endLoc) {
  if (Tok.is(tok::kw_alignas)) {
    Diag(Tok.getLocation(), diag::warn_cxx98_compat_alignas);
    ParseAlignmentSpecifier(attrs, endLoc);
    return;
  }

  assert(Tok.is(tok::l_square) && NextToken().is(tok::l_square)
      && "Not a C++11 attribute list");

  Diag(Tok.getLocation(), diag::warn_cxx98_compat_attribute);

  ConsumeBracket();
  ConsumeBracket();

  llvm::SmallDenseMap<IdentifierInfo*, SourceLocation, 4> SeenAttrs;

  while (Tok.isNot(tok::r_square)) {
    // attribute not present
    if (TryConsumeToken(tok::comma))
      continue;

    SourceLocation ScopeLoc, AttrLoc;
    IdentifierInfo *ScopeName = nullptr, *AttrName = nullptr;

    AttrName = TryParseCXX11AttributeIdentifier(AttrLoc);
    if (!AttrName)
      // Break out to the "expected ']'" diagnostic.
      break;

    // scoped attribute
    if (TryConsumeToken(tok::coloncolon)) {
      ScopeName = AttrName;
      ScopeLoc = AttrLoc;

      AttrName = TryParseCXX11AttributeIdentifier(AttrLoc);
      if (!AttrName) {
        Diag(Tok.getLocation(), diag::err_expected) << tok::identifier;
        SkipUntil(tok::r_square, tok::comma, StopAtSemi | StopBeforeMatch);
        continue;
      }
    }

    bool StandardAttr = IsBuiltInOrStandardCXX11Attribute(AttrName, ScopeName);
    bool AttrParsed = false;

    if (StandardAttr &&
        !SeenAttrs.insert(std::make_pair(AttrName, AttrLoc)).second)
      Diag(AttrLoc, diag::err_cxx11_attribute_repeated)
          << AttrName << SourceRange(SeenAttrs[AttrName]);

    // Parse attribute arguments
    if (Tok.is(tok::l_paren))
      AttrParsed = ParseCXX11AttributeArgs(AttrName, AttrLoc, attrs, endLoc,
                                           ScopeName, ScopeLoc);

    if (!AttrParsed)
      attrs.addNew(AttrName,
                   SourceRange(ScopeLoc.isValid() ? ScopeLoc : AttrLoc,
                               AttrLoc),
                   ScopeName, ScopeLoc, nullptr, 0, AttributeList::AS_CXX11);

    if (TryConsumeToken(tok::ellipsis))
      Diag(Tok, diag::err_cxx11_attribute_forbids_ellipsis)
        << AttrName->getName();
  }

  if (ExpectAndConsume(tok::r_square))
    SkipUntil(tok::r_square);
  if (endLoc)
    *endLoc = Tok.getLocation();
  if (ExpectAndConsume(tok::r_square))
    SkipUntil(tok::r_square);
}

/// ParseCXX11Attributes - Parse a C++11 attribute-specifier-seq.
///
/// attribute-specifier-seq:
///       attribute-specifier-seq[opt] attribute-specifier
void Parser::ParseCXX11Attributes(ParsedAttributesWithRange &attrs,
                                  SourceLocation *endLoc) {
  assert(getLangOpts().CPlusPlus11);

  SourceLocation StartLoc = Tok.getLocation(), Loc;
  if (!endLoc)
    endLoc = &Loc;

  do {
    ParseCXX11AttributeSpecifier(attrs, endLoc);
  } while (isCXX11AttributeSpecifier());

  attrs.Range = SourceRange(StartLoc, *endLoc);
}

void Parser::DiagnoseAndSkipCXX11Attributes() {
  // Start and end location of an attribute or an attribute list.
  SourceLocation StartLoc = Tok.getLocation();
  SourceLocation EndLoc = SkipCXX11Attributes();

  if (EndLoc.isValid()) {
    SourceRange Range(StartLoc, EndLoc);
    Diag(StartLoc, diag::err_attributes_not_allowed)
      << Range;
  }
}

SourceLocation Parser::SkipCXX11Attributes() {
  SourceLocation EndLoc;

  if (!isCXX11AttributeSpecifier())
    return EndLoc;

  do {
    if (Tok.is(tok::l_square)) {
      BalancedDelimiterTracker T(*this, tok::l_square);
      T.consumeOpen();
      T.skipToEnd();
      EndLoc = T.getCloseLocation();
    } else {
      assert(Tok.is(tok::kw_alignas) && "not an attribute specifier");
      ConsumeToken();
      BalancedDelimiterTracker T(*this, tok::l_paren);
      if (!T.consumeOpen())
        T.skipToEnd();
      EndLoc = T.getCloseLocation();
    }
  } while (isCXX11AttributeSpecifier());

  return EndLoc;
}

/// ParseMicrosoftAttributes - Parse a Microsoft attribute [Attr]
///
/// [MS] ms-attribute:
///             '[' token-seq ']'
///
/// [MS] ms-attribute-seq:
///             ms-attribute[opt]
///             ms-attribute ms-attribute-seq
void Parser::ParseMicrosoftAttributes(ParsedAttributes &attrs,
                                      SourceLocation *endLoc) {
  assert(Tok.is(tok::l_square) && "Not a Microsoft attribute list");

  while (Tok.is(tok::l_square)) {
    // FIXME: If this is actually a C++11 attribute, parse it as one.
    ConsumeBracket();
    SkipUntil(tok::r_square, StopAtSemi | StopBeforeMatch);
    if (endLoc) *endLoc = Tok.getLocation();
    ExpectAndConsume(tok::r_square);
  }
}

void Parser::ParseMicrosoftIfExistsClassDeclaration(DeclSpec::TST TagType,
                                                    AccessSpecifier& CurAS) {
  IfExistsCondition Result;
  if (ParseMicrosoftIfExistsCondition(Result))
    return;
  
  BalancedDelimiterTracker Braces(*this, tok::l_brace);
  if (Braces.consumeOpen()) {
    Diag(Tok, diag::err_expected) << tok::l_brace;
    return;
  }

  switch (Result.Behavior) {
  case IEB_Parse:
    // Parse the declarations below.
    break;
        
  case IEB_Dependent:
    Diag(Result.KeywordLoc, diag::warn_microsoft_dependent_exists)
      << Result.IsIfExists;
    // Fall through to skip.
      
  case IEB_Skip:
    Braces.skipToEnd();
    return;
  }

  while (Tok.isNot(tok::r_brace) && !isEofOrEom()) {
    // __if_exists, __if_not_exists can nest.
    if ((Tok.is(tok::kw___if_exists) || Tok.is(tok::kw___if_not_exists))) {
      ParseMicrosoftIfExistsClassDeclaration((DeclSpec::TST)TagType, CurAS);
      continue;
    }

    // Check for extraneous top-level semicolon.
    if (Tok.is(tok::semi)) {
      ConsumeExtraSemi(InsideStruct, TagType);
      continue;
    }

    AccessSpecifier AS = getAccessSpecifierIfPresent();
    if (AS != AS_none) {
      // Current token is a C++ access specifier.
      CurAS = AS;
      SourceLocation ASLoc = Tok.getLocation();
      ConsumeToken();
      if (Tok.is(tok::colon))
        Actions.ActOnAccessSpecifier(AS, ASLoc, Tok.getLocation());
      else
        Diag(Tok, diag::err_expected) << tok::colon;
      ConsumeToken();
      continue;
    }

    // Parse all the comma separated declarators.
    ParseCXXClassMemberDeclaration(CurAS, nullptr);
  }
  
  Braces.consumeClose();
}
