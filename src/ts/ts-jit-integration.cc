// Copyright 2024 the V8 Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ts/ts-jit-integration.h"

#include <cstring>
#include <utility>

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
#include "src/compiler/js-operator.h"
#include "src/compiler/control-operator.h"
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
// TypeInfoForJIT - Data population with real TS type data
// ---------------------------------------------------------------------------

void TypeInfoForJIT::PopulateFromTypeSystem(TSTypeSystem* type_system,
                                             Zone* zone) {
  if (type_system == nullptr || zone == nullptr) return;

  is_populated = true;
  is_strict = true;

  if (variable_types == nullptr) {
    variable_types = zone->New<ZoneList<VariableTypeEntry>>(0, zone);
  }
  if (node_types == nullptr) {
    node_types = zone->New<ZoneList<std::pair<int, TSType*>>>(0, zone);
  }

  if (return_type == nullptr) {
    return_type = type_system->NewAny();
    has_explicit_return_type = false;
  } else if (return_type->kind() != TypeKind::kAny &&
             return_type->kind() != TypeKind::kUnknown) {
    has_explicit_return_type = true;
  }

  if (param_types != nullptr && param_types->length() > 0) {
    has_explicit_param_types = true;
    for (int i = 0; i < param_types->length(); i++) {
      TSType* p = param_types->at(i);
      if (p == nullptr || p->kind() == TypeKind::kAny ||
          p->kind() == TypeKind::kUnknown) {
        has_explicit_param_types = false;
        break;
      }
    }
  } else if (param_types == nullptr) {
    has_explicit_param_types = false;
  }

  if (has_explicit_return_type || has_explicit_param_types) {
    should_skip_type_checks = true;
  } else {
    should_skip_type_checks = false;
  }

  for (int i = 0; i < variable_types->length(); i++) {
    const VariableTypeEntry& entry = variable_types->at(i);
    if (entry.type != nullptr &&
        entry.type->kind() != TypeKind::kAny &&
        entry.type->kind() != TypeKind::kUnknown) {
      node_types->Add(std::make_pair(entry.node_id, entry.type), zone);
    }
  }
}

TSType* TypeInfoForJIT::GetVariableType(const char* name) const {
  if (name == nullptr || variable_types == nullptr) return nullptr;
  for (int i = 0; i < variable_types->length(); i++) {
    if (strcmp(variable_types->at(i).name, name) == 0) {
      return variable_types->at(i).type;
    }
  }
  return nullptr;
}

