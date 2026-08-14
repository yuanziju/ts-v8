#include "src/ts/ts-parser.h"

#include "src/ast/ast.h"
#include "src/ast/ast-value-factory.h"
#include "src/ast/scopes.h"
#include "src/common/globals.h"
#include "src/common/message-template.h"
#include "src/objects/scope-info.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/scanner.h"
#include "src/parsing/token.h"
#include "src/zone/zone-list-inl.h"

namespace v8 {
namespace internal {
namespace ts {

namespace {

V8_INLINE bool IsContextualKeyword(Scanner* scanner, AstValueFactory* factory,
                                   const char* keyword) {
  if (scanner->peek() != Token::kIdentifier) return false;
  if (scanner->next_literal_contains_escapes()) return false;
  return scanner->CurrentSymbol(factory)->IsOneByteEqualTo(keyword);
}

V8_INLINE bool CheckContextualKeywordStr(Parser* parser, Scanner* scanner,
                                         AstValueFactory* factory,
                                         const char* keyword) {
  if (IsContextualKeyword(scanner, factory, keyword)) {
    parser->Next();
    return true;
  }
  return false;
}

V8_INLINE bool PeekContextualKeywordStr(Scanner* scanner,
                                        AstValueFactory* factory,
                                        const char* keyword) {
  return IsContextualKeyword(scanner, factory, keyword);
}

}  // namespace

TSParser::TSParser(LocalIsolate* local_isolate, ParseInfo* info)
    : Parser(local_isolate, info),
      type_system_(zone()),
      is_typescript_(true),
      in_type_annotation_(false),
      parse_depth_(0),
      allow_decorators_(true),
      allow_enums_(true),
      allow_namespaces_(true),
      decorators_(0, zone()) {
  type_system_.Initialize(zone());
}

TSParser::~TSParser() {}

void TSParser::ParseProgram(
    Isolate* isolate, DirectHandle<Script> script, ParseInfo* info,
    MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info) {
  type_system_.Initialize(zone());
  Parser::ParseProgram(isolate, script, info, maybe_outer_scope_info);
}

void TSParser::ParseFunction(Isolate* isolate, ParseInfo* info,
                              DirectHandle<SharedFunctionInfo> shared_info) {
  type_system_.Initialize(zone());
  Parser::ParseFunction(isolate, info, shared_info);
}

void TSParser::ParseTypeAnnotation(TSType** out_type) {
  if (out_type == nullptr) return;
  *out_type = nullptr;

  if (peek() == Token::kColon) {
    Next();
    in_type_annotation_ = true;
    TSType* type = ParseUnionType();
    in_type_annotation_ = false;
    *out_type = type;
  }
}

TSType* TSParser::ParseTypeReference() {
  const AstRawString* name = GetIdentifier();
  const char* type_name = reinterpret_cast<const char*>(name->raw_data());

  TSType* type = type_system_.LookupType(type_name);
  if (type == nullptr) {
    type = type_system_.NewTypeReference(type_name);
  }

  if (peek() == Token::kLessThan) {
    Next();
    ZoneList<TSType*>* type_args = zone()->New<ZoneList<TSType*>>(0, zone());
    while (peek() != Token::kGreaterThan && peek() != Token::kEos) {
      TSType* arg_type = ParseUnionType();
      if (arg_type != nullptr) {
        type_args->Add(arg_type, zone());
      }
      if (!Check(Token::kComma)) break;
    }
    Expect(Token::kGreaterThan);
    for (int i = 0; i < type_args->length(); i++) {
      type->type_arguments()->Add(type_args->at(i), zone());
    }
  }

  while (Check(Token::kLeftBracket)) {
    Expect(Token::kRightBracket);
    type = type_system_.NewArray(type);
  }

  return type;
}

TSType* TSParser::ParseUnionType() {
  TSType* left = ParseIntersectionType();
  if (left == nullptr) return nullptr;

  ZoneList<TSType*>* types = zone()->New<ZoneList<TSType*>>(0, zone());
  types->Add(left, zone());

  while (Check(Token::kBitOr)) {
    TSType* right = ParseIntersectionType();
    if (right != nullptr) {
      types->Add(right, zone());
    }
  }

  if (types->length() == 1) return left;
  return type_system_.NewUnion(types);
}

TSType* TSParser::ParseIntersectionType() {
  TSType* left = ParseArrayType();
  if (left == nullptr) return nullptr;

  ZoneList<TSType*>* types = zone()->New<ZoneList<TSType*>>(0, zone());
  types->Add(left, zone());

  while (Check(Token::kBitAnd)) {
    TSType* right = ParseArrayType();
    if (right != nullptr) {
      types->Add(right, zone());
    }
  }

  if (types->length() == 1) return left;
  return type_system_.NewIntersection(types);
}

TSType* TSParser::ParseArrayType() {
  TSType* base = ParseTupleType();
  if (base == nullptr) return nullptr;

  while (Check(Token::kLeftBracket)) {
    Expect(Token::kRightBracket);
    base = type_system_.NewArray(base);
  }

  return base;
}

TSType* TSParser::ParseTupleType() {
  if (peek() == Token::kLeftBracket) {
    Next();
    ZoneList<TSType*>* elem_types = zone()->New<ZoneList<TSType*>>(0, zone());
    while (peek() != Token::kRightBracket && peek() != Token::kEos) {
      if (peek() == Token::kEllipsis) {
        Next();
        TSType* rest_type = ParseUnionType();
        if (rest_type != nullptr) {
          elem_types->Add(rest_type, zone());
        }
        break;
      }
      TSType* elem = ParseUnionType();
      if (elem != nullptr) {
        elem_types->Add(elem, zone());
      }
      if (!Check(Token::kComma)) break;
    }
    Expect(Token::kRightBracket);

    TSType* tuple = zone()->New<TSType>(zone(), TypeKind::kTuple);
    for (int i = 0; i < elem_types->length(); i++) {
      tuple->union_types()->Add(elem_types->at(i), zone());
    }
    return tuple;
  }
  return ParseFunctionType();
}

TSType* TSParser::ParseFunctionType() {
  if (peek() == Token::kLessThan) {
    Next();
    ZoneList<TypeParameter*>* type_params =
        zone()->New<ZoneList<TypeParameter*>>(0, zone());
    ParseTypeParameterDeclaration(type_params);
    Expect(Token::kGreaterThan);
  }

  if (peek() == Token::kLeftParen) {
    int save_depth = parse_depth_;
    parse_depth_++;

    Expect(Token::kLeftParen);
    ZoneList<TSType*>* param_types = zone()->New<ZoneList<TSType*>>(0, zone());
    while (peek() != Token::kRightParen && peek() != Token::kEos) {
      if (peek() == Token::kEllipsis) {
        Next();
        GetIdentifier();
        TSType* rest_type = ParseUnionType();
        if (rest_type != nullptr) {
          param_types->Add(rest_type, zone());
        }
        break;
      }

      if (peek() == Token::kIdentifier || peek() == Token::kThis) {
        Next();
      }
      Expect(Token::kColon);
      TSType* param_type = ParseUnionType();
      if (param_type != nullptr) {
        param_types->Add(param_type, zone());
      }
      if (!Check(Token::kComma)) break;
    }
    Expect(Token::kRightParen);

    if (Check(Token::kLessThan)) {
      Next();
      while (peek() != Token::kGreaterThan && peek() != Token::kEos) {
        ParseUnionType();
        Check(Token::kComma);
      }
      Expect(Token::kGreaterThan);
    }

    Expect(Token::kArrow);
    TSType* return_type = ParseUnionType();
    parse_depth_ = save_depth;

    TSType* func_type = zone()->New<TSType>(zone(), TypeKind::kFunction);
    func_type->set_referenced_type(return_type);
    return func_type;
  }

  return ParseLiteralType();
}

TSType* TSParser::ParseLiteralType() {
  Token::Value token = peek();
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  if (token == Token::kString) {
    Next();
    return type_system_.NewLiteral("string");
  }
  if (token == Token::kNumber || token == Token::kSmi) {
    Next();
    return type_system_.NewLiteral("number");
  }
  if (token == Token::kTrueLiteral || token == Token::kFalseLiteral) {
    Next();
    return type_system_.NewLiteral("boolean");
  }
  if (token == Token::kNullLiteral) {
    Next();
    return type_system_.NewNull();
  }
  if (token == Token::kThis) {
    return ParseThisType();
  }
  if (token == Token::kVoid) {
    Next();
    return type_system_.NewVoid();
  }

  if (token == Token::kIdentifier) {
    if (PeekContextualKeywordStr(sc, avf, "keyof")) {
      Next();
      return ParseKeyofType();
    }
    if (PeekContextualKeywordStr(sc, avf, "typeof")) {
      Next();
      return ParseIndexedAccessType();
    }
    if (PeekContextualKeywordStr(sc, avf, "unknown")) {
      Next();
      return type_system_.NewUnknown();
    }
    if (PeekContextualKeywordStr(sc, avf, "never")) {
      Next();
      return type_system_.NewNever();
    }
    if (PeekContextualKeywordStr(sc, avf, "any")) {
      Next();
      return type_system_.NewAny();
    }
    if (PeekContextualKeywordStr(sc, avf, "boolean")) {
      Next();
      return type_system_.NewBoolean();
    }
    if (PeekContextualKeywordStr(sc, avf, "number")) {
      Next();
      return type_system_.NewNumber();
    }
    if (PeekContextualKeywordStr(sc, avf, "string")) {
      Next();
      return type_system_.NewString();
    }
    if (PeekContextualKeywordStr(sc, avf, "symbol")) {
      Next();
      return type_system_.NewSymbol();
    }
    if (PeekContextualKeywordStr(sc, avf, "undefined")) {
      Next();
      return type_system_.NewUndefined();
    }
    if (PeekContextualKeywordStr(sc, avf, "object")) {
      Next();
      return type_system_.NewObject();
    }
    if (PeekContextualKeywordStr(sc, avf, "const")) {
      Next();
      return ParseLiteralType();
    }
  }

  return ParseTypeReference();
}

TSType* TSParser::ParseParenthesizedType() {
  Expect(Token::kLeftParen);
  TSType* type = ParseUnionType();
  Expect(Token::kRightParen);
  return type;
}

TSType* TSParser::ParseIndexedAccessType() {
  TSType* object_type = ParseTypeReference();
  if (object_type == nullptr) return nullptr;

  if (Check(Token::kLeftBracket)) {
    TSType* index_type = ParseUnionType();
    Expect(Token::kRightBracket);

    TSType* result = zone()->New<TSType>(zone(), TypeKind::kIndexedAccess);
    result->set_referenced_type(object_type);
    result->set_element_type(index_type);
    return result;
  }

  return object_type;
}

TSType* TSParser::ParseConditionalType() {
  TSType* check_type = ParseTypeReference();
  if (check_type == nullptr) return nullptr;

  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  if (IsContextualKeyword(sc, avf, "extends") &&
      !sc->HasLineTerminatorAfterNext()) {
    Next();
    TSType* extends_type = ParseTypeReference();
    Expect(Token::kConditional);
    TSType* true_type = ParseUnionType();
    Expect(Token::kColon);
    TSType* false_type = ParseUnionType();

    return type_system_.NewConditional(check_type, extends_type,
                                       true_type, false_type);
  }

  return check_type;
}

TSType* TSParser::ParseMappedType() {
  Expect(Token::kLeftBrace);
  TSType* result = zone()->New<TSType>(zone(), TypeKind::kMapped);
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  while (peek() != Token::kRightBrace && peek() != Token::kEos) {
    if (CheckContextualKeywordStr(this, sc, avf, "readonly")) {
    }

    if (peek() == Token::kLeftBracket) {
      Next();
      if (CheckContextualKeywordStr(this, sc, avf, "keyof")) {
        Next();
        ParseTypeReference();
      } else {
        ParseTypeReference();
      }
      Expect(Token::kRightBracket);
      Expect(Token::kColon);
      ParseUnionType();
    } else {
      GetIdentifier();
      if (Check(Token::kQuestion)) {
      }
      Expect(Token::kColon);
      ParseUnionType();
    }

    Check(Token::kSemicolon);
  }

  Expect(Token::kRightBrace);
  return result;
}

TSType* TSParser::ParseKeyofType() {
  TSType* source = ParseTypeReference();
  return type_system_.NewKeyof(source);
}

TSType* TSParser::ParseThisType() {
  Expect(Token::kThis);
  return type_system_.NewThis();
}

void TSParser::ParseTypeParameterDeclaration(ZoneList<TypeParameter*>* params) {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  while (peek() != Token::kGreaterThan && peek() != Token::kEos) {
    const AstRawString* name = GetIdentifier();
    const char* param_name = reinterpret_cast<const char*>(name->raw_data());
    TypeParameter* param = type_system_.NewTypeParameter(param_name);

    if (IsContextualKeyword(sc, avf, "extends") &&
        !sc->HasLineTerminatorAfterNext()) {
      Next();
      TSType* constraint = ParseUnionType();
      param->set_constraint(constraint);
    }

    if (Check(Token::kAssign)) {
      TSType* default_type = ParseUnionType();
      param->set_default_type(default_type);
    }

    params->Add(param, zone());

    if (!Check(Token::kComma)) break;
  }
}

void TSParser::ParseInterfaceDeclaration() {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  const AstRawString* name = GetIdentifier();
  const char* interface_name = reinterpret_cast<const char*>(name->raw_data());

  ZoneList<TypeParameter*>* type_params = nullptr;
  if (peek() == Token::kLessThan) {
    Next();
    type_params = zone()->New<ZoneList<TypeParameter*>>(0, zone());
    ParseTypeParameterDeclaration(type_params);
    Expect(Token::kGreaterThan);
  }

  if (IsContextualKeyword(sc, avf, "extends") &&
      !sc->HasLineTerminatorAfterNext()) {
    Next();
    while (true) {
      ParseTypeReference();
      if (!Check(Token::kComma)) break;
    }
  }

  Expect(Token::kLeftBrace);

  ZoneList<PropertyDescriptor>* properties =
      zone()->New<ZoneList<PropertyDescriptor>>(0, zone());

  while (peek() != Token::kRightBrace && peek() != Token::kEos) {
    bool is_readonly = false;
    bool is_public = true;
    bool is_private = false;
    bool is_protected = false;

    if (CheckContextualKeywordStr(this, sc, avf, "readonly")) {
      is_readonly = true;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "public")) {
      is_public = true;
      is_private = false;
      is_protected = false;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "private")) {
      is_private = true;
      is_public = false;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "protected")) {
      is_protected = true;
      is_public = false;
    }

