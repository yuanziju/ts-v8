// Copyright 2024 the V8 Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_TS_TS_JIT_INTEGRATION_H_
#define V8_TS_TS_JIT_INTEGRATION_H_

#include "src/ts/ts-type-system.h"
#include "src/compiler/turbofan-types.h"
#include "src/compiler/turbofan-typer.h"
#include "src/compiler/turbofan-graph.h"
#include "src/compiler/node.h"
#include "src/codegen/machine-type.h"
#include "src/maglev/maglev-compiler.h"

namespace v8 {
namespace internal {

class JSHeapBroker;
class PipelineImpl;
class TFGraph;
class Node;

namespace ts {

struct VariableTypeEntry {
  const char* name;
  TSType* type;
  int node_id;
};

struct TypeInfoForJIT {
  TSType* return_type = nullptr;
  ZoneList<TSType*>* param_types = nullptr;
  TSType* this_type = nullptr;
  bool has_explicit_return_type = false;
  bool has_explicit_param_types = false;
  bool is_strict = false;
  bool should_skip_type_checks = false;

  ZoneList<VariableTypeEntry>* variable_types = nullptr;
  ZoneList<std::pair<int, TSType*>>* node_types = nullptr;

  bool is_populated = false;

  void PopulateFromTypeSystem(TSTypeSystem* type_system,
                               Zone* zone);

  TSType* GetVariableType(const char* name) const;
  TSType* GetNodeType(int node_id) const;

  bool HasStableTypes() const {
    return has_explicit_return_type || has_explicit_param_types;
  }
};

class TSToTurboFanBridge {
 public:
  explicit TSToTurboFanBridge(JSHeapBroker* broker, Zone* zone);

  compiler::Type Convert(TSType* ts_type);

  compiler::Type ConvertFunctionSignature(TSType* return_type,
                                          ZoneList<TSType*>* param_types);

  void ApplyTypeConstraints(TFGraph* graph, Node* function_node,
                             TypeInfoForJIT* info);

  void PreTypeGraph(TFGraph* graph, TypeInfoForJIT* info);

  bool ShouldSkipSpeculativeType(TSType* ts_type);

  void EliminateDeadCode(TFGraph* graph, TypeInfoForJIT* info);

  void NarrowBranchTypes(TFGraph* graph, Node* branch_node,
                         TypeInfoForJIT* info);

  static compiler::Type CreateCompilerFunctionType(
      compiler::Type return_type, ZoneVector<compiler::Type>* param_types,
      Zone* zone);

 private:
  JSHeapBroker* broker_;
  Zone* zone_;

  ZoneList<std::pair<TSType*, compiler::Type>>* conversion_cache_;

  compiler::Type ConvertPrimitive(TSType* ts_type);
  compiler::Type ConvertObject(TSType* ts_type);
  compiler::Type ConvertArray(TSType* ts_type);
  compiler::Type ConvertFunction(TSType* ts_type);
  compiler::Type ConvertUnion(TSType* ts_type);
  compiler::Type ConvertIntersection(TSType* ts_type);
  compiler::Type ConvertLiteral(TSType* ts_type);

  Node* CreateTypeAnchor(TFGraph* graph, Node* node, compiler::Type type);
  void RemoveTypeChecksForNode(TFGraph* graph, Node* node,
                                compiler::Type guaranteed);

  void WalkAndPreTypeNodes(TFGraph* graph, TypeInfoForJIT* info);
  void ApplyNarrowingToBranch(TFGraph* graph, Node* node,
                              compiler::Type narrowed_type);
};

class TSTurboFanIntegration {
 public:
  static void BeforeTyperPhase(PipelineImpl* pipeline,
                               TSTypeSystem* type_system,
                               Zone* zone);

  static void AfterTyperPhase(PipelineImpl* pipeline,
                              TSTypeSystem* type_system,
                              Zone* zone);

  static void DuringGraphBuild(PipelineImpl* pipeline,
                               TypeInfoForJIT* info,
                               Zone* zone);

  static bool ShouldUseTSOptimization(JSFunction* function);

  static TypeInfoForJIT* CollectTypeInfo(JSFunction* function,
                                         TSTypeSystem* type_system,
                                         Zone* zone);

  static void ApplyDeadCodeElimination(PipelineImpl* pipeline,
                                       TypeInfoForJIT* info,
                                       Zone* zone);

 private:
  static void PopulateTypeInfoFromFunction(JSFunction* function,
                                           TSTypeSystem* type_system,
                                           TypeInfoForJIT* info,
                                           Zone* zone);
};

class TSMaglevIntegration {
 public:
  static void BeforeGraphBuild(maglev::MaglevCompilationInfo* info,
                               TSTypeSystem* type_system,
                               Zone* zone);

  static void DuringGraphBuild(maglev::MaglevCompilationInfo* info,
                               TypeInfoForJIT* ts_info,
                               Zone* zone);

  static void OptimizePhiSelection(maglev::MaglevCompilationInfo* info,
                                    Zone* zone);

  static bool ShouldSkipMapCheck(TSType* object_type);
  static bool ShouldSkipNumberCheck(TSType* value_type);
  static bool ShouldSkipBooleanCheck(TSType* value_type);
  static bool ShouldSkipStringCheck(TSType* value_type);
  static bool ShouldSkipUndefinedCheck(TSType* value_type);
  static bool ShouldSkipTypeGuard(TSType* guarded_type, TSType* actual_type);

  static MachineRepresentation SelectMaglevRepresentation(
      TSType* ts_type, MachineRepresentation current_rep);

  static void ApplyTypeGuards(maglev::MaglevCompilationInfo* info,
                               TypeInfoForJIT* ts_info,
                               Zone* zone);

  static void InjectUnboxedRepresentations(
      maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
      Zone* zone);

  static void SkipRedundantChecks(maglev::MaglevCompilationInfo* info,
                                  TypeInfoForJIT* ts_info,
                                  Zone* zone);
};

compiler::Type TSTypeToCompilerType(JSHeapBroker* broker,
                                     TSType* ts_type,
                                     Zone* zone);

class TSRepresentationSelector {
 public:
  static MachineRepresentation SelectRepresentation(TSType* ts_type);

  static bool CanBeSmi(TSType* ts_type);
  static bool CanBeHeapNumber(TSType* ts_type);
  static bool CanBeWord32(TSType* ts_type);

  static MachineRepresentation GetBestRepresentation(TSType* ts_type);

  static bool ShouldUseUnboxed(TSType* ts_type);

  static MachineRepresentation WidestRepresentation(
      MachineRepresentation a, MachineRepresentation b);
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_JIT_INTEGRATION_H_