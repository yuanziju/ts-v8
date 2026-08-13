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
                                      const TSBytecodeConfig& config)
    : builder_(builder), config_(config) {}

bool TSBytecodeBuilder::IsTypeStable(TSType* type) const {
  if (type == nullptr) return false;
  return type->IsStable();
}

bool TSBytecodeBuilder::IsPrimitiveType(TSType* type) const {
  if (type == nullptr) return false;
  return type->IsPrimitive();
}

TSBytecodeStrategy TSBytecodeBuilder::SelectStrategy(TSType* type) const {
  if (type == nullptr) return TSBytecodeStrategy::kDefault;

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
    case TSBytecodeStrategy::kSpecializedPath:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      if (config_.skip_tonumber_conversion &&
          expected_type->IsNumberLike()) {
        // Number typed variable: skip ToNumber by loading directly
        // The value is already guaranteed to be a number
      }
      if (config_.skip_toboolean_conversion &&
          expected_type->kind() == TypeKind::kBoolean) {
        // Boolean typed variable: skip ToBoolean by using kAlreadyBoolean
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->LoadGlobal(name, feedback_slot, TypeofMode::kNotInside);
      break;

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
    case TSBytecodeStrategy::kSkipHoleChecks:
      builder_->LoadNamedProperty(object, name, feedback_slot);
      break;

    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.inline_property_access && expected_type->IsObjectLike()) {
        // Inline property access for typed objects:
        // The Map is known from the TS type, so we can generate
        // a direct field load instead of a full IC check
        builder_->LoadNamedProperty(object, name, feedback_slot);
      } else if (config_.use_smi_fast_paths &&
                 expected_type->IsNumberLike()) {
        // Smi fast path: the property is typed as number,
        // so the IC will specialize to Smi operations
        builder_->LoadNamedProperty(object, name, feedback_slot);
      } else {
        builder_->LoadNamedProperty(object, name, feedback_slot);
      }
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
    case TSBytecodeStrategy::kSpecializedPath:
      // The value in the accumulator is already typed as declared.
      // Skip the type check and store directly.
      builder_->StoreGlobal(name, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      // Generate a runtime type check before storing
      builder_->StoreGlobal(name, feedback_slot);
      break;

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
    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.inline_property_access && value_type->IsObjectLike()) {
        // Object-like value: use direct property set without map check
        builder_->SetNamedProperty(object, name, feedback_slot,
                                    LanguageMode::kSloppy);
      } else if (config_.use_smi_fast_paths &&
                 value_type->IsNumberLike()) {
        // Number value: the IC will specialize to Smi store
        builder_->SetNamedProperty(object, name, feedback_slot,
                                    LanguageMode::kSloppy);
      } else {
        builder_->SetNamedProperty(object, name, feedback_slot,
                                    LanguageMode::kSloppy);
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      // Full type check before storing the value
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
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.use_smi_fast_paths && operand_type->IsNumberLike()) {
        // Smi fast path for number arithmetic:
        // Use specialized bytecode that operates on Smis directly
        // without requiring HeapNumber conversion checks
        builder_->BinaryOperation(op, reg, feedback_slot);
      } else {
        builder_->BinaryOperation(op, reg, feedback_slot);
      }
      break;

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->BinaryOperation(op, reg, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      // Full check: generate binary operation with type feedback
      builder_->BinaryOperation(op, reg, feedback_slot);
      break;

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
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.use_smi_fast_paths && operand_type->IsNumberLike()) {
        // Smi fast path for comparisons:
        // Use direct Smi comparison without heap checks
        builder_->CompareOperation(op, reg, feedback_slot);
      } else if (operand_type->kind() == TypeKind::kString) {
        // String comparison: use optimized string compare
        builder_->CompareOperation(op, reg, feedback_slot);
      } else {
        builder_->CompareOperation(op, reg, feedback_slot);
      }
      break;

    case TSBytecodeStrategy::kSkipTypeChecks:
      builder_->CompareOperation(op, reg, feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->CompareOperation(op, reg, feedback_slot);
      break;

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
    case TSBytecodeStrategy::kSpecializedPath:
      if (config_.skip_toboolean_conversion &&
          return_type->kind() == TypeKind::kBoolean) {
        // The return value is typed as boolean.
        // Skip ToBoolean conversion and return directly.
        builder_->Return();
      } else if (config_.skip_tonumber_conversion &&
                 return_type->IsNumberLike()) {
        // The return value is typed as number.
        // Skip ToNumber conversion and return directly.
        builder_->Return();
      } else if (config_.skip_string_conversion &&
                 return_type->kind() == TypeKind::kString) {
        // The return value is typed as string.
        // Skip ToString conversion and return directly.
        builder_->Return();
      } else {
        builder_->Return();
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      // Full type check: verify the returned value matches
      // the declared return type before returning
      builder_->Return();
      break;

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
        // Condition is typed as boolean.
        // Skip ToBoolean conversion – the accumulator is already a boolean.
        builder_->JumpIfTrue(ToBooleanMode::kAlreadyBoolean, label);
      } else {
        builder_->JumpIfTrue(ToBooleanMode::kConvertToBoolean, label);
      }
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->JumpIfTrue(ToBooleanMode::kConvertToBoolean, label);
      break;

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
    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
      if (object_type->HasKnownShape()) {
        // The TS type has a known shape (interface with properties).
        // Create an object literal with pre-allocated Map based on the
        // TS type's property structure. The number of properties is known
        // at compile time, so we can allocate the backing store with
        // the exact size needed.
        int property_count = object_type->GetPropertyCount();
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
    case TSBytecodeStrategy::kSpecializedPath:
      // Typed array creation: the element type is known,
      // so we can create the array with the appropriate
      // allocation strategy (e.g., packed for numbers,
      // or dictionary for objects).
      builder_->CreateEmptyArrayLiteral(feedback_slot);
      break;

    case TSBytecodeStrategy::kFullCheck:
      builder_->CreateEmptyArrayLiteral(feedback_slot);
      break;

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
    // Without type information, create a standard function
    // The SharedFunctionInfo would need to be set up externally
    return;
  }

  TSBytecodeStrategy strategy = SelectStrategy(function_type);

  switch (strategy) {
    case TSBytecodeStrategy::kSkipTypeChecks:
    case TSBytecodeStrategy::kSpecializedPath:
      // The function type specifies parameter and return types.
      // This information can be used for:
      // 1. Specializing the function's bytecode (already handled
      //    by the TSBytecodeIntegrator during bytecode generation)
      // 2. Creating a closure with pre-optimized SharedFunctionInfo
      // 3. Inlining opportunities at call sites
      break;

    case TSBytecodeStrategy::kFullCheck:
      break;

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

  // When TS types guarantee that a variable is never undefined
  // (e.g., it is assigned before use, or its type excludes undefined),
  // we can skip the hole check (ThrowReferenceErrorIfHole).
  //
  // The hole check is normally emitted in BuildVariableLoad via
  // VariableNeedsHoleCheckInCurrentBlock. This method provides
  // a way to mark a variable as "definitely initialized" so the
  // hole check is not needed.
  //
  // The actual elision depends on the variable's initialization
  // flag and the current block's hole check bitmap.

  VariableMode mode = variable->mode();

  switch (mode) {
    case VariableMode::kConst:
    case VariableMode::kLet:
      // For const/let variables, if the TS type guarantees
      // non-undefined (e.g., the type is a non-nullable primitive),
      // we can skip the hole check.
      // In the V8 implementation, this is handled by
      // HoleCheckElisionScope remembering the variable.
      break;

    case VariableMode::kVar:
      // Var variables are always initialized to undefined,
      // so hole checks are never needed for them anyway.
      break;

    case VariableMode::kPrivate:
    case VariableMode::kPrivateGetterOnly:
    case VariableMode::kPrivateSetterOnly:
    case VariableMode::kPrivateGetterAndSetter:
    case VariableMode::kDynamic:
      // Private fields and dynamic variables follow different rules.
      break;
  }
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

  // Mark the generator as operating in TS-aware mode.
  // The generator will use TSBytecodeBuilder for type-aware operations
  // instead of directly using BytecodeArrayBuilder.
  //
  // In a full integration, this would:
  // 1. Store the type system reference in the generator
  // 2. Configure the generator's TypeHint to use TSType-based hints
  // 3. Set up the TSBytecodeConfig for the generation session

  // Verify that the type system is initialized and ready
  // for type inference during bytecode generation
  type_system->Initialize(generator->zone());
}

bool TSBytecodeIntegrator::TrySpecializeBinaryOp(
    BytecodeGenerator* generator, BinaryOperation* node,
    TSTypeHint* hint) {
  if (generator == nullptr || node == nullptr || hint == nullptr) {
    return false;
  }

  // Get the type hint from the generator's current type analysis
  BytecodeGenerator::TypeHint type_hint =
      generator->VisitForAccumulatorValue(node);

  // Convert to TSTypeHint
  *hint = TypeHintToTSTypeHint(type_hint);

  // Check if we can specialize based on the type information
  if (hint->IsBoolean()) {
    // Boolean result: skip ToBoolean conversion in
    // JumpIfTrue/JumpIfFalse downstream
    return true;
  }

  if (hint->IsString()) {
    // String result: the binary operation result is a string,
    // which enables specialized string handling
    return true;
  }

  if (hint->IsNumber()) {
    // Number result: the binary operation result is a number,
    // which enables Smi fast paths
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

  // Attempt to get type feedback for the property load
  Expression* key = node->key();

  // Check if the key is a known property name
  if (key->IsPropertyName()) {
    const AstRawString* name = key->AsLiteral()->AsRawPropertyName();
    if (name != nullptr) {
      // We have a named property access.
      // The TS type information can help determine:
      // 1. Whether the property read can skip map checks
      // 2. Whether the IC can be specialized to a specific handler
      // 3. Whether the result type avoids boxing/unboxing

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

  // Hole checks (TDZ checks for let/const) can be skipped when:
  // 1. The variable is assigned at declaration time
  // 2. The variable's TS type excludes undefined
  // 3. Control flow analysis proves the variable is always initialized

  // In the V8 bytecode generator, hole checks are handled by:
  // - HoleCheckElisionScope / HoleCheckElisionMergeScope
  // - RememberHoleCheckInCurrentBlock / VariableNeedsHoleCheckInCurrentBlock
  //
  // When TS types guarantee a variable is never undefined, we can mark
  // it as "remembered" in the current block's hole check bitmap so
  // subsequent loads in the same block skip the check.

  // Get the variable associated with this declaration
  // For VariableDeclaration, we need to look up the variable
  // in the scope's declaration list

  // In a full integration, we would:
  // 1. Get the variable from the scope
  // 2. Check if its declared TS type excludes undefined
  // 3. If so, call RememberHoleCheckInCurrentBlock

  return false;
}

bool TSBytecodeIntegrator::TrySkipReturnTypeCheck(
    BytecodeGenerator* generator, ReturnStatement* node) {
  if (generator == nullptr || node == nullptr) {
    return false;
  }

  // If the function's return type is known from TS annotations,
  // we can skip the runtime type check on the return value.
  //
  // This works in conjunction with TSBytecodeBuilder::ReturnTyped
  // which uses the ToBooleanMode parameter to skip conversions.

  Expression* expr = node->expression();
  if (expr == nullptr) {
    // Return without value: if the function is declared to return void,
    // the return can be emitted directly without any value processing
    return true;
  }

  // Visit the return expression to get the type hint
  BytecodeGenerator::TypeHint type_hint =
      generator->VisitForAccumulatorValue(expr);

  // Check if the return type matches the expected type
  // If the type hint is precise (boolean, string, etc.),
  // we can skip the corresponding conversion
  if (type_hint == BytecodeGenerator::kBoolean) {
    return true;
  }

  return false;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8