    if (peek() == Token::kIdentifier || peek() == Token::kPrivateName ||
        peek() == Token::kLeftBracket) {
      const char* prop_name = nullptr;
      bool is_computed = Check(Token::kLeftBracket);
      if (is_computed) {
        ParseExpression();
        Expect(Token::kRightBracket);
      } else {
        const AstRawString* id = GetIdentifier();
        prop_name = reinterpret_cast<const char*>(id->raw_data());
      }

      bool is_optional = false;
      if (Check(Token::kQuestion)) {
        is_optional = true;
      }

      TSType* prop_type = nullptr;

      if (peek() == Token::kLessThan) {
        Next();
        ZoneList<TypeParameter*>* tp =
            zone()->New<ZoneList<TypeParameter*>>(0, zone());
        ParseTypeParameterDeclaration(tp);
        Expect(Token::kGreaterThan);
      }

      if (peek() == Token::kLeftParen) {
        Expect(Token::kLeftParen);
        while (peek() != Token::kRightParen && peek() != Token::kEos) {
          GetIdentifier();
          if (Check(Token::kColon)) {
            ParseUnionType();
          }
          Check(Token::kComma);
        }
        Expect(Token::kRightParen);

        if (Check(Token::kColon)) {
          prop_type = ParseUnionType();
        }
      } else if (Check(Token::kColon)) {
        prop_type = ParseUnionType();
      }

      if (prop_name != nullptr) {
        PropertyDescriptor prop;
        prop.name = prop_name;
        prop.type = prop_type;
        prop.is_readonly = is_readonly;
        prop.is_optional = is_optional;
        prop.is_public = is_public;
        prop.is_private = is_private;
        prop.is_protected = is_protected;
        prop.has_readonly_modifier = is_readonly;
        properties->Add(prop, zone());
      }
    } else {
      break;
    }

