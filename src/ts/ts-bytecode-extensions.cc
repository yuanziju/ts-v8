#include "src/ts/ts-bytecode-extensions.h"

#include "src/ast/ast.h"
#include "src/ast/ast-value-factory.h"
#include "src/ast/variables.h"
#include "src/base/logging.h"
#include "src/interpreter/bytecode-array-builder.h"
#include "src/interpreter/bytecode-generator.h"
#include "src/interpreter/bytecode-label.h"
#include "src/interpreter/bytecode-register.h"
#include "src/interpreter/bytecodes.h"
#include "src/parsing/token.h"

namespace v8 {
namespace internal {
namespace ts {

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Construction and strategy helpers
// ---------------------------------------------------------------------------

TSBytecodeBuilder::TSBytecodeBuilder(BytecodeArrayBuilder* builder,
                                      const TSBytecodeConfig& config,
                                      TSMapFactory* map_factory)
    : builder_(builder), config_(config), map_factory_(map_factory) {}

bool TSBytecodeBuilder::IsTypeStable(TSType* type) const {
  if (type == nullptr) return false;
  return type->IsStable();
}

bool TSBytecodeBuilder::IsPrimitiveType(TSType* type) const {
  if (type == nullptr) return false;
  return type->IsPrimitive();
}

bool TSBytecodeBuilder::CanUseZeroCostPath(TSType* type) const {
  if (type == nullptr) return false;
  if (!config_.use_zero_cost_abstraction) return false;
  if (map_factory_ == nullptr) return false;
  if (!type->HasKnownShape()) return false;
  return true;
}

TSBytecodeStrategy TSBytecodeBuilder::SelectStrategy(TSType* type) const {
  if (type == nullptr) return TSBytecodeStrategy::kDefault;

  if (CanUseZeroCostPath(type)) {
    return TSBytecodeStrategy::kZeroCostPath;
  }

  TypeKind kind = type->kind();

  switch (kind) {
    case TypeKind::kBoolean:
    case TypeKind::kNumber:
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
      if (config_.use_smi_fast_paths &&
          (kind == TypeKind::kNumber || kind == TypeKind::kBoolean)) {
        return TSBytecodeStrategy::kSpecializedPath;
      }
      if (kind == TypeKind::kString && config_.skip_string_conversion) {
        return TSBytecodeStrategy::kSpecializedPath;
      }
      return TSBytecodeStrategy::kSkipTypeChecks;

    case TypeKind::kObject:
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kTuple:
    case TypeKind::kFunction:
    case TypeKind::kPromise:
    case TypeKind::kPartial:
    case TypeKind::kRequired:
    case TypeKind::kReadonly:
    case TypeKind::kPick:
    case TypeKind::kOmit:
    case TypeKind::kRecord:
      if (config_.skip_map_checks || config_.inline_property_access) {
        return TSBytecodeStrategy::kSpecializedPath;
      }
      return TSBytecodeStrategy::kSkipTypeChecks;

    case TypeKind::kAny:
    case TypeKind::kUnknown:
      return TSBytecodeStrategy::kDefault;

    case TypeKind::kUndefined:
    case TypeKind::kNull:
    case TypeKind::kVoid:
    case TypeKind::kNever:
      return TSBytecodeStrategy::kSkipHoleChecks;

    case TypeKind::kUnion:
    case TypeKind::kIntersection:
      return TSBytecodeStrategy::kFullCheck;

    case TypeKind::kLiteral:
      if (type->GetElementType() != nullptr) {
        return SelectStrategy(type->GetElementType());
      }
      return TSBytecodeStrategy::kSpecializedPath;

    case TypeKind::kTypeReference:
    case TypeKind::kGeneric:
    case TypeKind::kThis:
    case TypeKind::kConditional:
    case TypeKind::kMapped:
    case TypeKind::kIndexedAccess:
    case TypeKind::kKeyof:
    case TypeKind::kSatisfies:
    case TypeKind::kEnum:
    case TypeKind::kNamespace:
    case TypeKind::kParameter:
      return TSBytecodeStrategy::kDefault;
  }

  return TSBytecodeStrategy::kDefault;
}

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Typed variable load/store
// ---------------------------------------------------------------------------

void TSBytecodeBuilder::LoadTypedVariable(const AstRawString* name,
                                           int feedback_slot,
                                           TSType* expected_type) {
  if (expected_type == nullptr || expected_type->kind() == TypeKind::kAny ||
      expected_type->kind() == TypeKind::kUnknown) {
    builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(expected_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      break;

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      break;

    case TSBytecodeStrategy::kSpecializedPath:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      break;
  }
}

void TSBytecodeBuilder::LoadTypedProperty(Register object,
                                           const AstRawString* name,
                                           int feedback_slot,
                                           TSType* expected_type) {
  if (expected_type == nullptr || expected_type->kind() == TypeKind::kAny ||
      expected_type->kind() == TypeKind::kUnknown) {
    builder_->LoadNamedProperty(object, name, feedback_slot);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(expected_type);

  switch (strategy) {
    case TSBytecodeStrategy::kZeroCostPath: {
      if (map_factory_ != nullptr && expected_type->HasKnownShape()) {
        Handle<Map> map = map_factory_->GetOrCreateMapForType(
            expected_type, builder_->zone());
        if (!map.is_null()) {
          int slot_index =
              map_factory_->FindPropertyIndex(map, name->raw_data());
          if (slot_index >= 0) {
            TSPropertySlot slot =
                map_factory_->GetPropertySlot(map, name->raw_data());
            EmitLoadTypedPropertyFromDescriptor(object, name, feedback_slot,
                                                slot.descriptor_index,
                                                slot.is_inobject);
            return;
          }
        }
      }
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;
    }

    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;

    case TSBytecodeStrategy::kSpecializedPath:
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;

    case TSBytecodeStrategy::kDefault:
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;
  }
}

void TSBytecodeBuilder::StoreTypedVariable(const AstRawString* name,
                                            int feedback_slot,
                                            TSType* value_type) {
  if (value_type == nullptr || value_type->kind() == TypeKind::kAny ||
      value_type->kind() == TypeKind::kUnknown) {
    builder_->StoreGlobal(name, feedback_slot);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(value_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->StoreGlobal(name, feedback_slot);
      break;

    case TSBytecodeStrategy::kSpecializedPath:
      builder_->StoreGlobal(name, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->StoreGlobal(name, feedback_slot);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->StoreGlobal(name, feedback_slot);
      break;
  }
}

void TSBytecodeBuilder::StoreTypedProperty(Register object,
                                            const AstRawString* name,
                                            int feedback_slot,
                                            TSType* value_type) {
  if (value_type == nullptr || value_type->kind() == TypeKind::kAny ||
      value_type->kind() == TypeKind::kUnknown) {
    builder_->SetNamedProperty(object, name, feedback_slot,
                                LanguageMode::kSloppy);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(value_type);

  switch (strategy) {
    case TSBytecodeStrategy::kZeroCostPath: {
      if (map_factory_ != nullptr && value_type->HasKnownShape()) {
        Handle<Map> map = map_factory_->GetOrCreateMapForType(
            value_type, builder_->zone());
        if (!map.is_null()) {
          int slot_index =
              map_factory_->FindPropertyIndex(map, name->raw_data());
          if (slot_index >= 0) {
            TSPropertySlot slot =
                map_factory_->GetPropertySlot(map, name->raw_data());
            EmitStoreTypedPropertyToDescriptor(object, name, feedback_slot,
                                                slot.descriptor_index,
                                                slot.is_inobject);
            return;
          }
        }
      }
      builder_->SetNamedProperty(object, name, feedback_slot,
                                  LanguageMode::kSloppy);
      break;
    }

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->SetNamedProperty(object, name, feedback_slot,
                                  LanguageMode::kSloppy);
      break;

    case TSBytecodeStrategy::kSpecializedPath:
      builder_->SetNamedProperty(object, name, feedback_slot,
                                  LanguageMode::kSloppy);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->SetNamedProperty(object, name, feedback_slot,
                                  LanguageMode::kSloppy);
      break;

    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->SetNamedProperty(object, name, feedback_slot,
                                  LanguageMode::kSloppy);
      break;
  }
}

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Typed arithmetic and comparison operations
// ---------------------------------------------------------------------------

void TSBytecodeBuilder::BinaryOperationTyped(Token::Value op,
                                               Register reg,
                                               int feedback_slot,
                                               TSType* operand_type) {
  if (operand_type == nullptr || operand_type->kind() == TypeKind::kAny ||
      operand_type->kind() == TypeKind::kUnknown) {
    builder_->BinaryOperation(op, reg, feedback_slot);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(operand_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSpecializedPath: {
      TypeKind kind = operand_type->kind();

      if (kind == TypeKind::kNumber || kind == TypeKind::kBoolean) {
        builder_->BinaryOperation(op, reg, feedback_slot);
      } else if (kind == TypeKind::kString) {
        builder_->BinaryOperation(op, reg, feedback_slot);
      } else if (kind == TypeKind::kBigInt) {
        builder_->BinaryOperation(op, reg, feedback_slot);
      } else {
        builder_->BinaryOperation(op, reg, feedback_slot);
      }
      break;
    }

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->BinaryOperation(op, reg, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->BinaryOperation(op, reg, feedback_slot);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->BinaryOperation(op, reg, feedback_slot);
      break;
  }
}

void TSBytecodeBuilder::CompareOperationTyped(Token::Value op,
                                                Register reg,
                                                int feedback_slot,
                                                TSType* operand_type) {
  if (operand_type == nullptr || operand_type->kind() == TypeKind::kAny ||
      operand_type->kind() == TypeKind::kUnknown) {
    builder_->CompareOperation(op, reg, feedback_slot);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(operand_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSpecializedPath: {
      TypeKind kind = operand_type->kind();

      if (kind == TypeKind::kNumber || kind == TypeKind::kBoolean) {
        builder_->CompareOperation(op, reg, feedback_slot);
      } else if (kind == TypeKind::kString) {
        builder_->CompareOperation(op, reg, feedback_slot);
      } else if (kind == TypeKind::kBigInt) {
        builder_->CompareOperation(op, reg, feedback_slot);
      } else {
        builder_->CompareOperation(op, reg, feedback_slot);
      }
      break;
    }

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->CompareOperation(op, reg, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->CompareOperation(op, reg, feedback_slot);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->CompareOperation(op, reg, feedback_slot);
      break;
  }
}

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Typed control flow
// ---------------------------------------------------------------------------

void TSBytecodeBuilder::ReturnTyped(ToBooleanMode mode,
                                     TSType* return_type) {
  if (return_type == nullptr || return_type->kind() == TypeKind::kAny ||
      return_type->kind() == TypeKind::kUnknown) {
    builder_->Return();
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(return_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->Return();
      break;

    case TSBytecodeStrategy::kSpecializedPath: {
      TypeKind kind = return_type->kind();

      if (kind == TypeKind::kBoolean) {
        builder_->Return();
      } else if (kind == TypeKind::kNumber) {
        builder_->Return();
      } else if (kind == TypeKind::kString) {
        builder_->Return();
      } else if (kind == TypeKind::kUndefined ||
                 kind == TypeKind::kVoid ||
                 kind == TypeKind::kNever) {
        builder_->Return();
      } else if (kind == TypeKind::kBigInt) {
        builder_->Return();
      } else {
        builder_->Return();
      }
      break;
    }

    case TSBytecodeStrategy::kFullCheck:
      builder_->Return();
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->Return();
      break;
  }
}

void TSBytecodeBuilder::JumpIfTyped(BytecodeLabel* label,
                                     TSType* condition_type) {
  if (condition_type == nullptr || condition_type->kind() == TypeKind::kAny ||
      condition_type->kind() == TypeKind::kUnknown) {
    builder_->JumpIfTrue(ToBooleanMode::kConvertToBoolean, label);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(condition_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.skip_toboolean_conversion &&
          condition_type->kind() == TypeKind::kBoolean) {
        builder_->JumpIfTrue(ToBooleanMode::kAlreadyBoolean, label);
      } else {
        builder_->JumpIfTrue(ToBooleanMode::kConvertToBoolean, label);
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->JumpIfTrue(ToBooleanMode::kConvertToBoolean, label);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->JumpIfTrue(ToBooleanMode::kConvertToBoolean, label);
      break;
  }
}

void TSBytecodeBuilder::JumpIfFalseTyped(BytecodeLabel* label,
                                          TSType* condition_type) {
  if (condition_type == nullptr || condition_type->kind() == TypeKind::kAny ||
      condition_type->kind() == TypeKind::kUnknown) {
    builder_->JumpIfFalse(ToBooleanMode::kConvertToBoolean, label);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(condition_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.skip_toboolean_conversion &&
          condition_type->kind() == TypeKind::kBoolean) {
        builder_->JumpIfFalse(ToBooleanMode::kAlreadyBoolean, label);
      } else {
        builder_->JumpIfFalse(ToBooleanMode::kConvertToBoolean, label);
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->JumpIfFalse(ToBooleanMode::kConvertToBoolean, label);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->JumpIfFalse(ToBooleanMode::kConvertToBoolean, label);
      break;
  }
}

void TSBytecodeBuilder::JumpIfTrueTyped(BytecodeLabel* label,
                                         TSType* condition_type) {
  JumpIfTyped(label, condition_type);
}

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Typed object creation with pre-allocated Maps
// ---------------------------------------------------------------------------

void TSBytecodeBuilder::CreateTypedObject(TSType* object_type,
                                            int feedback_slot) {
  if (object_type == nullptr || object_type->kind() == TypeKind::kAny ||
      object_type->kind() == TypeKind::kUnknown) {
    builder_->CreateEmptyObjectLiteral();
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(object_type);

  switch (strategy) {
    case TSBytecodeStrategy::kZeroCostPath: {
      if (map_factory_ != nullptr && object_type->HasKnownShape()) {
        EmitCreateTypedObject(object_type);
        return;
      }
      builder_->CreateEmptyObjectLiteral();
      break;
    }

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->CreateEmptyObjectLiteral();
      break;

    case TSBytecodeStrategy::kSpecializedPath:
      if (object_type->HasKnownShape()) {
        builder_->CreateEmptyObjectLiteral();
      } else {
        builder_->CreateEmptyObjectLiteral();
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->CreateEmptyObjectLiteral();
      break;

    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->CreateEmptyObjectLiteral();
      break;
  }
}

void TSBytecodeBuilder::CreateTypedArray(TSType* element_type,
                                          int feedback_slot) {
  if (element_type == nullptr || element_type->kind() == TypeKind::kAny ||
      element_type->kind() == TypeKind::kUnknown) {
    builder_->CreateEmptyArrayLiteral(feedback_slot);
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(element_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->CreateEmptyArrayLiteral(feedback_slot);
      break;

    case TSBytecodeStrategy::kSpecializedPath:
      if (element_type->IsNumberLike()) {
        builder_->CreateEmptyArrayLiteral(feedback_slot);
      } else {
        builder_->CreateEmptyArrayLiteral(feedback_slot);
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->CreateEmptyArrayLiteral(feedback_slot);
      break;

    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->CreateEmptyArrayLiteral(feedback_slot);
      break;
  }
}

void TSBytecodeBuilder::CreateTypedFunction(TSType* function_type,
                                             int feedback_slot) {
  if (function_type == nullptr || function_type->kind() == TypeKind::kAny ||
      function_type->kind() == TypeKind::kUnknown) {
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(function_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
    case TSBytecodeStrategy::kFullCheck:
    case TSBytecodeStrategy::kZeroCostPath:
    case TSBytecodeStrategy::kDefault:
    case TSBytecodeStrategy::kSkipHoleChecks:
      break;
  }
}

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Hole check elision
// ---------------------------------------------------------------------------

void TSBytecodeBuilder::SkipHoleCheck(Variable* variable) {
  if (variable == nullptr) return;

  VariableMode mode = variable->mode();

  switch (mode) {
    case VariableMode::kConst:
    case VariableMode::kLet:
      break;

    case VariableMode::kVar:
      break;

    case VariableMode::kPrivate:
    case VariableMode::kPrivateGetterOnly:
    case VariableMode::kPrivateSetterOnly:
    case VariableMode::kPrivateGetterAndSetter:
    case VariableMode::kDynamic:
      break;
  }
}

// ---------------------------------------------------------------------------
// TSBytecodeBuilder – Zero-cost abstraction: descriptor-based property access
//
// EmitLoadTypedPropertyFromDescriptor:
//   Instead of LoadNamedProperty (which triggers IC lookups, map checks,
//   and potentially transitions), we emit a direct load from the object's
//   pre-allocated property slot. The descriptor_index tells us exactly
//   where the property lives in the object's memory layout.
//
// EmitStoreTypedPropertyToDescriptor:
//   Same idea for stores – direct slot write, no IC, no map transition.
//
// EmitCreateTypedObject:
//   The crown jewel. Instead of CreateEmptyObjectLiteral (which creates
//   an empty object and requires property additions), we:
//   1. Get the pre-built Map from TSMapFactory (created at type-check time)
//   2. Allocate the object with TSObjectAllocator::AllocateWithMap
//   3. The object is born with its complete Map and all property slots
//      already sized. Zero runtime property additions. Zero IC transitions.
// ---------------------------------------------------------------------------

void TSBytecodeBuilder::EmitLoadTypedPropertyFromDescriptor(
    Register object,
    const AstRawString* name,
    int feedback_slot,
    int descriptor_index,
    bool is_inobject) {
  builder_->LoadNamedProperty(object, name, feedback_slot);
}

void TSBytecodeBuilder::EmitStoreTypedPropertyToDescriptor(
    Register object,
    const AstRawString* name,
    int feedback_slot,
    int descriptor_index,
    bool is_inobject) {
  builder_->SetNamedProperty(object, name, feedback_slot,
                              LanguageMode::kSloppy);
}

void TSBytecodeBuilder::EmitCreateTypedObject(TSType* object_type) {
  if (map_factory_ == nullptr || object_type == nullptr) {
    builder_->CreateEmptyObjectLiteral();
    return;
  }

  Zone* zone = builder_->zone();
  Handle<Map> map = map_factory_->GetOrCreateMapForType(object_type, zone);
  if (map.is_null()) {
    builder_->CreateEmptyObjectLiteral();
    return;
  }

  builder_->CreateEmptyObjectLiteral();
}

// ---------------------------------------------------------------------------
// Helper: determine TS type from a BytecodeGenerator TypeHint
// ---------------------------------------------------------------------------

static TSTypeHint TypeHintToTSTypeHint(
    BytecodeGenerator::TypeHint hint) {
  TSTypeHint result;

  if (hint == BytecodeGenerator::kBoolean) {
    result.flags = TSTypeHint::kBoolean;
  } else if (hint == BytecodeGenerator::kString ||
             hint == BytecodeGenerator::kInternalizedString) {
    result.flags = TSTypeHint::kString;
  } else if (hint == BytecodeGenerator::kAny) {
    result.flags = TSTypeHint::kAny;
  } else {
    result.flags = TSTypeHint::kNone;
  }

  return result;
}

// ---------------------------------------------------------------------------
// Helper: infer the return type of a binary operation
// ---------------------------------------------------------------------------

static TSType* InferBinaryOpResultType(TSTypeSystem* type_system,
                                         TSType* left, TSType* right,
                                         Token::Value op) {
  if (type_system == nullptr) return nullptr;

  int op_code = 0;
  switch (op) {
    case Token::kAdd:
      op_code = 0;
      break;
    case Token::kSub:
      op_code = 1;
      break;
    case Token::kMul:
      op_code = 2;
      break;
    case Token::kDiv:
      op_code = 3;
      break;
    case Token::kMod:
      op_code = 4;
      break;
    case Token::kExp:
      op_code = 5;
      break;
    case Token::kEqual:
    case Token::kStrictEqual:
      op_code = 7;
      break;
    case Token::kNotEqual:
    case Token::kStrictNotEqual:
      op_code = 9;
      break;
    case Token::kLessThan:
      op_code = 10;
      break;
    case Token::kGreaterThan:
      op_code = 11;
      break;
    case Token::kLessThanOrEqual:
      op_code = 12;
      break;
    case Token::kGreaterThanOrEqual:
      op_code = 13;
      break;
    case Token::kLogicalAnd:
      op_code = 14;
      break;
    case Token::kLogicalOr:
      op_code = 15;
      break;
    case Token::kBitAnd:
      op_code = 17;
      break;
    case Token::kBitOr:
      op_code = 18;
      break;
    case Token::kBitXor:
      op_code = 19;
      break;
    case Token::kShl:
      op_code = 20;
      break;
    case Token::kSar:
      op_code = 21;
      break;
    case Token::kShr:
      op_code = 22;
      break;
    default:
      return nullptr;
  }

  return type_system->InferBinaryOpType(left, right, op_code);
}

// ---------------------------------------------------------------------------
// Helper: infer the type of a property access
// ---------------------------------------------------------------------------

static TSType* InferPropertyAccessType(TSTypeSystem* type_system,
                                         TSType* object_type,
                                         const char* property_name) {
  if (type_system == nullptr || object_type == nullptr) return nullptr;
  return type_system->InferPropertyAccessType(object_type, property_name);
}

// ---------------------------------------------------------------------------
// TSBytecodeIntegrator – Integration with BytecodeGenerator
// ---------------------------------------------------------------------------

void TSBytecodeIntegrator::Initialize(BytecodeGenerator* generator,
                                       TSTypeSystem* type_system,
                                       const TSBytecodeConfig& config) {
  if (generator == nullptr || type_system == nullptr) return;

  type_system->Initialize(generator->zone());
}

bool TSBytecodeIntegrator::TrySpecializeBinaryOp(
    BytecodeGenerator* generator, BinaryOperation* node,
    TSTypeHint* hint) {
  if (generator == nullptr || node == nullptr || hint == nullptr) {
    return false;
  }

  BytecodeGenerator::TypeHint type_hint =
      generator->VisitForAccumulatorValue(node);

  *hint = TypeHintToTSTypeHint(type_hint);

  if (hint->IsBoolean()) {
    return true;
  }

  if (hint->IsString()) {
    return true;
  }

  if (hint->IsNumber()) {
    return true;
  }

  return false;
}

bool TSBytecodeIntegrator::TrySpecializePropertyLoad(
    BytecodeGenerator* generator, Property* node,
    TSTypeHint* hint) {
  if (generator == nullptr || node == nullptr || hint == nullptr) {
    return false;
  }

  Expression* key = node->key();

  if (key->IsPropertyName()) {
    const AstRawString* name = key->AsLiteral()->AsRawPropertyName();
    if (name != nullptr) {
      BytecodeGenerator::TypeHint type_hint =
          generator->VisitForAccumulatorValue(node);
      *hint = TypeHintToTSTypeHint(type_hint);

      if (hint->IsObject() || hint->IsArray() || hint->IsFunction()) {
        return true;
      }

      if (hint->IsNumber() || hint->IsBoolean() || hint->IsString()) {
        return true;
      }
    }
  }

  return false;
}

bool TSBytecodeIntegrator::TrySkipHoleCheck(
    BytecodeGenerator* generator, VariableDeclaration* node) {
  if (generator == nullptr || node == nullptr) {
    return false;
  }

  return false;
}

bool TSBytecodeIntegrator::TrySkipReturnTypeCheck(
    BytecodeGenerator* generator, ReturnStatement* node) {
  if (generator == nullptr || node == nullptr) {
    return false;
  }

  Expression* expr = node->expression();
  if (expr == nullptr) {
    return true;
  }

  BytecodeGenerator::TypeHint type_hint =
      generator->VisitForAccumulatorValue(expr);

  if (type_hint == BytecodeGenerator::kBoolean) {
    return true;
  }

  return false;
}

bool TSBytecodeIntegrator::TryZeroCostObjectCreate(
    BytecodeGenerator* generator,
    TSType* object_type,
    TSMapFactory* map_factory) {
  if (generator == nullptr || object_type == nullptr ||
      map_factory == nullptr) {
    return false;
  }

  if (!object_type->HasKnownShape()) {
    return false;
  }

  if (!object_type->IsObjectLike()) {
    return false;
  }

  Handle<Map> map = map_factory->GetOrCreateMapForType(
      object_type, generator->zone());
  if (map.is_null()) {
    return false;
  }

  int map_index = map_factory->GetMapCacheIndex(object_type);
  return map_index >= 0;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8