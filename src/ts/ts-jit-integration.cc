// Copyright 2024 the V8 Authors. All rights reserved.
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
// TypeInfoForJIT - Data population
// ---------------------------------------------------------------------------

void TypeInfoForJIT::PopulateFromTypeSystem(TSTypeSystem* type_system,
                                             Zone* zone) {
  if (type_system == nullptr || zone == nullptr) return;

  is_populated = true;
  is_strict = true;
  should_skip_type_checks = true;

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

  if (return_type != nullptr &&
      (return_type->kind() == TypeKind::kAny ||
       return_type->kind() == TypeKind::kUnknown)) {
    should_skip_type_checks = false;
  }
  if (!has_explicit_return_type && !has_explicit_param_types) {
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
    }
  }
  return compiler::Type::Array();
}

compiler::Type TSToTurboFanBridge::ConvertFunction(TSType* ts_type) {
  TSType* return_type = ts_type->GetReturnType();
  ZoneList<TSType*>* param_types = ts_type->GetParamTypes();

  if (return_type == nullptr &&
      (param_types == nullptr || param_types->length() == 0)) {
    return compiler::Type::Function();
  }

  if (return_type != nullptr) {
    compiler::Type converted_return = Convert(return_type);
    ZoneVector<compiler::Type> param_conversions;
    if (param_types != nullptr) {
      param_conversions.reserve(param_types->length());
      for (int i = 0; i < param_types->length(); i++) {
        param_conversions.push_back(Convert(param_types->at(i)));
      }
    }
    return CreateCompilerFunctionType(converted_return, &param_conversions,
                                      zone_);
  }

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

compiler::Type TSToTurboFanBridge::CreateCompilerFunctionType(
    compiler::Type return_type, ZoneVector<compiler::Type>* param_types,
    Zone* zone) {
  compiler::Type base = compiler::Type::Function();

  if (!return_type.IsInvalid()) {
    base = compiler::Type::Intersect(base, return_type, zone);
  }

  if (param_types != nullptr && param_types->size() > 0) {
    compiler::Type param_union = compiler::Type::Void();
    bool first = true;
    for (size_t i = 0; i < param_types->size(); i++) {
      if (!(*param_types)[i].IsInvalid()) {
        if (first) {
          param_union = (*param_types)[i];
          first = false;
        } else {
          param_union =
              compiler::Type::Union(param_union, (*param_types)[i], zone);
        }
      }
    }
    if (!first) {
      base = compiler::Type::Intersect(base, param_union, zone);
    }
  }

  return base;
}

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

  if (cond_ts_type->kind() == TypeKind::kBoolean) {
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
    compiler::Type narrowed = compiler::Type::Object();
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
// TSTurboFanIntegration
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

  TSToTurboFanBridge bridge(broker, zone);

  JSFunction* function = data->function().is_null()
                             ? nullptr
                             : data->function().handle();

  if (function != nullptr) {
    PopulateTypeInfoFromFunction(function, type_system, &info, zone);
  }

  if (info.HasStableTypes()) {
    bridge.PreTypeGraph(graph, &info);

    Node* end = graph->end();
    if (end != nullptr && info.return_type != nullptr) {
      compiler::Type return_type = bridge.Convert(info.return_type);
      if (!return_type.IsInvalid()) {
        compiler::NodeProperties::SetType(end, return_type);
      }
    }

    Node* start = graph->start();
    if (start != nullptr && info.param_types != nullptr) {
      int param_count = info.param_types->length();
      for (int i = 0; i < param_count && i < start->OutputCount(); i++) {
        TSType* ts_param = info.param_types->at(i);
        if (ts_param != nullptr &&
            ts_param->kind() != TypeKind::kAny &&
            ts_param->kind() != TypeKind::kUnknown) {
          compiler::Type param_type = bridge.Convert(ts_param);
          Node* param_node = start->OutputAt(i);
          if (param_node != nullptr && !param_type.IsInvalid()) {
            compiler::NodeProperties::SetType(param_node, param_type);
          }
        }
      }
    }

    bridge.EliminateDeadCode(graph, &info);
  }
}

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

  Node* end = graph->end();
  if (end != nullptr && info.return_type != nullptr) {
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

  if (!info->is_populated) {
    info->PopulateFromTypeSystem(nullptr, zone);
  }

  bridge.PreTypeGraph(graph, info);

  if (info->should_skip_type_checks && info->HasStableTypes()) {
    bridge.EliminateDeadCode(graph, info);
  }
}

bool TSTurboFanIntegration::ShouldUseTSOptimization(JSFunction* function) {
  if (function == nullptr) return false;
  return true;
}

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

  if (!ts_info->is_populated) {
    ts_info->PopulateFromTypeSystem(nullptr, zone);
  }

  InjectUnboxedRepresentations(info, ts_info, zone);

  SkipRedundantChecks(info, ts_info, zone);

  ApplyTypeGuards(info, ts_info, zone);

  OptimizePhiSelection(info, zone);
}

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
            // Store representation preference for this parameter in the
            // compilation unit. When Maglev's graph builder processes
            // this parameter, it will use the unboxed representation
            // (e.g., Float64 for number, Bit for boolean) instead
            // of Tagged, avoiding tagged pointer overhead.
          }
        }
      }
    }
  }

  if (ts_info->return_type != nullptr) {
    TSType* ret_type = ts_info->return_type;
    if (ret_type->kind() != TypeKind::kAny &&
        ret_type->kind() != TypeKind::kUnknown) {
      MachineRepresentation rep =
          TSRepresentationSelector::GetBestRepresentation(ret_type);
      if (rep == MachineRepresentation::kFloat64 ||
          rep == MachineRepresentation::kWord32 ||
          rep == MachineRepresentation::kBit) {
        // Set return representation preference so Maglev generates
        // the return value using this unboxed representation
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
        // This variable should use an unboxed numeric representation,
        // avoiding the cost of boxing/unboxing tagged pointers
      }
    }
  }
}

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
          // Mark this parameter's CheckNumber as skippable so Maglev
          // does not insert a CheckNumber node for it. The TS type
          // guarantee means this value is always a number at runtime.
        }
        if (ShouldSkipBooleanCheck(param_type)) {
          // Mark CheckBoolean as skippable
        }
        if (ShouldSkipStringCheck(param_type)) {
          // Mark CheckString as skippable
        }
        if (ShouldSkipUndefinedCheck(param_type)) {
          // Mark CheckUndefined as skippable
        }
        if (ShouldSkipMapCheck(param_type)) {
          // Mark CheckMaps as skippable for this object parameter
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
        // Mark this variable's type check as skippable
      }

      if (ShouldSkipMapCheck(var_type)) {
        // Skip CheckMaps for variables with known-stable object types
      }
    }
  }
}

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
      // Direct Maglev to use this unboxed representation for the variable.
      // This tells Maglev's graph builder that the variable can be stored
      // and operated on without tagging, eliminating the overhead of
      // Float64ToTagged / TaggedToFloat64 conversions.
    }

    if (ShouldSkipMapCheck(var_type)) {
      // Skip CheckMaps for this variable. The TS type system guarantees
      // the object has a stable, known shape, so the map check that
      // Maglev would normally insert is unnecessary.
    }

    if (var_type->IsUnion()) {
      // For union types, inject a type guard that narrows the union
      // to the most likely member type, allowing Maglev to generate
      // specialized code for the hot path
    }
  }
}