    Check(Token::kSemicolon);
  }

  Expect(Token::kRightBrace);
  type_system_.RegisterInterface(interface_name, properties, type_params);
}

void TSParser::ParseEnumDeclaration() {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  const AstRawString* name = GetIdentifier();
  const char* enum_name = reinterpret_cast<const char*>(name->raw_data());

  Expect(Token::kLeftBrace);

  ZoneList<const char*>* members =
      zone()->New<ZoneList<const char*>>(0, zone());

  while (peek() != Token::kRightBrace && peek() != Token::kEos) {
    if (CheckContextualKeywordStr(this, sc, avf, "const")) {
    }

    if (peek() == Token::kIdentifier) {
      const AstRawString* member_id = GetIdentifier();
      const char* member_name =
          reinterpret_cast<const char*>(member_id->raw_data());
      members->Add(member_name, zone());
    } else if (peek() == Token::kString || peek() == Token::kNumber ||
               peek() == Token::kSmi) {
      Next();
    } else {
      break;
    }

    if (Check(Token::kAssign)) {
      if (peek() == Token::kString || peek() == Token::kNumber ||
          peek() == Token::kSmi) {
        Next();
      } else {
        ParseExpression();
      }
    }

    Check(Token::kComma);
    Check(Token::kSemicolon);
  }

  Expect(Token::kRightBrace);
  type_system_.RegisterEnum(enum_name, members);
}

