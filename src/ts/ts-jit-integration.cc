// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ts/ts-jit-integration.h"

#include "src/base/logging.h"
#include "src/codegen/machine-type.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/operator.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/pipeline-data-inl.h"
#include "src/compiler/type-cache.h"
#include "src/compiler/turbofan-graph.h"
#include "src/compiler/turbofan-typer.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/opcodes.h"
#include "src/ts/ts-type-system.h"
#include "src/zone/zone-list.h"

namespace v8 {
namespace internal {

namespace compiler {
class JSHeapBroker;
class PipelineImpl;
}  // namespace compiler

namespace ts {

// ---------------------------------------------------------------------------
// TSToTurboFanBridge
// ---------------------------------------------------------------------------

TSToTurboFanBridge::TSToTurboFanBridge(JSHeapBroker* broker, Zone* zone)
    : broker_(broker),
      zone_(zone),
      conversion_cache_(
          zone->New<ZoneList<std::pair<TSType*, compiler::Type>>>(0, zone)) {}

compiler::Type TSToTurboFanBridge::Convert(TSType* ts_type) {
  if (ts_type == nullptr) return compiler::Type::Any();

  for (int i = 0; i < conversion_cache_->length(); i++) {
    if (conversion_cache_->at(i).first == ts_type) {
      return conversion_cache_->at(i).second;
    }
  }

  compiler::Type result;
  switch (ts_type->kind()) {
    case TypeKind::kBoolean:
    case TypeKind::kNumber:
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
    case TypeKind::kUndefined:
    case TypeKind::kNull:
    case TypeKind::kVoid:
    case TypeKind::kAny:
    case TypeKind::kUnknown:
    case TypeKind::kNever:
      result = ConvertPrimitive(ts_type);
      break;

    case TypeKind::kObject:
    case TypeKind::kInterface:
    case TypeKind::kPromise:
    case TypeKind::kPartial:
    case TypeKind::kRequired:
    case TypeKind::kReadonly:
    case TypeKind::kPick:
    case TypeKind::kOmit:
    case TypeKind::kRecord:
      result = ConvertObject(ts_type);
      break;

    case TypeKind::kArray:
    case TypeKind::kTuple:
      result = ConvertArray(ts_type);
      break;

    case TypeKind::kFunction:
      result = ConvertFunction(ts_type);
      break;

    case TypeKind::kUnion:
      result = ConvertUnion(ts_type);
      break;

    case TypeKind::kIntersection:
      result = ConvertIntersection(ts_type);
      break;

    case TypeKind::kLiteral:
    case TypeKind::kTemplateLiteral:
      result = ConvertLiteral(ts_type);
      break;

    case TypeKind::kThis:
      result = compiler::Type::Receiver();
      break;

    case TypeKind::kConditional:
      result = compiler::Type::Any();
      break;

    case TypeKind::kMapped:
    case TypeKind::kIndexedAccess:
    case TypeKind::kKeyof:
      result = compiler::Type::Any();
      break;

    case TypeKind::kGeneric:
    case TypeKind::kTypeReference:
    case TypeKind::kInferred:
    case TypeKind::kSatisfies:
    case TypeKind::kEnum:
    case TypeKind::kNamespace:
    case TypeKind::kParameter:
      result = compiler::Type::Any();
      break;
  }

  conversion_cache_->Add(std::make_pair(ts_type, result), zone_);
  return result;
}

compiler::Type TSToTurboFanBridge::ConvertPrimitive(TSType* ts_type) {
  switch (ts_type->kind()) {
    case TypeKind::kBoolean:
      return compiler::Type::Boolean();
    case TypeKind::kNumber:
      return compiler::Type::Number();
    case TypeKind::kString:
      return compiler::Type::String();
    case TypeKind::kSymbol:
      return compiler::Type::Symbol();
    case TypeKind::kBigInt:
      return compiler::Type::BigInt();
    case TypeKind::kUndefined:
      return compiler::Type::Undefined();
    case TypeKind::kNull:
      return compiler::Type::Null();
    case TypeKind::kVoid:
      return compiler::Type::Undefined();
    case TypeKind::kAny:
      return compiler::Type::Any();
    case TypeKind::kUnknown:
      return compiler::Type::Any();
    case TypeKind::kNever:
      return compiler::Type::None();
    default:
      return compiler::Type::Any();
  }
}

compiler::Type TSToTurboFanBridge::ConvertObject(TSType* ts_type) {
  if (ts_type->kind() == TypeKind::kPromise) {
    return compiler::Type::Receiver();
  }
  if (ts_type->kind() == TypeKind::kRecord) {
    return compiler::Type::Object();
  }
  if (ts_type->kind() == TypeKind::kPartial ||
      ts_type->kind() == TypeKind::kRequired ||
      ts_type->kind() == TypeKind::kReadonly ||
      ts_type->kind() == TypeKind::kPick ||
      ts_type->kind() == TypeKind::kOmit) {
    return compiler::Type::Object();
  }
  return compiler::Type::Object();
}

compiler::Type TSToTurboFanBridge::ConvertArray(TSType* ts_type) {
  if (ts_type->kind() == TypeKind::kTuple) {
    ZoneList<TSType*>* elem_types = ts_type->union_types();
    if (elem_types != nullptr && elem_types->length() > 0) {
      ZoneVector<compiler::Type> converted(elem_types->length());
      for (int i = 0; i < elem_types->length(); i++) {
        converted[i] = Convert(elem_types->at(i));
      }
      if (elem_types->length() == 2) {
        return compiler::Type::Tuple(converted[0], converted[1], zone_);
      }
      if (elem_types->length() == 3) {
        return compiler::Type::Tuple(converted[0], converted[1], converted[2],
                                     zone_);
      }
    }
  }
  return compiler::Type::Array();
}

compiler::Type TSToTurboFanBridge::ConvertFunction(TSType* ts_type) {
  return compiler::Type::Function();
}

compiler::Type TSToTurboFanBridge::ConvertUnion(TSType* ts_type) {
  ZoneList<TSType*>* members = ts_type->union_types();
  if (members == nullptr || members->length() == 0) {
    return compiler::Type::None();
  }
  if (members->length() == 1) {
    return Convert(members->at(0));
  }
  compiler::Type result = Convert(members->at(0));
  for (int i = 1; i < members->length(); i++) {
    compiler::Type next = Convert(members->at(i));
    result = compiler::Type::Union(result, next, zone_);
  }
  return result;
}

compiler::Type TSToTurboFanBridge::ConvertIntersection(TSType* ts_type) {
  ZoneList<TSType*>* members = ts_type->union_types();
  if (members == nullptr || members->length() == 0) {
    return compiler::Type::Any();
  }
  compiler::Type result = Convert(members->at(0));
  for (int i = 1; i < members->length(); i++) {
    compiler::Type next = Convert(members->at(i));
    result = compiler::Type::Intersect(result, next, zone_);
  }
  return result;
}

compiler::Type TSToTurboFanBridge::ConvertLiteral(TSType* ts_type) {
  const char* value = ts_type->GetName();
  if (value == nullptr) {
    return compiler::Type::Any();
  }
  switch (ts_type->kind()) {
    case TypeKind::kLiteral: {
      const char* lit = ts_type->GetName();
      if (lit != nullptr) {
        double num = 0;
        bool is_number = false;
        bool is_boolean = false;
        if (strcmp(lit, "true") == 0 || strcmp(lit, "false") == 0) {
          is_boolean = true;
        } else {
          char* end = nullptr;
          num = strtod(lit, &end);
          if (end != lit && *end == '\0') {
            is_number = true;
          }
        }
        if (is_boolean) {
          return compiler::Type::Boolean();
        }
        if (is_number) {
          return compiler::Type::Constant(num, zone_);
        }
        return compiler::Type::String();
      }
      return compiler::Type::Any();
    }
    case TypeKind::kTemplateLiteral:
      return compiler::Type::String();
    default:
      return compiler::Type::Any();
  }
}

compiler::Type TSToTurboFanBridge::ConvertFunctionSignature(
    TSType* return_type, ZoneList<TSType*>* param_types) {
  if (return_type == nullptr && param_types == nullptr) {
    return compiler::Type::Function();
  }
  return compiler::Type::Function();
}

Node* TSToTurboFanBridge::CreateTypeAnchor(TFGraph* graph, Node* node,
                                            compiler::Type type) {
  if (node == nullptr) return nullptr;
  if (!type.IsInvalid()) {
    compiler::NodeProperties::SetType(node, type);
  }
  return node;
}

void TSToTurboFanBridge::RemoveTypeChecksForNode(TFGraph* graph, Node* node,
                                                  compiler::Type guaranteed) {
  if (node == nullptr) return;
  if (guaranteed.IsInvalid()) return;

  compiler::Type existing = compiler::NodeProperties::GetType(node);
  if (!existing.IsInvalid()) {
    compiler::Type narrowed =
        compiler::Type::Intersect(existing, guaranteed, zone_);
    compiler::NodeProperties::SetType(node, narrowed);
  } else {
    compiler::NodeProperties::SetType(node, guaranteed);
  }
}

void TSToTurboFanBridge::ApplyTypeConstraints(TFGraph* graph,
                                               Node* function_node,
                                               TypeInfoForJIT* info) {
  if (graph == nullptr || function_node == nullptr || info == nullptr) return;

  if (info->return_type != nullptr) {
    compiler::Type return_type = Convert(info->return_type);
    if (!return_type.IsInvalid()) {
      compiler::NodeProperties::SetType(function_node, return_type);
    }
  }

  if (info->param_types != nullptr && info->has_explicit_param_types) {
    int param_count = info->param_types->length();
    for (int i = 0; i < param_count; i++) {
      Node* param_node = function_node->InputAt(i);
      if (param_node != nullptr) {
        compiler::Type param_type = Convert(info->param_types->at(i));
        if (!param_type.IsInvalid()) {
          RemoveTypeChecksForNode(graph, param_node, param_type);
        }
      }
    }
  }
}

void TSToTurboFanBridge::PreTypeGraph(TFGraph* graph, TypeInfoForJIT* info) {
  if (graph == nullptr || info == nullptr) return;

  Node* start = graph->start();
  Node* end = graph->end();
  if (start == nullptr) return;

  if (info->return_type != nullptr) {
    compiler::Type return_type = Convert(info->return_type);
    if (!return_type.IsInvalid() && end != nullptr) {
      compiler::NodeProperties::SetType(end, return_type);
    }
  }

  if (info->param_types != nullptr) {
    int param_count = info->param_types->length();
    for (int i = 0; i < param_count; i++) {
      TSType* ts_param = info->param_types->at(i);
      if (ts_param != nullptr) {
        compiler::Type param_type = Convert(ts_param);
        if (!param_type.IsInvalid()) {
          Node* param_node = start->InputAt(i);
          if (param_node != nullptr) {
            compiler::NodeProperties::SetType(param_node, param_type);
          }
        }
      }
    }
  }
}

bool TSToTurboFanBridge::ShouldSkipSpeculativeType(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  switch (ts_type->kind()) {
    case TypeKind::kBoolean:
    case TypeKind::kNumber:
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
    case TypeKind::kUndefined:
    case TypeKind::kNull:
    case TypeKind::kVoid:
      return true;

    case TypeKind::kAny:
    case TypeKind::kUnknown:
    case TypeKind::kNever:
      return false;

    case TypeKind::kLiteral:
    case TypeKind::kTemplateLiteral:
      return true;

    case TypeKind::kObject:
    case TypeKind::kInterface:
      return ts_type->HasKnownShape();

    case TypeKind::kArray:
    case TypeKind::kTuple:
      return true;

    case TypeKind::kFunction:
      return true;

    case TypeKind::kUnion:
    case TypeKind::kIntersection: {
      ZoneList<TSType*>* members = ts_type->union_types();
      if (members == nullptr) return false;
      for (int i = 0; i < members->length(); i++) {
        if (!ShouldSkipSpeculativeType(members->at(i))) {
          return false;
        }
      }
      return true;
    }

    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration
// ---------------------------------------------------------------------------

void TSTurboFanIntegration::BeforeTyperPhase(PipelineImpl* pipeline,
                                              TSTypeSystem* type_system,
                                              Zone* zone) {
  if (pipeline == nullptr || type_system == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TSToTurboFanBridge bridge(broker, zone);

  TypeInfoForJIT info;
  info.is_strict = true;
  info.should_skip_type_checks = true;

  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  bridge.PreTypeGraph(graph, &info);
}

void TSTurboFanIntegration::AfterTyperPhase(PipelineImpl* pipeline,
                                             TSTypeSystem* type_system,
                                             Zone* zone) {
  if (pipeline == nullptr || type_system == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TSToTurboFanBridge bridge(broker, zone);

  TypeInfoForJIT info;
  info.is_strict = true;

  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  Node* end = graph->end();
  if (end != nullptr && info.return_type != nullptr) {
    compiler::Type return_type = bridge.Convert(info.return_type);
    if (!return_type.IsInvalid()) {
      compiler::Type existing = compiler::NodeProperties::GetType(end);
      if (!existing.IsInvalid()) {
        compiler::Type refined =
            compiler::Type::Intersect(existing, return_type, zone);
        compiler::NodeProperties::SetType(end, refined);
      }
    }
  }
}

void TSTurboFanIntegration::DuringGraphBuild(PipelineImpl* pipeline,
                                              TypeInfoForJIT* info,
                                              Zone* zone) {
  if (pipeline == nullptr || info == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TSToTurboFanBridge bridge(broker, zone);
  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  bridge.PreTypeGraph(graph, info);
}

bool TSTurboFanIntegration::ShouldUseTSOptimization(JSFunction* function) {
  if (function == nullptr) return false;
  return true;
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration
// ---------------------------------------------------------------------------

void TSMaglevIntegration::BeforeGraphBuild(
    maglev::MaglevCompilationInfo* info, TSTypeSystem* type_system,
    Zone* zone) {
  if (info == nullptr || type_system == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  compiler::JSHeapBroker* broker = info->broker();
  if (broker == nullptr) return;
}

void TSMaglevIntegration::DuringGraphBuild(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (ts_info->should_skip_type_checks) {
    return;
  }
}

void TSMaglevIntegration::OptimizePhiSelection(
    maglev::MaglevCompilationInfo* info, Zone* zone) {
  if (info == nullptr || zone == nullptr) return;
}

bool TSMaglevIntegration::ShouldSkipMapCheck(TSType* object_type) {
  if (object_type == nullptr) return false;

  if (object_type->kind() == TypeKind::kObject ||
      object_type->kind() == TypeKind::kInterface) {
    return object_type->HasKnownShape();
  }

  if (object_type->kind() == TypeKind::kArray ||
      object_type->kind() == TypeKind::kTuple) {
    return true;
  }

  if (object_type->kind() == TypeKind::kUnion) {
    ZoneList<TSType*>* members = object_type->union_types();
    if (members == nullptr) return false;
    for (int i = 0; i < members->length(); i++) {
      if (!ShouldSkipMapCheck(members->at(i))) {
        return false;
      }
    }
    return true;
  }

  return false;
}

bool TSMaglevIntegration::ShouldSkipNumberCheck(TSType* value_type) {
  if (value_type == nullptr) return false;

  return value_type->kind() == TypeKind::kNumber ||
         value_type->kind() == TypeKind::kBoolean;
}

bool TSMaglevIntegration::ShouldSkipBooleanCheck(TSType* value_type) {
  if (value_type == nullptr) return false;

  return value_type->kind() == TypeKind::kBoolean;
}

// ---------------------------------------------------------------------------
// Standalone helper: TSTypeToCompilerType
// ---------------------------------------------------------------------------

compiler::Type TSTypeToCompilerType(JSHeapBroker* broker, TSType* ts_type,
                                     Zone* zone) {
  if (ts_type == nullptr) return compiler::Type::Any();

  TSToTurboFanBridge bridge(broker, zone);
  return bridge.Convert(ts_type);
}

// ---------------------------------------------------------------------------
// TSRepresentationSelector
// ---------------------------------------------------------------------------

MachineRepresentation TSRepresentationSelector::SelectRepresentation(
    TSType* ts_type) {
  if (ts_type == nullptr) return MachineRepresentation::kTagged;

  switch (ts_type->kind()) {
    case TypeKind::kBoolean:
      return MachineRepresentation::kBit;
    case TypeKind::kNumber:
      return MachineRepresentation::kFloat64;
    case TypeKind::kString:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kSymbol:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kBigInt:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kUndefined:
    case TypeKind::kNull:
      return MachineRepresentation::kTagged;
    case TypeKind::kVoid:
      return MachineRepresentation::kTagged;
    case TypeKind::kObject:
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kTuple:
    case TypeKind::kFunction:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kLiteral:
      return MachineRepresentation::kTagged;
    case TypeKind::kUnion:
    case TypeKind::kIntersection:
      return MachineRepresentation::kTagged;
    default:
      return MachineRepresentation::kTagged;
  }
}

bool TSRepresentationSelector::CanBeSmi(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  if (ts_type->kind() == TypeKind::kNumber) return true;
  if (ts_type->kind() == TypeKind::kBoolean) return true;

  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* name = ts_type->GetName();
    if (name != nullptr) {
      char* end = nullptr;
      strtod(name, &end);
      if (end != name && *end == '\0') return true;
    }
    return false;
  }

  if (ts_type->kind() == TypeKind::kUnion) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members == nullptr) return false;
    for (int i = 0; i < members->length(); i++) {
      if (!CanBeSmi(members->at(i))) return false;
    }
    return members->length() > 0;
  }

  return false;
}

bool TSRepresentationSelector::CanBeHeapNumber(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  if (ts_type->kind() == TypeKind::kNumber) return true;

  if (ts_type->kind() == TypeKind::kUnion) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members == nullptr) return false;
    for (int i = 0; i < members->length(); i++) {
      if (members->at(i)->kind() == TypeKind::kNumber) return true;
    }
    return false;
  }

  return false;
}

bool TSRepresentationSelector::CanBeWord32(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  if (ts_type->kind() == TypeKind::kNumber) return true;
  if (ts_type->kind() == TypeKind::kBoolean) return true;

  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* name = ts_type->GetName();
    if (name != nullptr) {
      char* end = nullptr;
      double val = strtod(name, &end);
      if (end != name && *end == '\0') {
        if (val >= INT32_MIN && val <= INT32_MAX) return true;
      }
    }
    return false;
  }

  if (ts_type->kind() == TypeKind::kUnion) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members == nullptr) return false;
    for (int i = 0; i < members->length(); i++) {
      if (CanBeWord32(members->at(i))) return true;
    }
    return false;
  }

  return false;
}

MachineRepresentation TSRepresentationSelector::GetBestRepresentation(
    TSType* ts_type) {
  if (ts_type == nullptr) return MachineRepresentation::kTagged;

  if (CanBeSmi(ts_type)) {
    return MachineRepresentation::kTaggedSigned;
  }

  if (ts_type->kind() == TypeKind::kBoolean) {
    return MachineRepresentation::kBit;
  }

  if (ts_type->kind() == TypeKind::kNumber) {
    return MachineRepresentation::kFloat64;
  }

  if (ts_type->kind() == TypeKind::kString ||
      ts_type->kind() == TypeKind::kSymbol ||
      ts_type->kind() == TypeKind::kBigInt) {
    return MachineRepresentation::kTaggedPointer;
  }

  if (ts_type->kind() == TypeKind::kObject ||
      ts_type->kind() == TypeKind::kInterface ||
      ts_type->kind() == TypeKind::kArray ||
      ts_type->kind() == TypeKind::kFunction) {
    return MachineRepresentation::kTaggedPointer;
  }

  return MachineRepresentation::kTagged;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8