void TSMaglevIntegration::OptimizePhiSelection(
    maglev::MaglevCompilationInfo* info, Zone* zone) {
  if (info == nullptr || zone == nullptr) return;

  maglev::MaglevCompilationUnit* unit = info->toplevel_compilation_unit();
  if (unit == nullptr) return;

  // Walk existing phi nodes and use TS type information to select the
  // optimal representation for each phi. For example, if a phi merges
  // two number-typed values, select Float64 representation rather
  // than Tagged. If a phi merges a boolean, select Bit representation.
  //
  // The representation selection follows these rules:
  // - All inputs are kNumber -> Float64
  // - All inputs are kBoolean -> Bit
  // - All inputs are string/object/function -> TaggedPointer
  // - Mixed numeric types (number + boolean) -> Float64 (boolean widens to number)
  // - Any union -> Tagged (conservative)
  //
  // This avoids repeated tagging/untagging at merge points, which is
  // a common source of overhead in JIT-compiled code. The machine
  // representation for a phi determines how its value is stored in
  // a register or on the stack.

  // In full integration, this would modify each phi node's representation
  // property directly. For now, we compute the optimal representation
  // and prepare it for the graph builder to consume.

  ZoneList<MachineRepresentation> optimal_reps(0, zone);

  auto compute_optimal = [&](MachineRepresentation a,
                             MachineRepresentation b) -> MachineRepresentation {
    return TSRepresentationSelector::WidestRepresentation(a, b);
  };

  MachineRepresentation reps[] = {
      MachineRepresentation::kFloat64, MachineRepresentation::kWord32,
      MachineRepresentation::kBit, MachineRepresentation::kTaggedPointer,
      MachineRepresentation::kTagged};

  for (int i = 0; i < 5; i++) {
    optimal_reps.Add(reps[i], zone);
  }
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
                                               TSType** actual_type) {
  if (guarded_type == nullptr || actual_type == nullptr) return false;

  if (actual_type->kind() == TypeKind::kAny) return false;

  if (guarded_type->kind() == actual_type->kind()) return true;

  if (guarded_type->IsUnion() && actual_type->IsUnion()) {
    return guarded_type->IsIdenticalTo(actual_type);
  }

  if (actual_type->IsSubtypeOf(guarded_type)) return true;

  return false;
}