void TSParser::ParseNamespaceDeclaration() {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  const AstRawString* name = GetIdentifier();
  USE(name);

  while (Check(Token::kPeriod)) {
    GetIdentifier();
  }

  Expect(Token::kLeftBrace);

  while (peek() != Token::kRightBrace && peek() != Token::kEos) {
    if (CheckContextualKeywordStr(this, sc, avf, "export")) {
    }
    if (CheckContextualKeywordStr(this, sc, avf, "declare")) {
    }

    switch (peek()) {
      case Token::kFunction:
        ParseHoistableDeclaration(nullptr, false);
        break;
      case Token::kClass:
        Consume(Token::kClass);
        ParseClassDeclaration(nullptr, false);
        break;
      case Token::kVar:
      case Token::kLet:
      case Token::kConst:
        ParseVariableStatement(kStatementListItem, nullptr);
        break;
      default:
        if (CheckContextualKeywordStr(this, sc, avf, "interface")) {
          ParseInterfaceDeclaration();
        } else if (CheckContextualKeywordStr(this, sc, avf, "type")) {
          ParseTypeAliasDeclaration();
        } else if (CheckContextualKeywordStr(this, sc, avf, "enum")) {
          ParseEnumDeclaration();
        } else if (CheckContextualKeywordStr(this, sc, avf, "namespace")) {
          ParseNamespaceDeclaration();
        } else if (peek() == Token::kLeftBrace) {
          Next();
          while (peek() != Token::kRightBrace && peek() != Token::kEos) {
            Next();
          }
          Expect(Token::kRightBrace);
        } else {
          Next();
        }
        break;
    }
  }

  Expect(Token::kRightBrace);
}

