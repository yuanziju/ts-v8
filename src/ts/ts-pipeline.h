#ifndef V8_TS_TS_PIPELINE_H_
#define V8_TS_TS_PIPELINE_H_

#include "src/ts/ts-type-system.h"
#include "src/ts/ts-type-checker.h"
#include "src/ts/ts-parser.h"
#include "src/parsing/parse-info.h"
#include "src/ast/ast.h"

namespace v8 {
namespace internal {

class Isolate;
class ParseInfo;
class Script;

namespace ts {

struct TSCompilationConfig {
  bool is_typescript = false;
  bool type_check = true;
  bool strict_mode = true;
  bool check_nulls = true;
  bool check_implicit_any = false;
  bool skip_type_erasure = false;
  bool generate_runtime_checks = false;
  bool trust_types = true;
};

class TSPipeline {
 public:
  explicit TSPipeline(const TSCompilationConfig& config);

  static bool IsTypeScriptFile(const char* filename);

  static bool HasTypeScriptSyntax(const char* source, size_t length);

  void Initialize(ParseInfo* info);

  bool Compile(Isolate* isolate, ParseInfo* info,
                DirectHandle<Script> script,
                MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info);

  TSTypeSystem* type_system() { return type_system_; }
  TSTypeChecker* type_checker() { return type_checker_; }
  TSParser* ts_parser() { return ts_parser_; }

  struct TypeInfoForJIT {
    ZoneList<std::pair<int, TSType*>>* variable_types;
    ZoneList<std::pair<int, TSType*>>* parameter_types;
    TSType* return_type;
    bool types_are_stable;
  };

  TypeInfoForJIT* GetTypeInfoForJIT(Zone* zone);

  struct MapCreationHint {
    const char* name;
    ZoneList<PropertyDescriptor>* properties;
    bool is_stable;
    int expected_inobject_properties;
  };

  ZoneList<MapCreationHint>* GetMapHints(Zone* zone);

 private:
  bool ParseStage(Isolate* isolate, ParseInfo* info,
                   DirectHandle<Script> script,
                   MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info);
  bool TypeCheckStage(FunctionLiteral* program);
  bool AnnotateASTStage(FunctionLiteral* program);
  bool BytecodeGenerationStage(Isolate* isolate, ParseInfo* info,
                                FunctionLiteral* program);

  bool DetectTypeScript(ParseInfo* info);

  void CollectTypeAnnotations(FunctionLiteral* root);
  void CollectTypeAnnotationsRecursive(AstNode* node,
                                       ZoneList<TSType*>* types);

  void PropagateTypes(FunctionLiteral* root);
  void PropagateTypesRecursive(AstNode* node);

  void GenerateMapHints(FunctionLiteral* root);
  void GenerateVariableTypeMap(FunctionLiteral* root);

  TSCompilationConfig config_;

  TSTypeSystem* type_system_ = nullptr;
  TSTypeChecker* type_checker_ = nullptr;
  TSParser* ts_parser_ = nullptr;

  ZoneList<std::pair<const char*, TSType*>>* variable_types_ = nullptr;
  ZoneList<std::pair<int, TSType*>>* parameter_types_ = nullptr;
  TSType* function_return_type_ = nullptr;
  ZoneList<MapCreationHint>* map_hints_ = nullptr;

  bool is_typescript_ = false;
  Zone* zone_ = nullptr;
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_PIPELINE_H_