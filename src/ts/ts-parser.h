#ifndef V8_TS_TS_PARSER_H_
#define V8_TS_TS_PARSER_H_

#include "src/parsing/parser-base.h"
#include "src/parsing/parser.h"
#include "src/ts/ts-type-system.h"

namespace v8 {
namespace internal {

class ParseInfo;

namespace ts {

class TSParser : public Parser {
 public:
  TSParser(LocalIsolate* local_isolate, ParseInfo* info);
  ~TSParser();

  void ParseProgram(Isolate* isolate, DirectHandle<Script> script,
                     ParseInfo* info, MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info);
  void ParseFunction(Isolate* isolate, ParseInfo* info,
                      DirectHandle<SharedFunctionInfo> shared_info);

  TSTypeSystem* type_system() { return &type_system_; }

  bool is_typescript() const { return is_typescript_; }

 private:
  void ParseTypeAnnotation(TSType** out_type);
  TSType* ParseTypeReference();
  TSType* ParseUnionType();
  TSType* ParseIntersectionType();
  TSType* ParseArrayType();
  TSType* ParseTupleType();
  TSType* ParseFunctionType();
  TSType* ParseLiteralType();
  TSType* ParseParenthesizedType();
  TSType* ParseIndexedAccessType();
  TSType* ParseConditionalType();
  TSType* ParseMappedType();
  TSType* ParseKeyofType();
  TSType* ParseThisType();
  TSType* ParseTypeParameterDeclaration(ZoneList<TypeParameter*>* params);
  void ParseInterfaceDeclaration();
  void ParseEnumDeclaration();
  void ParseNamespaceDeclaration();
  void ParseTypeAliasDeclaration();
  void ParseDecorator();

  Statement* ParseTSStatement();
  Expression* ParseTSExpression();
  Expression* ParseAsExpression(Expression* expr);
  Expression* ParseSatisfiesExpression(Expression* expr);

  void ParseClassMembersWithTypes(ZoneList<PropertyDescriptor>* props);

  void CheckTypeAnnotation(TSType* annotated_type, Expression* value);
  TSType* InferTypeFromExpression(Expression* expr);

  void ParseTSImportDeclaration();
  void ParseTSExportDeclaration();

  TSTypeSystem type_system_;
  bool is_typescript_;

  bool in_type_annotation_;
  int parse_depth_;

  bool allow_decorators_;
  bool allow_enums_;
  bool allow_namespaces_;

  ZoneList<Expression*> decorators_;
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_PARSER_H_