void TSParser::ParseTypeAliasDeclaration() {
  const AstRawString* name = GetIdentifier();
  const char* alias_name = reinterpret_cast<const char*>(name->raw_data());

  ZoneList<TypeParameter*>* type_params = nullptr;
  if (peek() == Token::kLessThan) {
    Next();
    type_params = zone()->New<ZoneList<TypeParameter*>>(0, zone());
    ParseTypeParameterDeclaration(type_params);
    Expect(Token::kGreaterThan);
  }

  Expect(Token::kAssign);
  TSType* type = ParseUnionType();
  if (type != nullptr) {
    type->set_name(alias_name);
  }
  type_system_.RegisterInterface(alias_name, type ? type->properties() : nullptr, type_params);
}

void TSParser::ParseDecorator() {
  ParseExpression();
}

Statement* TSParser::ParseTSStatement() {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  if (CheckContextualKeywordStr(this, sc, avf, "declare")) {
    return ParseTSStatement();
  }

  if (CheckContextualKeywordStr(this, sc, avf, "export")) {
    if (CheckContextualKeywordStr(this, sc, avf, "default")) {
    }
    return ParseTSStatement();
  }

  if (CheckContextualKeywordStr(this, sc, avf, "import")) {
    ParseTSImportDeclaration();
    return nullptr;
  }

  if (CheckContextualKeywordStr(this, sc, avf, "interface")) {
    ParseInterfaceDeclaration();
    return nullptr;
  }

  if (CheckContextualKeywordStr(this, sc, avf, "type")) {
    ParseTypeAliasDeclaration();
    return nullptr;
  }

  if (CheckContextualKeywordStr(this, sc, avf, "enum")) {
    ParseEnumDeclaration();
    return nullptr;
  }

  if (CheckContextualKeywordStr(this, sc, avf, "namespace")) {
    ParseNamespaceDeclaration();
    return nullptr;
  }

  switch (peek()) {
    case Token::kFunction:
      return ParseHoistableDeclaration(nullptr, false);
    case Token::kClass:
      Consume(Token::kClass);
      return ParseClassDeclaration(nullptr, false);
    case Token::kVar:
      return ParseVariableStatement(kStatementListItem, nullptr);
    case Token::kConst:
      return ParseVariableStatement(kStatementListItem, nullptr);
    case Token::kLet:
      if (IsNextLetKeyword()) {
        return ParseVariableStatement(kStatementListItem, nullptr);
      }
      break;
    case Token::kAsync:
      if (PeekAhead() == Token::kFunction &&
          !sc->HasLineTerminatorAfterNext()) {
        Consume(Token::kAsync);
        return ParseAsyncFunctionDeclaration(nullptr, false);
      }
      break;
    default:
      break;
  }

  return Parser::ParseStatement(nullptr, nullptr,
                               kAllowLabelledFunctionStatement);
}