TSType* TypeInfoForJIT::GetNodeType(int node_id) const {
  if (node_types == nullptr) return nullptr;
  for (int i = 0; i < node_types->length(); i++) {
    if (node_types->at(i).first == node_id) {
      return node_types->at(i).second;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge - Construction
// ---------------------------------------------------------------------------

TSToTurboFanBridge::TSToTurboFanBridge(JSHeapBroker* broker, Zone* zone)
    : broker_(broker),
      zone_(zone),
      conversion_cache_(
          zone->New<ZoneList<std::pair<TSType*, compiler::Type>>>(0, zone)) {}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::Convert - Main TS->TurboFan type dispatch
// ---------------------------------------------------------------------------

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
    case TypeKind::kTrue:
    case TypeKind::kFalse:
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
    case TypeKind::kConstructor:
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
    case TypeKind::kMapped:
    case TypeKind::kIndexedAccess:
    case TypeKind::kKeyof:
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

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertPrimitive
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConvertPrimitive(TSType* ts_type) {
  switch (ts_type->kind()) {
    case TypeKind::kBoolean:
    case TypeKind::kTrue:
    case TypeKind::kFalse:
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

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::NumericTypeFromTS - Extract numeric precision info
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::NumericTypeFromTS(TSType* ts_type) {
  if (ts_type == nullptr) return compiler::Type::Number();

  if (ts_type->kind() == TypeKind::kNumber) {
    return compiler::Type::Number();
  }

  if (ts_type->kind() == TypeKind::kBoolean ||
      ts_type->kind() == TypeKind::kTrue ||
      ts_type->kind() == TypeKind::kFalse) {
    return compiler::Type::Boolean();
  }

  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* lit = ts_type->GetName();
    if (lit != nullptr) {
      char* end = nullptr;
      double val = strtod(lit, &end);
      if (end != lit && *end == '\0') {
        return compiler::Type::Constant(val, zone_);
      }
    }
    return compiler::Type::Number();
  }

  if (ts_type->IsUnion()) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members != nullptr && members->length() > 0) {
      compiler::Type result = NumericTypeFromTS(members->at(0));
      for (int i = 1; i < members->length(); i++) {
        result = compiler::Type::Union(result, NumericTypeFromTS(members->at(i)),
                                       zone_);
      }
      return result;
    }
  }

  return compiler::Type::Number();
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConstructTypedObjectShape
//   Uses HasKnownShape() and the property list to construct typed object
//   shapes with internal property type annotations, instead of returning
//   a generic Object() type for all objects.
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConstructTypedObjectShape(TSType* ts_type) {
  if (ts_type == nullptr) return compiler::Type::Object();

  if (!ts_type->HasKnownShape()) {
    return compiler::Type::Object();
  }

  ZoneList<PropertyDescriptor>* props = ts_type->GetProperties();
  if (props == nullptr || props->length() == 0) {
    return compiler::Type::Object();
  }

  compiler::Type base = compiler::Type::Object();

  int concrete_props = 0;
  for (int i = 0; i < props->length(); i++) {
    const PropertyDescriptor& prop = props->at(i);
    if (prop.type != nullptr &&
        prop.type->kind() != TypeKind::kAny &&
        prop.type->kind() != TypeKind::kUnknown) {
      compiler::Type prop_compiler_type = Convert(prop.type);
      if (!prop_compiler_type.IsInvalid()) {
        base = compiler::Type::Intersect(base, prop_compiler_type, zone_);
        concrete_props++;
      }
    }
  }

  if (concrete_props == 0) {
    return compiler::Type::Object();
  }

  return base;
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertObject
//   NOW uses HasKnownShape() and property list to construct typed object
//   shapes with internal property type annotations, rather than returning
//   a plain compiler::Type::Object() for all object types.
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConvertObject(TSType* ts_type) {
  if (ts_type == nullptr) return compiler::Type::Object();

  switch (ts_type->kind()) {
    case TypeKind::kPromise: {
      TSType* elem = ts_type->GetElementType();
      if (elem != nullptr && elem->kind() != TypeKind::kAny &&
          elem->kind() != TypeKind::kUnknown) {
        compiler::Type elem_type = Convert(elem);
        if (!elem_type.IsInvalid()) {
          return compiler::Type::Intersect(compiler::Type::Receiver(),
                                           elem_type, zone_);
        }
      }
      return compiler::Type::Receiver();
    }

    case TypeKind::kRecord: {
      TSType* value_type = ts_type->GetElementType();
      if (value_type != nullptr && value_type->kind() != TypeKind::kAny) {
        compiler::Type val = Convert(value_type);
        if (!val.IsInvalid()) {
          return compiler::Type::Intersect(compiler::Type::Object(), val,
                                           zone_);
        }
      }
      return compiler::Type::Object();
    }

    case TypeKind::kPartial:
    case TypeKind::kRequired:
    case TypeKind::kReadonly:
    case TypeKind::kPick:
    case TypeKind::kOmit: {
      TSType* ref = ts_type->GetReferencedType();
      if (ref != nullptr && ref->HasKnownShape()) {
        return ConstructTypedObjectShape(ref);
      }
      ZoneList<PropertyDescriptor>* props = ts_type->GetProperties();
      if (props != nullptr && props->length() > 0) {
        return ConstructTypedObjectShape(ts_type);
      }
      return compiler::Type::Object();
    }

    case TypeKind::kObject:
    case TypeKind::kInterface: {
      if (ts_type->HasKnownShape()) {
        return ConstructTypedObjectShape(ts_type);
      }
      return compiler::Type::Object();
    }

    default:
      return compiler::Type::Object();
  }
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertArray
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConvertArray(TSType* ts_type) {
  if (ts_type->kind() == TypeKind::kTuple) {
    ZoneList<TSType*>* elem_types = ts_type->union_types();
    if (elem_types != nullptr && elem_types->length() > 0) {
      ZoneVector<compiler::Type> converted(elem_types->length(), zone_);
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
      compiler::Type result = converted[0];
      for (int i = 1; i < elem_types->length(); i++) {
        result = compiler::Type::Union(result, converted[i], zone_);
      }
      return result;
    }
  }

  if (ts_type->kind() == TypeKind::kArray) {
    TSType* elem = ts_type->GetElementType();
    if (elem != nullptr && elem->kind() != TypeKind::kAny &&
        elem->kind() != TypeKind::kUnknown) {
      compiler::Type elem_type = Convert(elem);
      if (!elem_type.IsInvalid()) {
        return compiler::Type::Intersect(compiler::Type::Array(), elem_type,
                                         zone_);
      }
    }
  }

  return compiler::Type::Array();
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertFunction
//   Creates a TurboFan function type that specifies the exact parameter
//   and return types, enabling TurboFan to inline more aggressively and
//   avoid type checks.
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConvertFunction(TSType* ts_type) {
  TSType* return_type = ts_type->GetReturnType();
  ZoneList<TSType*>* param_types = ts_type->GetParamTypes();

  if (return_type == nullptr &&
      (param_types == nullptr || param_types->length() == 0)) {
    return compiler::Type::Function();
  }

  if (return_type != nullptr ||
      (param_types != nullptr && param_types->length() > 0)) {
    compiler::Type converted_return =
        return_type != nullptr ? Convert(return_type)
                               : compiler::Type::Any();
    ZoneVector<compiler::Type> param_conversions;
    int arity = 0;
    if (param_types != nullptr) {
      arity = param_types->length();
      param_conversions.reserve(arity);
      for (int i = 0; i < arity; i++) {
        param_conversions.push_back(Convert(param_types->at(i)));
      }
    }
    compiler::Type fn_type = CreateCompilerFunctionType(
        converted_return, &param_conversions, zone_);

    if (arity > 0 && !ts_type->HasRestParameter()) {
      fn_type = compiler::Type::Intersect(
          fn_type, compiler::Type::Constant(arity, zone_), zone_);
    }

    return fn_type;
  }

  return compiler::Type::Function();
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::CreateCompilerFunctionType
//   Builds a precise TurboFan function type with exact param/return types.
//   This enables TurboFan to:
//   - Skip speculative parameter type checks (CheckNumber, CheckString, etc.)
//   - Infer return type precisely without speculation
//   - Perform more aggressive inlining since the function signature is known
//   - Generate specialized code for known parameter counts
//   - Eliminate redundant Convert* nodes at call sites
//
//   The function type is constructed as:
//     Function ∩ ReturnType ∩ (Param1 × Param2 × ... × ParamN)
//   where each parameter type is intersected individually to allow
//   per-parameter type narrowing in TurboFan's forward analysis.
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::CreateCompilerFunctionType(
    compiler::Type return_type, ZoneVector<compiler::Type>* param_types,
    Zone* zone) {
  compiler::Type base = compiler::Type::Function();

  if (!return_type.IsInvalid()) {
    base = compiler::Type::Intersect(base, return_type, zone);
  }

  if (param_types != nullptr && param_types->size() > 0) {
    for (size_t i = 0; i < param_types->size(); i++) {
      if (!(*param_types)[i].IsInvalid()) {
        base = compiler::Type::Intersect(base, (*param_types)[i], zone);
      }
    }
  }

  return base;
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertUnion / ConvertIntersection
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertLiteral
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConvertLiteral(TSType* ts_type) {
  const char* value = ts_type->GetName();
  if (value == nullptr) {
    return compiler::Type::Any();
  }
  switch (ts_type->kind()) {
    case TypeKind::kLiteral: {
      const char* lit = ts_type->GetName();
      if (lit != nullptr) {
        bool is_boolean = false;
        if (strcmp(lit, "true") == 0 || strcmp(lit, "false") == 0) {
          is_boolean = true;
        }
        if (is_boolean) {
          return compiler::Type::Boolean();
        }
        char* end = nullptr;
        double num = strtod(lit, &end);
        if (end != lit && *end == '\0') {
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

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::ConvertFunctionSignature
// ---------------------------------------------------------------------------

compiler::Type TSToTurboFanBridge::ConvertFunctionSignature(
    TSType* return_type, ZoneList<TSType*>* param_types) {
  if (return_type == nullptr &&
      (param_types == nullptr || param_types->length() == 0)) {
    return compiler::Type::Function();
  }

  ZoneVector<compiler::Type> converted_params;
  if (param_types != nullptr && param_types->length() > 0) {
    converted_params.reserve(param_types->length());
    for (int i = 0; i < param_types->length(); i++) {
      converted_params.push_back(Convert(param_types->at(i)));
    }
  }

  compiler::Type converted_return = compiler::Type::Any();
  if (return_type != nullptr) {
    converted_return = Convert(return_type);
  }

  return CreateCompilerFunctionType(converted_return, &converted_params,
                                    zone_);
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge - Graph manipulation
// ---------------------------------------------------------------------------

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

  if (info->param_types != nullptr && info->param_types->length() > 0) {
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

  WalkAndPreTypeNodes(graph, info);
}

void TSToTurboFanBridge::WalkAndPreTypeNodes(TFGraph* graph,
                                              TypeInfoForJIT* info) {
  if (graph == nullptr || info == nullptr) return;

  Node* start = graph->start();
  if (start == nullptr) return;

  ZoneList<Node*> visited(0, zone_);
  ZoneList<Node*> worklist(0, zone_);

  worklist.Add(start, zone_);
  visited.Add(start, zone_);

  while (worklist.length() > 0) {
    Node* current = worklist.RemoveLast();

    int id = current->id();
    TSType* ts_type = info->GetNodeType(id);
    if (ts_type != nullptr) {
      compiler::Type tf_type = Convert(ts_type);
      if (!tf_type.IsInvalid()) {
        compiler::NodeProperties::SetType(current, tf_type);
      }
    }

    for (int i = 0; i < current->OutputCount(); i++) {
      Node* output = current->OutputAt(i);
      if (output != nullptr) {
        bool found = false;
        for (int j = 0; j < visited.length(); j++) {
          if (visited.at(j) == output) {
            found = true;
            break;
          }
        }
        if (!found) {
          visited.Add(output, zone_);
          worklist.Add(output, zone_);
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
    case TypeKind::kTrue:
    case TypeKind::kFalse:
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
    case TypeKind::kConstructor:
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
// TSToTurboFanBridge::IsDeadCodePath
//   Checks if a node's TS type proves it is unreachable.
// ---------------------------------------------------------------------------

bool TSToTurboFanBridge::IsDeadCodePath(Node* node, TypeInfoForJIT* info) {
  if (node == nullptr || info == nullptr) return false;

  int id = node->id();
  TSType* ts_type = info->GetNodeType(id);
  if (ts_type == nullptr) return false;

  if (ts_type->kind() == TypeKind::kNever) {
    return true;
  }

  if (ts_type->kind() == TypeKind::kUnion) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members != nullptr) {
      bool all_never = true;
      for (int i = 0; i < members->length(); i++) {
        if (members->at(i)->kind() != TypeKind::kNever) {
          all_never = false;
          break;
        }
      }
      if (all_never) return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::EliminateDeadCode
//   Walks the graph and marks code paths as Dead when TS type constraints
//   prove they are unreachable:
//   - kNever type annotations on branch conditions
//   - Redundant type checks (e.g., CheckNumber for a number-typed value)
//   - Union types where one branch is impossible
//   - Contradictory type constraints
// ---------------------------------------------------------------------------

void TSToTurboFanBridge::EliminateDeadCode(TFGraph* graph,
                                            TypeInfoForJIT* info) {
  if (graph == nullptr || info == nullptr) return;

  Node* start = graph->start();
  if (start == nullptr) return;

  ZoneList<Node*> visited(0, zone_);
  ZoneList<Node*> worklist(0, zone_);
  ZoneList<Node*> dead_nodes(0, zone_);

  worklist.Add(start, zone_);
  visited.Add(start, zone_);

  while (worklist.length() > 0) {
    Node* current = worklist.RemoveLast();

    if (current->opcode() == compiler::Branch ||
        current->opcode() == compiler::IfValue ||
        current->opcode() == compiler::IfCondition) {
      Node* cond = current->InputAt(0);
      if (cond != nullptr) {
        TSType* cond_type = info->GetNodeType(cond->id());
        if (cond_type != nullptr) {
          if (cond_type->kind() == TypeKind::kNever) {
            for (int i = 0; i < current->OutputCount(); i++) {
              dead_nodes.Add(current->OutputAt(i), zone_);
            }
          } else if (cond_type->kind() == TypeKind::kBoolean) {
            NarrowBranchTypes(graph, current, info);
          } else if (cond_type->IsUnion()) {
            ZoneList<TSType*>* members = cond_type->union_types();
            if (members != nullptr) {
              bool has_never_member = false;
              bool has_known_member = false;
              for (int i = 0; i < members->length(); i++) {
                if (members->at(i)->kind() == TypeKind::kNever) {
                  has_never_member = true;
                } else {
                  has_known_member = true;
                }
              }
              if (has_never_member && !has_known_member) {
                for (int i = 0; i < current->OutputCount(); i++) {
                  dead_nodes.Add(current->OutputAt(i), zone_);
                }
              } else if (has_known_member) {
                NarrowBranchTypes(graph, current, info);
              }
            }
          }
        }
      }
    }

    if (current->opcode() == compiler::Switch) {
      Node* tag = current->InputAt(0);
      if (tag != nullptr) {
        TSType* tag_type = info->GetNodeType(tag->id());
        if (tag_type != nullptr && tag_type->kind() == TypeKind::kNever) {
          for (int i = 0; i < current->OutputCount(); i++) {
            dead_nodes.Add(current->OutputAt(i), zone_);
          }
        }
      }
    }

    if (current->opcode() == compiler::CheckNumber ||
        current->opcode() == compiler::CheckString ||
        current->opcode() == compiler::CheckBoolean ||
        current->opcode() == compiler::CheckUndefined ||
        current->opcode() == compiler::CheckMaps) {
      Node* input = current->InputAt(0);
      if (input != nullptr) {
        TSType* input_type = info->GetNodeType(input->id());
        if (input_type != nullptr) {
          bool is_redundant = false;
          switch (current->opcode()) {
            case compiler::CheckNumber:
              is_redundant = (input_type->kind() == TypeKind::kNumber ||
                              input_type->kind() == TypeKind::kBoolean);
              break;
            case compiler::CheckString:
              is_redundant = (input_type->kind() == TypeKind::kString);
              break;
            case compiler::CheckBoolean:
              is_redundant = (input_type->kind() == TypeKind::kBoolean ||
                              input_type->kind() == TypeKind::kTrue ||
                              input_type->kind() == TypeKind::kFalse);
              break;
            case compiler::CheckUndefined:
              is_redundant = (input_type->kind() == TypeKind::kUndefined ||
                              input_type->kind() == TypeKind::kVoid);
              break;
            case compiler::CheckMaps:
              is_redundant = input_type->HasKnownShape();
              break;
            default:
              break;
          }
          if (is_redundant) {
            dead_nodes.Add(current, zone_);
          }
        }
      }
    }

    for (int i = 0; i < current->OutputCount(); i++) {
      Node* output = current->OutputAt(i);
      if (output != nullptr) {
        bool found = false;
        for (int j = 0; j < visited.length(); j++) {
          if (visited.at(j) == output) {
            found = true;
            break;
          }
        }
        if (!found) {
          visited.Add(output, zone_);
          worklist.Add(output, zone_);
        }
      }
    }
  }

  for (int i = 0; i < dead_nodes.length(); i++) {
    Node* dead = dead_nodes.at(i);
    if (dead != nullptr) {
      compiler::NodeProperties::SetType(dead, compiler::Type::None());
    }
  }
}

// ---------------------------------------------------------------------------
// TSToTurboFanBridge::NarrowBranchTypes
// ---------------------------------------------------------------------------

void TSToTurboFanBridge::NarrowBranchTypes(TFGraph* graph,
                                            Node* branch_node,
                                            TypeInfoForJIT* info) {
  if (graph == nullptr || branch_node == nullptr || info == nullptr) return;

  if (branch_node->opcode() != compiler::Branch &&
      branch_node->opcode() != compiler::IfValue &&
      branch_node->opcode() != compiler::IfCondition) {
    return;
  }

  Node* condition = branch_node->InputAt(0);
  if (condition == nullptr) return;

  TSType* cond_ts_type = info->GetNodeType(condition->id());
  if (cond_ts_type == nullptr) return;

  if (cond_ts_type->kind() == TypeKind::kNever) {
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i),
                              compiler::Type::None());
    }
    return;
  }

  if (cond_ts_type->kind() == TypeKind::kBoolean ||
      cond_ts_type->kind() == TypeKind::kTrue ||
      cond_ts_type->kind() == TypeKind::kFalse) {
    compiler::Type narrowed = compiler::Type::Boolean();
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
    }
  }

  if (cond_ts_type->kind() == TypeKind::kNumber) {
    compiler::Type narrowed = compiler::Type::Number();
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
    }
  }

  if (cond_ts_type->kind() == TypeKind::kString) {
    compiler::Type narrowed = compiler::Type::String();
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
    }
  }

  if (cond_ts_type->kind() == TypeKind::kObject ||
      cond_ts_type->kind() == TypeKind::kInterface) {
    compiler::Type narrowed = ConvertObject(cond_ts_type);
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
    }
  }

  if (cond_ts_type->kind() == TypeKind::kUndefined ||
      cond_ts_type->kind() == TypeKind::kVoid) {
    compiler::Type narrowed = compiler::Type::Undefined();
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
    }
  }

  if (cond_ts_type->kind() == TypeKind::kNull) {
    compiler::Type narrowed = compiler::Type::Null();
    for (int i = 0; i < branch_node->OutputCount(); i++) {
      ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
    }
  }

  if (cond_ts_type->IsUnion()) {
    ZoneList<TSType*>* members = cond_ts_type->union_types();
    if (members != nullptr && members->length() > 0) {
      compiler::Type narrowed = Convert(members->at(0));
      for (int i = 1; i < members->length(); i++) {
        narrowed = compiler::Type::Union(narrowed, Convert(members->at(i)),
                                          zone_);
      }
      for (int i = 0; i < branch_node->OutputCount(); i++) {
        ApplyNarrowingToBranch(graph, branch_node->OutputAt(i), narrowed);
      }
    }
  }
}

void TSToTurboFanBridge::ApplyNarrowingToBranch(TFGraph* graph, Node* node,
                                                 compiler::Type narrowed_type) {
  if (node == nullptr || narrowed_type.IsInvalid()) return;

  compiler::Type existing = compiler::NodeProperties::GetType(node);
  compiler::Type new_type;
  if (!existing.IsInvalid()) {
    new_type = compiler::Type::Intersect(existing, narrowed_type, zone_);
  } else {
    new_type = narrowed_type;
  }
  compiler::NodeProperties::SetType(node, new_type);
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration - CollectTypeInfo & PopulateTypeInfoFromFunction
// ---------------------------------------------------------------------------

TypeInfoForJIT* TSTurboFanIntegration::CollectTypeInfo(
    JSFunction* function, TSTypeSystem* type_system, Zone* zone) {
  if (type_system == nullptr || zone == nullptr) return nullptr;

  TypeInfoForJIT* info = zone->New<TypeInfoForJIT>();
  info->PopulateFromTypeSystem(type_system, zone);

  PopulateTypeInfoFromFunction(function, type_system, info, zone);

  return info;
}

void TSTurboFanIntegration::PopulateTypeInfoFromFunction(
    JSFunction* function, TSTypeSystem* type_system, TypeInfoForJIT* info,
    Zone* zone) {
  if (function == nullptr || type_system == nullptr || info == nullptr) return;

  if (info->return_type == nullptr ||
      info->return_type->kind() == TypeKind::kAny ||
      info->return_type->kind() == TypeKind::kUnknown) {
    info->return_type = type_system->NewAny();
    info->has_explicit_return_type = false;
  } else {
    info->has_explicit_return_type = true;
  }

  if (info->param_types == nullptr) {
    info->param_types = zone->New<ZoneList<TSType*>>(0, zone);
    info->has_explicit_param_types = false;
  }

  if (info->variable_types == nullptr) {
    info->variable_types = zone->New<ZoneList<VariableTypeEntry>>(0, zone);
  }

  if (info->param_types->length() > 0 && info->has_explicit_param_types) {
    for (int i = 0; i < info->param_types->length(); i++) {
      TSType* p = info->param_types->at(i);
      if (p != nullptr &&
          p->kind() != TypeKind::kAny &&
          p->kind() != TypeKind::kUnknown) {
        VariableTypeEntry entry;
        entry.name = "";
        entry.type = p;
        entry.node_id = i + 1;
        info->variable_types->Add(entry, zone);
      }
    }
  }

  info->is_populated = true;

  if (info->has_explicit_return_type || info->has_explicit_param_types) {
    info->should_skip_type_checks = true;
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::PreColorGraphNodes
//   Pre-color the entire IR graph with TS types before TurboFan's own
//   typer phase runs, allowing TurboFan to skip its speculation phase.
// ---------------------------------------------------------------------------

void TSTurboFanIntegration::PreColorGraphNodes(
    TFGraph* graph, TypeInfoForJIT* info, TSToTurboFanBridge* bridge) {
  if (graph == nullptr || info == nullptr || bridge == nullptr) return;

  Node* start = graph->start();
  Node* end = graph->end();
  if (start == nullptr) return;

  if (info->return_type != nullptr) {
    compiler::Type return_type = bridge->Convert(info->return_type);
    if (!return_type.IsInvalid() && end != nullptr) {
      compiler::NodeProperties::SetType(end, return_type);
    }
  }

  if (info->param_types != nullptr && info->param_types->length() > 0) {
    int param_count = info->param_types->length();
    for (int i = 0; i < param_count && i < start->OutputCount(); i++) {
      TSType* ts_param = info->param_types->at(i);
      if (ts_param != nullptr &&
          ts_param->kind() != TypeKind::kAny &&
          ts_param->kind() != TypeKind::kUnknown) {
        compiler::Type param_type = bridge->Convert(ts_param);
        Node* param_node = start->OutputAt(i);
        if (param_node != nullptr && !param_type.IsInvalid()) {
          compiler::NodeProperties::SetType(param_node, param_type);
        }
      }
    }
  }

  bridge->WalkAndPreTypeNodes(graph, info);

  if (info->should_skip_type_checks && info->HasStableTypes()) {
    bridge->EliminateDeadCode(graph, info);
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::InjectFunctionTypeGuards
//   Injects precise function type guards for parameter and return types.
//   Enables TurboFan to inline more aggressively and avoid type checks.
// ---------------------------------------------------------------------------

void TSTurboFanIntegration::InjectFunctionTypeGuards(
    PipelineImpl* pipeline, TypeInfoForJIT* info, Zone* zone) {
  if (pipeline == nullptr || info == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  TSToTurboFanBridge bridge(broker, zone);

  Node* start = graph->start();
  Node* end = graph->end();
  if (start == nullptr) return;

  if (info->return_type != nullptr && info->has_explicit_return_type) {
    compiler::Type return_type = bridge.Convert(info->return_type);
    if (!return_type.IsInvalid() && end != nullptr) {
      compiler::Type existing = compiler::NodeProperties::GetType(end);
      if (!existing.IsInvalid()) {
        compiler::Type refined =
            compiler::Type::Intersect(existing, return_type, zone);
        compiler::NodeProperties::SetType(end, refined);
      } else {
        compiler::NodeProperties::SetType(end, return_type);
      }
    }
  }

  if (info->param_types != nullptr && info->has_explicit_param_types) {
    int param_count = info->param_types->length();
    for (int i = 0; i < param_count && i < start->OutputCount(); i++) {
      TSType* ts_param = info->param_types->at(i);
      if (ts_param != nullptr &&
          ts_param->kind() != TypeKind::kAny &&
          ts_param->kind() != TypeKind::kUnknown) {
        compiler::Type param_type = bridge.Convert(ts_param);
        Node* param_node = start->OutputAt(i);
        if (param_node != nullptr && !param_type.IsInvalid()) {
          bridge.RemoveTypeChecksForNode(graph, param_node, param_type);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::BeforeTyperPhase
//   Main entry point: reads TS type annotations for the entire function
//   and pre-colors the TurboFan IR graph nodes with these types. This
//   allows TurboFan's own type system to skip its speculation phase.
// ---------------------------------------------------------------------------

void TSTurboFanIntegration::BeforeTyperPhase(PipelineImpl* pipeline,
                                              TSTypeSystem* type_system,
                                              Zone* zone) {
  if (pipeline == nullptr || type_system == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  TypeInfoForJIT info;
  info.PopulateFromTypeSystem(type_system, zone);

  JSFunction* function = data->function().is_null()
                             ? nullptr
                             : data->function().handle();

  if (function != nullptr) {
    PopulateTypeInfoFromFunction(function, type_system, &info, zone);
  }

  TSToTurboFanBridge bridge(broker, zone);

  if (info.HasStableTypes()) {
    PreColorGraphNodes(graph, &info, &bridge);

    InjectFunctionTypeGuards(pipeline, &info, zone);

    if (info.should_skip_type_checks) {
      bridge.EliminateDeadCode(graph, &info);

      Node* start = graph->start();
      if (start != nullptr) {
        for (int i = 0; i < start->OutputCount(); i++) {
          Node* param_projection = start->OutputAt(i);
          if (param_projection != nullptr &&
              (param_projection->opcode() == compiler::Parameter ||
               param_projection->opcode() == compiler::Int32Constant)) {
            TSType* ts_param =
                info.GetNodeType(param_projection->id());
            if (ts_param != nullptr &&
                ts_param->kind() != TypeKind::kAny &&
                ts_param->kind() != TypeKind::kUnknown) {
              compiler::Type param_type = bridge.Convert(ts_param);
              if (!param_type.IsInvalid()) {
                compiler::NodeProperties::SetType(param_projection,
                                                   param_type);
              }
            }
          }
        }
      }

      Node* end = graph->end();
      if (end != nullptr && info.return_type != nullptr &&
          info.has_explicit_return_type) {
        compiler::Type return_type = bridge.Convert(info.return_type);
        if (!return_type.IsInvalid()) {
          compiler::NodeProperties::SetType(end, return_type);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::AfterTyperPhase
//   Intersects TS types with TurboFan's inferred types for max precision.
// ---------------------------------------------------------------------------

void TSTurboFanIntegration::AfterTyperPhase(PipelineImpl* pipeline,
                                             TSTypeSystem* type_system,
                                             Zone* zone) {
  if (pipeline == nullptr || type_system == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  TSToTurboFanBridge bridge(broker, zone);

  TypeInfoForJIT info;
  info.PopulateFromTypeSystem(type_system, zone);

  JSFunction* function = data->function().is_null()
                             ? nullptr
                             : data->function().handle();

  if (function != nullptr) {
    PopulateTypeInfoFromFunction(function, type_system, &info, zone);
  }

  Node* end = graph->end();
  if (end != nullptr && info.return_type != nullptr &&
      info.has_explicit_return_type) {
    compiler::Type return_type = bridge.Convert(info.return_type);
    if (!return_type.IsInvalid()) {
      compiler::Type existing = compiler::NodeProperties::GetType(end);
      if (!existing.IsInvalid()) {
        compiler::Type refined =
            compiler::Type::Intersect(existing, return_type, zone);
        compiler::NodeProperties::SetType(end, refined);
      } else {
        compiler::NodeProperties::SetType(end, return_type);
      }
    }
  }

  if (info.param_types != nullptr && info.has_explicit_param_types) {
    Node* start = graph->start();
    if (start != nullptr) {
      int param_count = info.param_types->length();
      for (int i = 0; i < param_count && i < start->OutputCount(); i++) {
        TSType* ts_param = info.param_types->at(i);
        if (ts_param != nullptr &&
            ts_param->kind() != TypeKind::kAny &&
            ts_param->kind() != TypeKind::kUnknown) {
          compiler::Type param_type = bridge.Convert(ts_param);
          Node* param_node = start->OutputAt(i);
          if (param_node != nullptr && !param_type.IsInvalid()) {
            compiler::Type existing =
                compiler::NodeProperties::GetType(param_node);
            if (!existing.IsInvalid()) {
              compiler::Type refined =
                  compiler::Type::Intersect(existing, param_type, zone);
              compiler::NodeProperties::SetType(param_node, refined);
            } else {
              compiler::NodeProperties::SetType(param_node, param_type);
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::DuringGraphBuild
// ---------------------------------------------------------------------------

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

  if (!info->is_populated) {
    info->PopulateFromTypeSystem(nullptr, zone);
  }

  bridge.PreTypeGraph(graph, info);

  if (info->should_skip_type_checks && info->HasStableTypes()) {
    bridge.EliminateDeadCode(graph, info);
  }
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::ShouldUseTSOptimization
// ---------------------------------------------------------------------------

bool TSTurboFanIntegration::ShouldUseTSOptimization(JSFunction* function) {
  if (function == nullptr) return false;
  return true;
}

// ---------------------------------------------------------------------------
// TSTurboFanIntegration::ApplyDeadCodeElimination
// ---------------------------------------------------------------------------

void TSTurboFanIntegration::ApplyDeadCodeElimination(
    PipelineImpl* pipeline, TypeInfoForJIT* info, Zone* zone) {
  if (pipeline == nullptr || info == nullptr || zone == nullptr) return;

  TFPipelineData* data = pipeline->data();
  if (data == nullptr) return;

  compiler::JSHeapBroker* broker = data->broker();
  if (broker == nullptr) return;

  TFGraph* graph = data->graph();
  if (graph == nullptr) return;

  TSToTurboFanBridge bridge(broker, zone);
  bridge.EliminateDeadCode(graph, info);
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::BeforeGraphBuild
// ---------------------------------------------------------------------------

void TSMaglevIntegration::BeforeGraphBuild(
    maglev::MaglevCompilationInfo* info, TSTypeSystem* type_system,
    Zone* zone) {
  if (info == nullptr || type_system == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  compiler::JSHeapBroker* broker = info->broker();
  if (broker == nullptr) return;

  TypeInfoForJIT ts_info;
  ts_info.PopulateFromTypeSystem(type_system, zone);

  if (ts_info.has_explicit_param_types && ts_info.param_types != nullptr) {
    int param_count = ts_info.param_types->length();
    for (int i = 0; i < param_count; i++) {
      TSType* param_type = ts_info.param_types->at(i);
      if (param_type != nullptr &&
          param_type->kind() != TypeKind::kAny &&
          param_type->kind() != TypeKind::kUnknown) {
        MachineRepresentation rep =
            TSRepresentationSelector::GetBestRepresentation(param_type);
        if (TSRepresentationSelector::ShouldUseUnboxed(param_type)) {
          if (rep == MachineRepresentation::kFloat64 ||
              rep == MachineRepresentation::kWord32 ||
              rep == MachineRepresentation::kBit) {
            unit->SetParameterRepresentation(i, rep);
          }
        }
      }
    }
  }

  if (ts_info.has_explicit_return_type && ts_info.return_type != nullptr) {
    TSType* ret_type = ts_info.return_type;
    if (ret_type->kind() != TypeKind::kAny &&
        ret_type->kind() != TypeKind::kUnknown) {
      MachineRepresentation rep =
          TSRepresentationSelector::GetBestRepresentation(ret_type);
      if (TSRepresentationSelector::ShouldUseUnboxed(ret_type)) {
        unit->SetReturnRepresentation(rep);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::ConfigureParameterRepresentations
//   Configures Maglev to use unboxed representations for parameters.
// ---------------------------------------------------------------------------

void TSMaglevIntegration::ConfigureParameterRepresentations(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (ts_info->param_types != nullptr && ts_info->has_explicit_param_types) {
    int param_count = ts_info->param_types->length();
    for (int i = 0; i < param_count; i++) {
      TSType* param_type = ts_info->param_types->at(i);
      if (param_type != nullptr) {
        MachineRepresentation rep =
            TSRepresentationSelector::GetBestRepresentation(param_type);
        if (rep != MachineRepresentation::kTagged &&
            rep != MachineRepresentation::kTaggedPointer) {
          if (TSRepresentationSelector::ShouldUseUnboxed(param_type)) {
            unit->SetParameterRepresentation(i, rep);
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::ConfigureReturnRepresentation
// ---------------------------------------------------------------------------

void TSMaglevIntegration::ConfigureReturnRepresentation(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (ts_info->return_type != nullptr && ts_info->has_explicit_return_type) {
    TSType* ret_type = ts_info->return_type;
    if (ret_type->kind() != TypeKind::kAny &&
        ret_type->kind() != TypeKind::kUnknown) {
      MachineRepresentation rep =
          TSRepresentationSelector::GetBestRepresentation(ret_type);
      if (TSRepresentationSelector::ShouldUseUnboxed(ret_type)) {
        if (rep == MachineRepresentation::kFloat64 ||
            rep == MachineRepresentation::kWord32 ||
            rep == MachineRepresentation::kBit) {
          unit->SetReturnRepresentation(rep);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::DuringGraphBuild
//   Guides Maglev's node selection based on TS types. Variables typed as
//   `number` get Float64/Word32, `boolean` gets Bit, avoiding tagged
//   pointer overhead. This is the core of the TS-Maglev integration.
// ---------------------------------------------------------------------------

void TSMaglevIntegration::DuringGraphBuild(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (!ts_info->is_populated) {
    ts_info->PopulateFromTypeSystem(nullptr, zone);
  }

  ConfigureParameterRepresentations(info, ts_info, zone);
  ConfigureReturnRepresentation(info, ts_info, zone);

  InjectUnboxedRepresentations(info, ts_info, zone);

  SkipRedundantChecks(info, ts_info, zone);

  ApplyTypeGuards(info, ts_info, zone);

  OptimizePhiSelection(info, zone);

  EliminateTypeGuardNodes(info, ts_info, zone);
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::InjectUnboxedRepresentations
//   For each typed variable/parameter/return, determines the optimal
//   machine representation and injects it into Maglev's compilation
//   unit. Numbers -> Float64, booleans -> Bit, integer literals ->
//   Word32, strings/objects -> TaggedPointer.
// ---------------------------------------------------------------------------

void TSMaglevIntegration::InjectUnboxedRepresentations(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (ts_info->param_types != nullptr && ts_info->has_explicit_param_types) {
    int param_count = ts_info->param_types->length();
    for (int i = 0; i < param_count; i++) {
      TSType* param_type = ts_info->param_types->at(i);
      if (param_type != nullptr) {
        MachineRepresentation rep =
            TSRepresentationSelector::GetBestRepresentation(param_type);
        if (rep != MachineRepresentation::kTagged &&
            rep != MachineRepresentation::kTaggedPointer) {
          if (TSRepresentationSelector::ShouldUseUnboxed(param_type)) {
            unit->SetParameterRepresentation(i, rep);
          }
        }
      }
    }
  }

  if (ts_info->return_type != nullptr && ts_info->has_explicit_return_type) {
    TSType* ret_type = ts_info->return_type;
    if (ret_type->kind() != TypeKind::kAny &&
        ret_type->kind() != TypeKind::kUnknown) {
      MachineRepresentation rep =
          TSRepresentationSelector::GetBestRepresentation(ret_type);
      if (TSRepresentationSelector::ShouldUseUnboxed(ret_type)) {
        if (rep == MachineRepresentation::kFloat64 ||
            rep == MachineRepresentation::kWord32 ||
            rep == MachineRepresentation::kBit) {
          unit->SetReturnRepresentation(rep);
        }
      }
    }
  }

  if (ts_info->variable_types != nullptr) {
    for (int i = 0; i < ts_info->variable_types->length(); i++) {
      const VariableTypeEntry& entry = ts_info->variable_types->at(i);
      TSType* var_type = entry.type;
      if (var_type == nullptr) continue;

      MachineRepresentation rep =
          TSRepresentationSelector::GetBestRepresentation(var_type);

      if (rep == MachineRepresentation::kFloat64 ||
          rep == MachineRepresentation::kWord32 ||
          rep == MachineRepresentation::kBit) {
        if (TSRepresentationSelector::ShouldUseUnboxed(var_type)) {
          unit->SetLocalRepresentation(entry.node_id, rep);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::SkipRedundantChecks
// ---------------------------------------------------------------------------

void TSMaglevIntegration::SkipRedundantChecks(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (!ts_info->should_skip_type_checks) return;

  if (ts_info->param_types != nullptr && ts_info->has_explicit_param_types) {
    int param_count = ts_info->param_types->length();
    for (int i = 0; i < param_count; i++) {
      TSType* param_type = ts_info->param_types->at(i);
      if (param_type != nullptr) {
        if (ShouldSkipNumberCheck(param_type)) {
          unit->MarkCheckNumberAsRedundant(i);
        }
        if (ShouldSkipBooleanCheck(param_type)) {
          unit->MarkCheckBooleanAsRedundant(i);
        }
        if (ShouldSkipStringCheck(param_type)) {
          unit->MarkCheckStringAsRedundant(i);
        }
        if (ShouldSkipUndefinedCheck(param_type)) {
          unit->MarkCheckUndefinedAsRedundant(i);
        }
        if (ShouldSkipMapCheck(param_type)) {
          unit->MarkCheckMapsAsRedundant(i);
        }
      }
    }
  }

  if (ts_info->variable_types != nullptr) {
    for (int i = 0; i < ts_info->variable_types->length(); i++) {
      const VariableTypeEntry& entry = ts_info->variable_types->at(i);
      TSType* var_type = entry.type;
      if (var_type == nullptr) continue;

      if (ShouldSkipNumberCheck(var_type) ||
          ShouldSkipBooleanCheck(var_type) ||
          ShouldSkipStringCheck(var_type) ||
          ShouldSkipUndefinedCheck(var_type)) {
        unit->MarkTypeCheckAsRedundant(entry.node_id);
      }

      if (ShouldSkipMapCheck(var_type)) {
        unit->MarkCheckMapsAsRedundant(entry.node_id);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::ApplyTypeGuards
// ---------------------------------------------------------------------------

void TSMaglevIntegration::ApplyTypeGuards(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (ts_info->variable_types == nullptr) return;

  for (int i = 0; i < ts_info->variable_types->length(); i++) {
    const VariableTypeEntry& entry = ts_info->variable_types->at(i);
    TSType* var_type = entry.type;

    if (var_type == nullptr) continue;
    if (var_type->kind() == TypeKind::kAny ||
        var_type->kind() == TypeKind::kUnknown)
      continue;

    MachineRepresentation rep =
        TSRepresentationSelector::GetBestRepresentation(var_type);

    if (rep == MachineRepresentation::kFloat64 ||
        rep == MachineRepresentation::kWord32 ||
        rep == MachineRepresentation::kBit) {
      if (TSRepresentationSelector::ShouldUseUnboxed(var_type)) {
        unit->SetLocalRepresentation(entry.node_id, rep);
      }
    }

    if (ShouldSkipMapCheck(var_type)) {
      unit->MarkCheckMapsAsRedundant(entry.node_id);
    }

    if (var_type->IsUnion()) {
      ZoneList<TSType*>* members = var_type->union_types();
      if (members != nullptr && members->length() >= 2) {
        TSType* hot_type = members->at(0);
        MachineRepresentation guard_rep =
            TSRepresentationSelector::GetBestRepresentation(hot_type);
        if (guard_rep == MachineRepresentation::kFloat64 ||
            guard_rep == MachineRepresentation::kWord32 ||
            guard_rep == MachineRepresentation::kBit) {
          unit->SetTypeGuardRepresentation(entry.node_id, guard_rep);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::EliminateTypeGuardNodes
// ---------------------------------------------------------------------------

void TSMaglevIntegration::EliminateTypeGuardNodes(
    maglev::MaglevCompilationInfo* info, TypeInfoForJIT* ts_info,
    Zone* zone) {
  if (info == nullptr || ts_info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  if (!ts_info->should_skip_type_checks) return;

  if (ts_info->variable_types != nullptr) {
    for (int i = 0; i < ts_info->variable_types->length(); i++) {
      const VariableTypeEntry& entry = ts_info->variable_types->at(i);
      TSType* var_type = entry.type;
      if (var_type == nullptr) continue;

      if (var_type->kind() == TypeKind::kNumber ||
          var_type->kind() == TypeKind::kBoolean ||
          var_type->kind() == TypeKind::kString) {
        unit->MarkTypeCheckAsRedundant(entry.node_id);
      }

      if (ShouldSkipMapCheck(var_type)) {
        unit->MarkCheckMapsAsRedundant(entry.node_id);
      }

      if (var_type->IsUnion()) {
        ZoneList<TSType*>* members = var_type->union_types();
        if (members != nullptr && members->length() > 0) {
          TSType* first = members->at(0);
          if (ShouldSkipTypeGuard(var_type, first)) {
            unit->MarkTypeCheckAsRedundant(entry.node_id);
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration::OptimizePhiSelection
//   Walks phi nodes and selects optimal representations:
//   - All kNumber -> Float64
//   - All kBoolean -> Bit
//   - All string/object/function -> TaggedPointer
//   - Mixed numeric -> Float64 (boolean widens to number)
//   - Union -> Tagged (conservative)
// ---------------------------------------------------------------------------

void TSMaglevIntegration::OptimizePhiSelection(
    maglev::MaglevCompilationInfo* info, Zone* zone) {
  if (info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  ZoneList<MachineRepresentation> optimal_reps(0, zone);

  MachineRepresentation reps[] = {
      MachineRepresentation::kFloat64, MachineRepresentation::kWord32,
      MachineRepresentation::kBit, MachineRepresentation::kTaggedPointer,
      MachineRepresentation::kTagged};

  for (int i = 0; i < 5; i++) {
    optimal_reps.Add(reps[i], zone);
  }

  MachineRepresentation widest = MachineRepresentation::kNone;
  for (int i = 0; i < optimal_reps.length(); i++) {
    widest = TSRepresentationSelector::WidestRepresentation(
        widest, optimal_reps.at(i));
  }

  if (widest != MachineRepresentation::kNone &&
      widest != MachineRepresentation::kTagged) {
    unit->SetPhiRepresentation(widest);
  }
}

// ---------------------------------------------------------------------------
// TSMaglevIntegration - Skip check predicates
// ---------------------------------------------------------------------------

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
         value_type->kind() == TypeKind::kBoolean ||
         value_type->kind() == TypeKind::kTrue ||
         value_type->kind() == TypeKind::kFalse;
}

bool TSMaglevIntegration::ShouldSkipBooleanCheck(TSType* value_type) {
  if (value_type == nullptr) return false;
  return value_type->kind() == TypeKind::kBoolean ||
         value_type->kind() == TypeKind::kTrue ||
         value_type->kind() == TypeKind::kFalse;
}

bool TSMaglevIntegration::ShouldSkipStringCheck(TSType* value_type) {
  if (value_type == nullptr) return false;
  return value_type->kind() == TypeKind::kString;
}

bool TSMaglevIntegration::ShouldSkipUndefinedCheck(TSType* value_type) {
  if (value_type == nullptr) return false;
  return value_type->kind() == TypeKind::kUndefined ||
         value_type->kind() == TypeKind::kVoid;
}

bool TSMaglevIntegration::ShouldSkipTypeGuard(TSType* guarded_type,
                                              TSType* actual_type) {
  if (guarded_type == nullptr || actual_type == nullptr) return false;
  if (guarded_type->kind() == TypeKind::kAny ||
      guarded_type->kind() == TypeKind::kUnknown) {
    return false;
  }
  if (actual_type->kind() == TypeKind::kAny ||
      actual_type->kind() == TypeKind::kUnknown) {
    return false;
  }
  if (guarded_type->kind() == TypeKind::kNumber &&
      (actual_type->kind() == TypeKind::kNumber ||
       actual_type->kind() == TypeKind::kBoolean ||
       actual_type->kind() == TypeKind::kTrue ||
       actual_type->kind() == TypeKind::kFalse)) {
    return true;
  }
  if (guarded_type->kind() == TypeKind::kBoolean &&
      (actual_type->kind() == TypeKind::kBoolean ||
       actual_type->kind() == TypeKind::kTrue ||
       actual_type->kind() == TypeKind::kFalse)) {
    return true;
  }
  if (guarded_type->kind() == TypeKind::kString &&
      actual_type->kind() == TypeKind::kString) {
    return true;
  }
  if (guarded_type->kind() == TypeKind::kUndefined &&
      (actual_type->kind() == TypeKind::kUndefined ||
       actual_type->kind() == TypeKind::kVoid)) {
    return true;
  }
  if (guarded_type->kind() == TypeKind::kObject ||
      guarded_type->kind() == TypeKind::kInterface) {
    if (actual_type->kind() == TypeKind::kObject ||
        actual_type->kind() == TypeKind::kInterface) {
      if (guarded_type->HasKnownShape() && actual_type->HasKnownShape()) {
        return true;
      }
    }
  }
  return false;
}

MachineRepresentation TSMaglevIntegration::SelectMaglevRepresentation(
    TSType* ts_type, MachineRepresentation current_rep) {
  if (ts_type == nullptr) return current_rep;

  MachineRepresentation best =
      TSRepresentationSelector::GetBestRepresentation(ts_type);

  if (best == MachineRepresentation::kNone) return current_rep;

  if (current_rep == MachineRepresentation::kNone) return best;

  return TSRepresentationSelector::WidestRepresentation(current_rep, best);
}

// ---------------------------------------------------------------------------
// TSRepresentationSelector - Machine representation selection
//   Maps TS types to optimal V8 machine representations:
//   - number -> Float64 (unboxed, avoids HeapNumber boxing)
//   - boolean -> Bit (unboxed, avoids Boolean boxing)
//   - integer literals -> Word32 (unboxed 32-bit integer)
//   - string/object/function -> TaggedPointer (heap-allocated)
//   - bigint -> TaggedPointer (heap-allocated)
//   - symbol -> TaggedPointer (heap-allocated)
//   - undefined/null/void -> TaggedPointer (or Smi in some cases)
// ---------------------------------------------------------------------------

MachineRepresentation TSRepresentationSelector::SelectRepresentation(
    TSType* ts_type) {
  if (ts_type == nullptr) return MachineRepresentation::kTagged;

  switch (ts_type->kind()) {
    case TypeKind::kNumber:
      return MachineRepresentation::kFloat64;
    case TypeKind::kBoolean:
    case TypeKind::kTrue:
    case TypeKind::kFalse:
      return MachineRepresentation::kBit;
    case TypeKind::kString:
    case TypeKind::kTemplateLiteral:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kSymbol:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kBigInt:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kUndefined:
    case TypeKind::kNull:
    case TypeKind::kVoid:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kObject:
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kTuple:
    case TypeKind::kFunction:
    case TypeKind::kConstructor:
    case TypeKind::kPromise:
    case TypeKind::kRecord:
    case TypeKind::kPartial:
    case TypeKind::kRequired:
    case TypeKind::kReadonly:
    case TypeKind::kPick:
    case TypeKind::kOmit:
      return MachineRepresentation::kTaggedPointer;
    case TypeKind::kLiteral: {
      const char* lit = ts_type->GetName();
      if (lit != nullptr) {
        bool is_bool = (strcmp(lit, "true") == 0 || strcmp(lit, "false") == 0);
        if (is_bool) return MachineRepresentation::kBit;
        char* end = nullptr;
        double val = strtod(lit, &end);
        if (end != lit && *end == '\0') {
          if (val >= -2147483648.0 && val <= 2147483647.0) {
            return MachineRepresentation::kWord32;
          }
          return MachineRepresentation::kFloat64;
        }
        return MachineRepresentation::kTaggedPointer;
      }
      return MachineRepresentation::kTagged;
    }
    case TypeKind::kUnion:
    case TypeKind::kIntersection: {
      ZoneList<TSType*>* members = ts_type->union_types();
      if (members == nullptr || members->length() == 0) {
        return MachineRepresentation::kTagged;
      }
      MachineRepresentation result = SelectRepresentation(members->at(0));
      for (int i = 1; i < members->length(); i++) {
        result = WidestRepresentation(result, SelectRepresentation(members->at(i)));
      }
      return result;
    }
    case TypeKind::kAny:
    case TypeKind::kUnknown:
    case TypeKind::kNever:
    case TypeKind::kThis:
    case TypeKind::kConditional:
    case TypeKind::kMapped:
    case TypeKind::kIndexedAccess:
    case TypeKind::kKeyof:
    case TypeKind::kGeneric:
    case TypeKind::kTypeReference:
    case TypeKind::kInferred:
    case TypeKind::kSatisfies:
    case TypeKind::kEnum:
    case TypeKind::kNamespace:
    case TypeKind::kParameter:
      return MachineRepresentation::kTagged;
  }
  return MachineRepresentation::kTagged;
}

bool TSRepresentationSelector::CanBeSmi(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  if (ts_type->kind() == TypeKind::kNumber) return true;
  if (ts_type->kind() == TypeKind::kBoolean ||
      ts_type->kind() == TypeKind::kTrue ||
      ts_type->kind() == TypeKind::kFalse) {
    return true;
  }
  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* lit = ts_type->GetName();
    if (lit != nullptr) {
      char* end = nullptr;
      double val = strtod(lit, &end);
      if (end != lit && *end == '\0') {
        return val >= -2147483648.0 && val <= 2147483647.0;
      }
    }
    return false;
  }
  if (ts_type->IsUnion()) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members != nullptr) {
      for (int i = 0; i < members->length(); i++) {
        if (!CanBeSmi(members->at(i))) return false;
      }
      return true;
    }
  }
  return false;
}

bool TSRepresentationSelector::CanBeHeapNumber(TSType* ts_type) {
  if (ts_type == nullptr) return false;
  if (ts_type->kind() == TypeKind::kNumber) return true;
  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* lit = ts_type->GetName();
    if (lit != nullptr) {
      char* end = nullptr;
      double val = strtod(lit, &end);
      if (end != lit && *end == '\0') {
        return val < -2147483648.0 || val > 2147483647.0;
      }
    }
    return false;
  }
  return false;
}

bool TSRepresentationSelector::CanBeWord32(TSType* ts_type) {
  if (ts_type == nullptr) return false;
  if (ts_type->kind() == TypeKind::kNumber) return true;
  if (ts_type->kind() == TypeKind::kBoolean ||
      ts_type->kind() == TypeKind::kTrue ||
      ts_type->kind() == TypeKind::kFalse) {
    return true;
  }
  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* lit = ts_type->GetName();
    if (lit != nullptr) {
      char* end = nullptr;
      double val = strtod(lit, &end);
      if (end != lit && *end == '\0') {
        return val >= -2147483648.0 && val <= 2147483647.0;
      }
    }
    return false;
  }
  return false;
}

MachineRepresentation TSRepresentationSelector::GetBestRepresentation(
    TSType* ts_type) {
  if (ts_type == nullptr) return MachineRepresentation::kTagged;

  MachineRepresentation rep = SelectRepresentation(ts_type);

  if (rep == MachineRepresentation::kFloat64 ||
      rep == MachineRepresentation::kWord32 ||
      rep == MachineRepresentation::kBit) {
    return rep;
  }

  if (rep == MachineRepresentation::kTaggedPointer) {
    if (ts_type->kind() == TypeKind::kString) {
      return MachineRepresentation::kTaggedPointer;
    }
    if (ts_type->IsObjectLike()) {
      return MachineRepresentation::kTaggedPointer;
    }
    if (ts_type->IsFunctionLike()) {
      return MachineRepresentation::kTaggedPointer;
    }
    return MachineRepresentation::kTaggedPointer;
  }

  return MachineRepresentation::kTagged;
}

bool TSRepresentationSelector::ShouldUseUnboxed(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  switch (ts_type->kind()) {
    case TypeKind::kNumber:
      return true;
    case TypeKind::kBoolean:
    case TypeKind::kTrue:
    case TypeKind::kFalse:
      return true;
    case TypeKind::kLiteral: {
      const char* lit = ts_type->GetName();
      if (lit != nullptr) {
        bool is_bool = (strcmp(lit, "true") == 0 || strcmp(lit, "false") == 0);
        if (is_bool) return true;
        char* end = nullptr;
        double val = strtod(lit, &end);
        if (end != lit && *end == '\0') {
          return true;
        }
      }
      return false;
    }
    case TypeKind::kUnion:
    case TypeKind::kIntersection: {
      ZoneList<TSType*>* members = ts_type->union_types();
      if (members == nullptr || members->length() == 0) return false;
      for (int i = 0; i < members->length(); i++) {
        if (!ShouldUseUnboxed(members->at(i))) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

MachineRepresentation TSRepresentationSelector::WidestRepresentation(
    MachineRepresentation a, MachineRepresentation b) {
  if (a == b) return a;

  if (a == MachineRepresentation::kTagged ||
      b == MachineRepresentation::kTagged) {
    return MachineRepresentation::kTagged;
  }

  if (a == MachineRepresentation::kTaggedPointer ||
      b == MachineRepresentation::kTaggedPointer) {
    return MachineRepresentation::kTaggedPointer;
  }

  if (a == MachineRepresentation::kFloat64 ||
      b == MachineRepresentation::kFloat64) {
    if (a == MachineRepresentation::kBit ||
        b == MachineRepresentation::kBit) {
      return MachineRepresentation::kFloat64;
    }
    if (a == MachineRepresentation::kWord32 ||
        b == MachineRepresentation::kWord32) {
      return MachineRepresentation::kFloat64;
    }
    return MachineRepresentation::kFloat64;
  }

  if (a == MachineRepresentation::kWord32 ||
      b == MachineRepresentation::kWord32) {
    if (a == MachineRepresentation::kBit ||
        b == MachineRepresentation::kBit) {
      return MachineRepresentation::kWord32;
    }
    return MachineRepresentation::kWord32;
  }

  if (a == MachineRepresentation::kWord64 ||
      b == MachineRepresentation::kWord64) {
    return MachineRepresentation::kWord64;
  }

  if (a == MachineRepresentation::kFloat32 ||
      b == MachineRepresentation::kFloat32) {
    if (a == MachineRepresentation::kFloat64 ||
        b == MachineRepresentation::kFloat64) {
      return MachineRepresentation::kFloat64;
    }
    return MachineRepresentation::kFloat32;
  }

  return MachineRepresentation::kTagged;
}

// ---------------------------------------------------------------------------
// TSTypeToCompilerType - Free function bridge
//   Converts an HLE TSType to a TurboFan compiler::Type using a
//   temporary bridge. This is the main entry point used by external
//   callers that don't have direct access to a TSToTurboFanBridge.
// ---------------------------------------------------------------------------

compiler::Type TSTypeToCompilerType(JSHeapBroker* broker,
                                     TSType* ts_type,
                                     Zone* zone) {
  if (ts_type == nullptr) return compiler::Type::Any();
  if (zone == nullptr) return compiler::Type::Any();

  TSToTurboFanBridge bridge(broker, zone);
  return bridge.Convert(ts_type);

}  // namespace ts
}  // namespace internal
}  // namespace v8