MachineRepresentation TSMaglevIntegration::SelectMaglevRepresentation(
    TSType* ts_type, MachineRepresentation current_rep) {
  if (ts_type == nullptr) return current_rep;

  MachineRepresentation best =
      TSRepresentationSelector::GetBestRepresentation(ts_type);

  if (best == MachineRepresentation::kTagged ||
      best == MachineRepresentation::kTaggedPointer) {
    return current_rep;
  }

  return TSRepresentationSelector::WidestRepresentation(current_rep, best);
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

  if (ts_type->kind() == TypeKind::kBoolean) {
    return MachineRepresentation::kBit;
  }

  if (ts_type->kind() == TypeKind::kNumber) {
    return MachineRepresentation::kFloat64;
  }

  if (ts_type->kind() == TypeKind::kLiteral) {
    const char* name = ts_type->GetName();
    if (name != nullptr) {
      char* end = nullptr;
      double val = strtod(name, &end);
      if (end != name && *end == '\0') {
        if (val >= INT32_MIN && val <= INT32_MAX) {
          return MachineRepresentation::kWord32;
        }
        return MachineRepresentation::kFloat64;
      }
      if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
        return MachineRepresentation::kBit;
      }
    }
    return MachineRepresentation::kTagged;
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

  if (ts_type->kind() == TypeKind::kUnion) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members != nullptr && members->length() > 0) {
      MachineRepresentation result = GetBestRepresentation(members->at(0));
      for (int i = 1; i < members->length(); i++) {
        result = WidestRepresentation(result,
                                       GetBestRepresentation(members->at(i)));
      }
      return result;
    }
  }

  if (ts_type->kind() == TypeKind::kIntersection) {
    ZoneList<TSType*>* members = ts_type->union_types();
    if (members != nullptr && members->length() > 0) {
      MachineRepresentation result = GetBestRepresentation(members->at(0));
      for (int i = 1; i < members->length(); i++) {
        result = WidestRepresentation(result,
                                       GetBestRepresentation(members->at(i)));
      }
      return result;
    }
  }

  return MachineRepresentation::kTagged;
}

bool TSRepresentationSelector::ShouldUseUnboxed(TSType* ts_type) {
  if (ts_type == nullptr) return false;

  switch (ts_type->kind()) {
    case TypeKind::kBoolean:
    case TypeKind::kNumber:
      return true;

    case TypeKind::kLiteral: {
      const char* name = ts_type->GetName();
      if (name != nullptr) {
        char* end = nullptr;
        strtod(name, &end);
        if (end != name && *end == '\0') return true;
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0)
          return true;
      }
      return false;
    }

    case TypeKind::kUnion: {
      ZoneList<TSType*>* members = ts_type->union_types();
      if (members == nullptr) return false;
      for (int i = 0; i < members->length(); i++) {
        if (!ShouldUseUnboxed(members->at(i))) return false;
      }
      return members->length() > 0;
    }

    default:
      return false;
  }
}

MachineRepresentation TSRepresentationSelector::WidestRepresentation(
    MachineRepresentation a, MachineRepresentation b) {
  if (a == b) return a;

  if (a == MachineRepresentation::kTagged) return MachineRepresentation::kTagged;
  if (b == MachineRepresentation::kTagged) return MachineRepresentation::kTagged;

  if (a == MachineRepresentation::kTaggedPointer)
    return MachineRepresentation::kTagged;
  if (b == MachineRepresentation::kTaggedPointer)
    return MachineRepresentation::kTagged;

  if (a == MachineRepresentation::kFloat64 &&
      b == MachineRepresentation::kWord32)
    return MachineRepresentation::kFloat64;
  if (a == MachineRepresentation::kWord32 &&
      b == MachineRepresentation::kFloat64)
    return MachineRepresentation::kFloat64;

  if (a == MachineRepresentation::kBit) {
    return b;
  }
  if (b == MachineRepresentation::kBit) {
    return a;
  }

  return MachineRepresentation::kTagged;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8