Expression* TSParser::ParseTSExpression() {
  Expression* expr = Parser::ParseExpression();
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  while (true) {
    if (IsContextualKeyword(sc, avf, "as") &&
        !sc->HasLineTerminatorAfterNext()) {
      Next();
      expr = ParseAsExpression(expr);
    } else if (IsContextualKeyword(sc, avf, "satisfies") &&
               !sc->HasLineTerminatorAfterNext()) {
      Next();
      expr = ParseSatisfiesExpression(expr);
    } else if (peek() == Token::kNot &&
               !sc->HasLineTerminatorAfterNext()) {
      Next();
    } else {
      break;
    }
  }

  return expr;
}

Expression* TSParser::ParseAsExpression(Expression* expr) {
  in_type_annotation_ = true;
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  TSType* type = nullptr;

  if (IsContextualKeyword(sc, avf, "keyof")) {
    Next();
    type = ParseKeyofType();
  } else if (peek() == Token::kLeftParen) {
    type = ParseParenthesizedType();
  } else {
    type = ParseUnionType();
  }

  in_type_annotation_ = false;

  if (type != nullptr && expr != nullptr) {
    CheckTypeAnnotation(type, expr);
  }

  return expr;
}

Expression* TSParser::ParseSatisfiesExpression(Expression* expr) {
  in_type_annotation_ = true;
  TSType* type = ParseUnionType();
  in_type_annotation_ = false;

  if (type != nullptr && expr != nullptr) {
    CheckTypeAnnotation(type, expr);
  }

  return expr;
}

Statement* TSParser::ParseClassDeclaration(Expression* maybe_name,
                                          bool is_export) {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  const AstRawString* name = GetIdentifier();
  const char* class_name = reinterpret_cast<const char*>(name->raw_data());

  ZoneList<TypeParameter*>* type_params = nullptr;
  if (peek() == Token::kLessThan) {
    Next();
    type_params = zone()->New<ZoneList<TypeParameter*>>(0, zone());
    ParseTypeParameterDeclaration(type_params);
    Expect(Token::kGreaterThan);
  }

  if (IsContextualKeyword(sc, avf, "extends") &&
      !sc->HasLineTerminatorAfterNext()) {
    Next();
    while (true) {
      ParseTypeReference();
      if (!Check(Token::kComma)) break;
    }
  }

  if (IsContextualKeyword(sc, avf, "implements") &&
      !sc->HasLineTerminatorAfterNext()) {
    Next();
    while (true) {
      ParseTypeReference();
      if (!Check(Token::kComma)) break;
    }
  }

  Expect(Token::kLeftBrace);

  ZoneList<PropertyDescriptor>* props =
      zone()->New<ZoneList<PropertyDescriptor>>(0, zone());
  ParseClassMembersWithTypes(class_name, type_params, props);

  Expect(Token::kRightBrace);

  return nullptr;
}

void TSParser::ParseClassMembersWithTypes(
    const char* class_name,
    ZoneList<TypeParameter*>* type_params,
    ZoneList<PropertyDescriptor>* props) {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  while (peek() != Token::kRightBrace && peek() != Token::kEos) {
    bool is_readonly = false;
    bool is_public = true;
    bool is_private = false;
    bool is_protected = false;

    if (CheckContextualKeywordStr(this, sc, avf, "readonly")) {
      is_readonly = true;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "public")) {
      is_public = true;
      is_private = false;
      is_protected = false;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "private")) {
      is_private = true;
      is_public = false;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "protected")) {
      is_protected = true;
      is_public = false;
    }
    if (CheckContextualKeywordStr(this, sc, avf, "abstract")) {
    }
    if (CheckContextualKeywordStr(this, sc, avf, "override")) {
    }

    if (peek() == Token::kStatic) {
      Next();
    }

    if (peek() == Token::kIdentifier || peek() == Token::kPrivateName ||
        peek() == Token::kGet || peek() == Token::kSet) {
      const AstRawString* id = GetIdentifier();
      const char* prop_name = reinterpret_cast<const char*>(id->raw_data());

      bool is_optional = false;
      if (Check(Token::kQuestion)) {
        is_optional = true;
      }

      TSType* prop_type = nullptr;

      if (Check(Token::kColon)) {
        prop_type = ParseUnionType();
      }

      if (peek() == Token::kLeftParen) {
        Expect(Token::kLeftParen);
        while (peek() != Token::kRightParen && peek() != Token::kEos) {
          GetIdentifier();
          if (Check(Token::kColon)) {
            ParseUnionType();
          }
          Check(Token::kComma);
        }
        Expect(Token::kRightParen);

        if (Check(Token::kColon)) {
          prop_type = ParseUnionType();
        }

        if (peek() == Token::kLeftBrace) {
          ParseBlock(nullptr);
        }
      } else if (peek() == Token::kAssign) {
        Next();
        ParseExpression();
      }

      if (prop_name != nullptr) {
        PropertyDescriptor prop;
        prop.name = prop_name;
        prop.type = prop_type;
        prop.is_readonly = is_readonly;
        prop.is_optional = is_optional;
        prop.is_public = is_public;
        prop.is_private = is_private;
        prop.is_protected = is_protected;
        prop.has_readonly_modifier = is_readonly;
        props->Add(prop, zone());
      }
    } else {
      Next();
    }

    Check(Token::kSemicolon);
  }

  type_system_.RegisterClass(class_name, props, type_params);
}

