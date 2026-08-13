// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_TS_TS_JIT_INTEGRATION_H_
#define V8_TS_TS_JIT_INTEGRATION_H_

#include "src/ts/ts-type-system.h"
#include "src/compiler/turbofan-types.h"
#include "src/compiler/turbofan-typer.h"
#include "src/maglev/maglev-compiler.h"

namespace v8 {
namespace internal {

class JSHeapBroker;
class PipelineImpl;
class TFGraph;
class Node;

namespace ts {

// Forward declaration: information about TS types collected for JIT compilation
struct TypeInfoForJIT {
  TSType* return_type = nullptr;
  ZoneList<TSType*>* param_types = nullptr;
  TSType* this_type = nullptr;
  bool has_explicit_return_type = false;
  bool has_explicit_param_types = false;
  bool is_strict = false;
  bool should_skip_type_checks = false;
};

// Bridge between TS type system and TurboFan's type system
class TSToTurboFanBridge {
 public:
  explicit TSToTurboFanBridge(JSHeapBroker* broker, Zone* zone);

  // Convert TSType to V8's compiler::Type
  compiler::Type Convert(TSType* ts_type);

  // Convert TS function signature to TurboFan function type
  compiler::Type ConvertFunctionSignature(TSType* return_type,
                                          ZoneList<TSType*>* param_types);

  // Apply TS type constraints to TurboFan graph
  void ApplyTypeConstraints(TFGraph* graph, Node* function_node,
                            TypeInfoForJIT* info);

  // Pre-type nodes in the graph based on TS annotations
  void PreTypeGraph(TFGraph* graph, TypeInfoForJIT* info);

  // Skip speculative typing when TS guarantees the type
  bool ShouldSkipSpeculativeType(TSType* ts_type);

 private:
  JSHeapBroker* broker_;
  Zone* zone_;

  // Type conversion cache
  ZoneList<std::pair<TSType*, compiler::Type>>* conversion_cache_;

  // Internal conversion methods
  compiler::Type ConvertPrimitive(TSType* ts_type);
  compiler::Type ConvertObject(TSType* ts_type);
  compiler::Type ConvertArray(TSType* ts_type);
  compiler::Type ConvertFunction(TSType* ts_type);
  compiler::Type ConvertUnion(TSType* ts_type);
  compiler::Type ConvertIntersection(TSType* ts_type);
  compiler::Type ConvertLiteral(TSType* ts_type);

  // TurboFan type manipulation helpers
  Node* CreateTypeAnchor(TFGraph* graph, Node* node, compiler::Type type);
  void RemoveTypeChecksForNode(TFGraph* graph, Node* node,
                                compiler::Type guaranteed);
};

// Integration with TurboFan pipeline
class TSTurboFanIntegration {
 public:
  // Called before TyperPhase to inject TS types
  static void BeforeTyperPhase(PipelineImpl* pipeline,
                               TSTypeSystem* type_system,
                               Zone* zone);

  // Called after TyperPhase to refine types
  static void AfterTyperPhase(PipelineImpl* pipeline,
                              TSTypeSystem* type_system,
                              Zone* zone);

  // Called during GraphBuilder to pre-type nodes
  static void DuringGraphBuild(PipelineImpl* pipeline,
                               TypeInfoForJIT* info,
                               Zone* zone);

  // Determine if a function should use TS-guided optimization
  static bool ShouldUseTSOptimization(JSFunction* function);
};

// Integration with Maglev compiler
class TSMaglevIntegration {
 public:
  // Called before Maglev graph building
  static void BeforeGraphBuild(maglev::MaglevCompilationInfo* info,
                               TSTypeSystem* type_system,
                               Zone* zone);

  // Called during Maglev graph building to inject types
  static void DuringGraphBuild(maglev::MaglevCompilationInfo* info,
                               TypeInfoForJIT* info,
                               Zone* zone);

  // Optimize Maglev Phi selection based on TS types
  static void OptimizePhiSelection(maglev::MaglevCompilationInfo* info,
                                    Zone* zone);

  // Determine if Maglev should skip certain checks
  static bool ShouldSkipMapCheck(TSType* object_type);
  static bool ShouldSkipNumberCheck(TSType* value_type);
  static bool ShouldSkipBooleanCheck(TSType* value_type);
};

// Helper for creating V8 compiler::Type from TS types
compiler::Type TSTypeToCompilerType(JSHeapBroker* broker,
                                     TSType* ts_type,
                                     Zone* zone);

// Helper for creating TurboFan representations from TS types
class TSRepresentationSelector {
 public:
  // Select representation based on TS type
  static MachineRepresentation SelectRepresentation(TSType* ts_type);

  // Check if TS type allows Smi representation
  static bool CanBeSmi(TSType* ts_type);

  // Check if TS type allows HeapNumber representation
  static bool CanBeHeapNumber(TSType* ts_type);

  // Check if TS type can be represented as Word32
  static bool CanBeWord32(TSType* ts_type);

  // Get the best representation for a TS type
  static MachineRepresentation GetBestRepresentation(TSType* ts_type);
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_JIT_INTEGRATION_H_