void TSParser::CheckTypeAnnotation(TSType* annotated_type,
                                    Expression* value) {
  if (annotated_type == nullptr || value == nullptr) return;

  TSType* inferred = InferTypeFromExpression(value);
  if (inferred == nullptr) return;

  if (!type_system_.IsAssignableTo(inferred, annotated_type)) {
    ReportMessageAt(scanner()->location(),
                    MessageTemplate::kInvalidOrUnexpectedToken);
  }
}

TSType* TSParser::InferTypeFromExpression(Expression* expr) {
  if (expr == nullptr) return nullptr;

  if (expr->IsLiteral()) {
    Literal* lit = expr->AsLiteral();
    if (lit->IsRawString()) return type_system_.NewString();
    if (lit->IsNumber()) return type_system_.NewNumber();
    if (lit->type() == Literal::kBoolean) return type_system_.NewBoolean();
    if (lit->type() == Literal::kNull) return type_system_.NewNull();
  }

  if (expr->IsNullLiteral()) return type_system_.NewNull();
  if (expr->IsBooleanLiteral()) return type_system_.NewBoolean();

  if (expr->IsUnaryOperation()) {
    UnaryOperation* unary = expr->AsUnaryOperation();
    Token::Value op = unary->op();
    if (op == Token::kAdd || op == Token::kSub || op == Token::kBitNot) {
      return type_system_.NewNumber();
    }
    if (op == Token::kNot) {
      return type_system_.NewBoolean();
    }
    if (op == Token::kTypeOf) {
      return type_system_.NewString();
    }
    if (op == Token::kVoid) {
      return type_system_.NewVoid();
    }
  }

  if (expr->IsBinaryOperation()) {
    BinaryOperation* bin = expr->AsBinaryOperation();
    Token::Value op = bin->op();
    if (Token::IsCompareOp(op)) {
      return type_system_.NewBoolean();
    }
    if (op == Token::kBitOr || op == Token::kBitXor || op == Token::kBitAnd ||
        op == Token::kShl || op == Token::kSar || op == Token::kShr) {
      return type_system_.NewNumber();
    }
    if (op == Token::kAdd) {
      Expression* left = bin->left();
      if (left->IsLiteral() && left->AsLiteral()->IsRawString()) {
        return type_system_.NewString();
      }
      return type_system_.NewNumber();
    }
  }

  if (expr->IsConditional()) {
    Conditional* cond = expr->AsConditional();
    return InferTypeFromExpression(cond->then_expression());
  }

  if (expr->IsFunctionLiteral()) {
    return zone()->New<TSType>(zone(), TypeKind::kFunction);
  }

  if (expr->IsArrayLiteral()) {
    ArrayLiteral* arr = expr->AsArrayLiteral();
    const ZonePtrList<Expression>* values = arr->values();
    if (values->is_empty()) {
      return type_system_.NewArray(type_system_.NewAny());
    }
    Expression* first = values->at(0);
    TSType* elem_type = InferTypeFromExpression(first);
    if (elem_type == nullptr) elem_type = type_system_.NewAny();
    return type_system_.NewArray(elem_type);
  }

  if (expr->IsObjectLiteral()) {
    return type_system_.NewObject();
  }

  if (expr->IsVariableProxy()) {
    return type_system_.NewAny();
  }

  if (expr->IsProperty()) {
    return type_system_.NewAny();
  }

  if (expr->IsCall()) {
    return type_system_.NewAny();
  }

  return type_system_.NewAny();
}

void TSParser::ParseTSImportDeclaration() {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  if (peek() == Token::kString) {
    Next();
    ExpectSemicolon();
    return;
  }

  if (CheckContextualKeywordStr(this, sc, avf, "default")) {
    ParseExpression();
    if (Check(Token::kComma)) {
      if (peek() == Token::kStar) {
        Next();
        if (IsContextualKeyword(sc, avf, "as")) {
          Next();
          if (peek() == Token::kIdentifier) {
            Next();
          }
        }
      }
    }
    Expect(Token::kIdentifier);
    Next();
    ExpectSemicolon();
    return;
  }

  if (peek() == Token::kIdentifier) {
    GetIdentifier();
    if (Check(Token::kComma)) {
      if (peek() == Token::kStar) {
        Next();
        if (IsContextualKeyword(sc, avf, "as")) {
          Next();
          if (peek() == Token::kIdentifier) {
            Next();
          }
        }
      }
    }
    Expect(Token::kIdentifier);
    Next();
    ExpectSemicolon();
    return;
  }

  if (peek() == Token::kStar) {
    Next();
    if (IsContextualKeyword(sc, avf, "as")) {
      Next();
      if (peek() == Token::kIdentifier) {
        Next();
      }
    }
    Expect(Token::kIdentifier);
    Next();
    ExpectSemicolon();
    return;
  }

  if (peek() == Token::kLeftBrace) {
    Next();
    while (peek() != Token::kRightBrace && peek() != Token::kEos) {
      if (peek() == Token::kIdentifier) {
        GetIdentifier();
        if (IsContextualKeyword(sc, avf, "as")) {
          Next();
          if (peek() == Token::kIdentifier) {
            Next();
          }
        }
      }
      Check(Token::kComma);
    }
    Expect(Token::kRightBrace);
    Expect(Token::kIdentifier);
    Next();
    ExpectSemicolon();
    return;
  }

  Next();
  ExpectSemicolon();
}

void TSParser::ParseTSExportDeclaration() {
  AstValueFactory* avf = ast_value_factory();
  Scanner* sc = scanner();

  if (Check(Token::kDefault)) {
    switch (peek()) {
      case Token::kFunction:
        ParseHoistableDeclaration(nullptr, false);
        break;
      case Token::kClass:
        Consume(Token::kClass);
        ParseClassDeclaration(nullptr, false);
        break;
      case Token::kIdentifier:
        Next();
        Expect(Token::kAssign);
        ParseExpression();
        ExpectSemicolon();
        break;
      default:
        break;
    }
    return;
  }

  if (peek() == Token::kStar) {
    Next();
    if (IsContextualKeyword(sc, avf, "as")) {
      Next();
      if (peek() == Token::kIdentifier) {
        Next();
      }
    }
    Expect(Token::kIdentifier);
    Next();
    ExpectSemicolon();
    return;
  }

  if (peek() == Token::kLeftBrace) {
    Next();
    while (peek() != Token::kRightBrace && peek() != Token::kEos) {
      if (peek() == Token::kIdentifier) {
        GetIdentifier();
        if (IsContextualKeyword(sc, avf, "as")) {
          Next();
          if (peek() == Token::kIdentifier) {
            Next();
          }
        }
      }
      Check(Token::kComma);
    }
    Expect(Token::kRightBrace);
    if (peek() == Token::kIdentifier) {
      Next();
    }
    ExpectSemicolon();
    return;
  }

  switch (peek()) {
    case Token::kFunction:
      ParseHoistableDeclaration(nullptr, false);
      break;
    case Token::kClass:
      Consume(Token::kClass);
      ParseClassDeclaration(nullptr, false);
      break;
    case Token::kVar:
    case Token::kLet:
    case Token::kConst:
      ParseVariableStatement(kStatementListItem, nullptr);
      break;
    default:
      if (CheckContextualKeywordStr(this, sc, avf, "interface")) {
        ParseInterfaceDeclaration();
      } else if (CheckContextualKeywordStr(this, sc, avf, "type")) {
        ParseTypeAliasDeclaration();
      } else if (CheckContextualKeywordStr(this, sc, avf, "enum")) {
        ParseEnumDeclaration();
      } else if (CheckContextualKeywordStr(this, sc, avf, "namespace")) {
        ParseNamespaceDeclaration();
      } else {
        Next();
      }
      break;
  }
}

}  // namespace ts
}  // namespace internal
}  // namespace v8