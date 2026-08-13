// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ts/ts-type-system.h"

#include "src/base/logging.h"

namespace v8 {
namespace internal {
namespace ts {

// ---------------------------------------------------------------------------
// Helper: check whether a TypeKind represents a primitive type
// ---------------------------------------------------------------------------
static bool IsPrimitiveKind(TypeKind kind) {
  switch (kind) {
    case TypeKind::kBoolean:
    case TypeKind::kNumber:
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
    case TypeKind::kUndefined:
    case TypeKind::kNull:
    case TypeKind::kVoid:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Helper: check whether a TypeKind represents an object-like type
// ---------------------------------------------------------------------------
static bool IsObjectLikeKind(TypeKind kind) {
  switch (kind) {
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
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Helper: check whether a TypeKind represents a function-like type
// ---------------------------------------------------------------------------
static bool IsFunctionLikeKind(TypeKind kind) {
  return kind == TypeKind::kFunction;
}

// ---------------------------------------------------------------------------
// Helper: check whether a TypeKind represents a numeric type
// ---------------------------------------------------------------------------
static bool IsNumericKind(TypeKind kind) {
  return kind == TypeKind::kNumber || kind == TypeKind::kBoolean ||
         kind == TypeKind::kBigInt;
}

// ---------------------------------------------------------------------------
// Helper: check whether a TypeKind represents a string-like type
// ---------------------------------------------------------------------------
static bool IsStringLikeKind(TypeKind kind) {
  return kind == TypeKind::kString;
}

// ---------------------------------------------------------------------------
// TSType – Primitive factories
// ---------------------------------------------------------------------------

TSType* TSType::Any(Zone* zone) {
  return zone->New<TSType>(TypeKind::kAny);
}

TSType* TSType::Unknown(Zone* zone) {
  return zone->New<TSType>(TypeKind::kUnknown);
}

TSType* TSType::Never(Zone* zone) {
  return zone->New<TSType>(TypeKind::kNever);
}

TSType* TSType::Boolean(Zone* zone) {
  return zone->New<TSType>(TypeKind::kBoolean);
}

TSType* TSType::Number(Zone* zone) {
  return zone->New<TSType>(TypeKind::kNumber);
}

TSType* TSType::String(Zone* zone) {
  return zone->New<TSType>(TypeKind::kString);
}

TSType* TSType::Symbol(Zone* zone) {
  return zone->New<TSType>(TypeKind::kSymbol);
}

TSType* TSType::BigInt(Zone* zone) {
  return zone->New<TSType>(TypeKind::kBigInt);
}

TSType* TSType::Undefined(Zone* zone) {
  return zone->New<TSType>(TypeKind::kUndefined);
}

TSType* TSType::Null(Zone* zone) {
  return zone->New<TSType>(TypeKind::kNull);
}

TSType* TSType::Void(Zone* zone) {
  return zone->New<TSType>(TypeKind::kVoid);
}

TSType* TSType::Object(Zone* zone) {
  return zone->New<TSType>(TypeKind::kObject);
}

// ---------------------------------------------------------------------------
// TSType – Complex factories
// ---------------------------------------------------------------------------

TSType* TSType::CreateInterface(Zone* zone, const char* name,
                                 ZoneList<PropertyDescriptor>* properties,
                                 ZoneList<TypeParameter>* type_params) {
  TSType* type = zone->New<TSType>(TypeKind::kInterface);
  type->name_ = name;
  type->properties_ = properties;
  type->type_params_ = type_params;
  return type;
}

TSType* TSType::CreateArray(Zone* zone, TSType* element_type) {
  TSType* type = zone->New<TSType>(TypeKind::kArray);
  type->element_type_ = element_type;
  return type;
}

TSType* TSType::CreateTuple(Zone* zone, ZoneList<TSType*>* element_types) {
  TSType* type = zone->New<TSType>(TypeKind::kTuple);
  type->union_members_ = element_types;
  return type;
}

TSType* TSType::CreateFunction(Zone* zone, ZoneList<TSType*>* param_types,
                                TSType* return_type, bool is_async) {
  TSType* type = zone->New<TSType>(TypeKind::kFunction);
  type->param_types_ = param_types;
  type->return_type_ = return_type;
  type->is_readonly_ = is_async;
  return type;
}

TSType* TSType::CreateUnion(Zone* zone, TSType* left, TSType* right) {
  TSType* type = zone->New<TSType>(TypeKind::kUnion);
  type->union_right_ = right;

  // Flatten union members into a ZoneList
  // If left is itself a union, merge its members; right is a single member.
  int capacity = 2;
  if (left->IsUnion()) {
    capacity = left->GetUnionMembers()->length() + 1;
  }
  ZoneList<TSType*>* members = zone->New<ZoneList<TSType*>>(capacity, zone);
  if (left->IsUnion()) {
    ZoneList<TSType*>* left_members = left->GetUnionMembers();
    for (int i = 0; i < left_members->length(); i++) {
      members->Add(left_members->at(i), zone);
    }
  } else {
    members->Add(left, zone);
  }
  members->Add(right, zone);
  type->union_members_ = members;
  return type;
}

TSType* TSType::CreateIntersection(Zone* zone, TSType* left, TSType* right) {
  TSType* type = zone->New<TSType>(TypeKind::kIntersection);
  type->union_right_ = right;

  // Flatten intersection members similarly to union
  int capacity = 2;
  if (left->kind() == TypeKind::kIntersection) {
    capacity = left->union_members_->length() + 1;
  }
  ZoneList<TSType*>* members = zone->New<ZoneList<TSType*>>(capacity, zone);
  if (left->kind() == TypeKind::kIntersection) {
    for (int i = 0; i < left->union_members_->length(); i++) {
      members->Add(left->union_members_->at(i), zone);
    }
  } else {
    members->Add(left, zone);
  }
  members->Add(right, zone);
  type->union_members_ = members;
  return type;
}

TSType* TSType::CreateLiteral(Zone* zone, const char* value,
                                TSType* base_type) {
  TSType* type = zone->New<TSType>(TypeKind::kLiteral);
  type->literal_value_ = value;
  type->element_type_ = base_type;
  return type;
}

TSType* TSType::CreatePromise(Zone* zone, TSType* value_type) {
  TSType* type = zone->New<TSType>(TypeKind::kPromise);
  type->element_type_ = value_type;
  return type;
}

TSType* TSType::CreateGeneric(Zone* zone, TypeParameter* param) {
  TSType* type = zone->New<TSType>(TypeKind::kGeneric);
  type->name_ = param->name;
  type->type_params_ = zone->New<ZoneList<TypeParameter>>(1, zone);
  type->type_params_->Add(*param, zone);
  return type;
}

// ---------------------------------------------------------------------------
// TSType – Type queries
// ---------------------------------------------------------------------------

bool TSType::IsPrimitive() const { return IsPrimitiveKind(kind_); }

bool TSType::IsObjectLike() const { return IsObjectLikeKind(kind_); }

bool TSType::IsFunctionLike() const { return IsFunctionLikeKind(kind_); }

bool TSType::IsUnion() const { return kind_ == TypeKind::kUnion; }

bool TSType::IsLiteral() const { return kind_ == TypeKind::kLiteral; }

bool TSType::IsNullable() const {
  return kind_ == TypeKind::kNull || kind_ == TypeKind::kUndefined;
}

bool TSType::IsStringLike() const {
  return kind_ == TypeKind::kString ||
         (kind_ == TypeKind::kLiteral && element_type_ != nullptr &&
          element_type_->kind() == TypeKind::kString);
}

bool TSType::IsNumberLike() const {
  return kind_ == TypeKind::kNumber || kind_ == TypeKind::kBoolean ||
         (kind_ == TypeKind::kLiteral && element_type_ != nullptr &&
          (element_type_->kind() == TypeKind::kNumber ||
           element_type_->kind() == TypeKind::kBoolean));
}

// ---------------------------------------------------------------------------
// TSType – Getters
// ---------------------------------------------------------------------------

const char* TSType::GetName() const { return name_; }

ZoneList<PropertyDescriptor>* TSType::GetProperties() const {
  return properties_;
}

TSType* TSType::GetElementType() const { return element_type_; }

ZoneList<TSType*>* TSType::GetUnionMembers() const {
  return union_members_;
}

TSType* TSType::GetReturnType() const { return return_type_; }

ZoneList<TSType*>* TSType::GetParamTypes() const { return param_types_; }

// ---------------------------------------------------------------------------
// TSType – Type comparison (core logic)
// ---------------------------------------------------------------------------

bool TSType::IsSubtypeOf(const TSType* other) const {
  // Reflexivity: a type is always a subtype of itself
  if (this == other) return true;

  // never is the bottom type – subtype of everything except itself
  if (kind_ == TypeKind::kNever) return true;

  // any is the top type – everything is a subtype of any
  if (other->kind_ == TypeKind::kAny) return true;

  // If this is any, it is only a subtype of any (handled above)
  if (kind_ == TypeKind::kAny) return false;

  // unknown: everything is a subtype of unknown except any (handled above)
  if (other->kind_ == TypeKind::kUnknown) return true;

  // If this is unknown, it's only a subtype of unknown and any
  if (kind_ == TypeKind::kUnknown) return false;

  // Literal types are subtypes of their base type
  if (kind_ == TypeKind::kLiteral && element_type_ != nullptr) {
    return element_type_->IsSubtypeOf(other);
  }

  // undefined is a subtype of void
  if (kind_ == TypeKind::kUndefined && other->kind() == TypeKind::kVoid) {
    return true;
  }

  // A union type A | B is a subtype of C if both A and B are subtypes of C
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsSubtypeOf(other)) return false;
    }
    return true;
  }

  // A non-union type C is a subtype of A | B if C is a subtype of A or B
  if (other->kind_ == TypeKind::kUnion && other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (IsSubtypeOf(other->union_members_->at(i))) return true;
    }
    return false;
  }

  // Intersection type: A & B is subtype of C if A is subtype of C or B is
  if (kind_ == TypeKind::kIntersection && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (union_members_->at(i)->IsSubtypeOf(other)) return true;
    }
    return false;
  }

  // C is subtype of A & B if C is subtype of both A and B
  if (other->kind_ == TypeKind::kIntersection &&
      other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (!IsSubtypeOf(other->union_members_->at(i))) return false;
    }
    return true;
  }

  // Primitive subtyping: boolean -> number (boolean is a numeric type)
  if (kind_ == TypeKind::kBoolean && other->kind() == TypeKind::kNumber) {
    return true;
  }

  // String literal is subtype of string
  if (kind_ == TypeKind::kLiteral && other->kind() == TypeKind::kString &&
      element_type_ != nullptr && element_type_->kind() == TypeKind::kString) {
    return true;
  }

  // Number literal is subtype of number
  if (kind_ == TypeKind::kLiteral && other->kind() == TypeKind::kNumber &&
      element_type_ != nullptr &&
      (element_type_->kind() == TypeKind::kNumber ||
       element_type_->kind() == TypeKind::kBoolean)) {
    return true;
  }

  // Boolean literal is subtype of boolean
  if (kind_ == TypeKind::kLiteral && other->kind() == TypeKind::kBoolean &&
      element_type_ != nullptr && element_type_->kind() == TypeKind::kBoolean) {
    return true;
  }

  // Array subtyping: covariant element types
  if (kind_ == TypeKind::kArray && other->kind() == TypeKind::kArray) {
    if (element_type_ == nullptr && other->element_type_ == nullptr) return true;
    if (element_type_ == nullptr) return true;
    if (other->element_type_ == nullptr) return false;
    return element_type_->IsSubtypeOf(other->element_type_);
  }

  // Array is a subtype of object
  if (kind_ == TypeKind::kArray && other->kind() == TypeKind::kObject) {
    return true;
  }

  // Tuple subtyping: covariant element types, matching length
  if (kind_ == TypeKind::kTuple && other->kind() == TypeKind::kTuple) {
    if (union_members_ == nullptr || other->union_members_ == nullptr) {
      return union_members_ == other->union_members_;
    }
    if (union_members_->length() != other->union_members_->length()) {
      return false;
    }
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsSubtypeOf(other->union_members_->at(i))) {
        return false;
      }
    }
    return true;
  }

  // Tuple is a subtype of array
  if (kind_ == TypeKind::kTuple && other->kind() == TypeKind::kArray) {
    if (union_members_ == nullptr) return true;
    if (other->element_type_ == nullptr) return true;
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsSubtypeOf(other->element_type_)) {
        return false;
      }
    }
    return true;
  }

  // Function subtyping: parameter contravariance, return covariance
  if (kind_ == TypeKind::kFunction && other->kind() == TypeKind::kFunction) {
    // Check return type covariance
    if (return_type_ != nullptr && other->return_type_ != nullptr) {
      if (!return_type_->IsSubtypeOf(other->return_type_)) return false;
    }
    // Check parameter contravariance
    if (param_types_ != nullptr && other->param_types_ != nullptr) {
      if (param_types_->length() != other->param_types_->length()) return false;
      for (int i = 0; i < param_types_->length(); i++) {
        if (!other->param_types_->at(i)->IsSubtypeOf(param_types_->at(i))) {
          return false;
        }
      }
    }
    return true;
  }

  // Function is a subtype of object
  if (kind_ == TypeKind::kFunction && other->kind() == TypeKind::kObject) {
    return true;
  }

  // Structural subtyping for interface/object types
  if (IsObjectLike() && other->IsObjectLike()) {
    // Check that all required properties of 'other' are present in 'this'
    // with compatible types
    ZoneList<PropertyDescriptor>* other_props = other->GetProperties();
    if (other_props != nullptr && properties_ != nullptr) {
      for (int i = 0; i < other_props->length(); i++) {
        const PropertyDescriptor& other_prop = other_props->at(i);
        bool found = false;
        for (int j = 0; j < properties_->length(); j++) {
          const PropertyDescriptor& this_prop = properties_->at(j);
          if (strcmp(this_prop.name, other_prop.name) == 0) {
            // Property must be present (other's required prop must exist)
            if (!other_prop.is_optional && this_prop.is_optional) {
              return false;
            }
            // Check type compatibility
            if (other_prop.type != nullptr && this_prop.type != nullptr) {
              if (!this_prop.type->IsSubtypeOf(other_prop.type)) {
                return false;
              }
            }
            // Readonly: if target is not readonly, source must not be readonly
            if (!other_prop.is_readonly && this_prop.is_readonly) {
              return false;
            }
            found = true;
            break;
          }
        }
        if (!found && !other_prop.is_optional) {
          return false;
        }
      }
      return true;
    }
    // If one has no properties, they're compatible only if both are object-like
    // (handled at a higher level)
    return other_props == nullptr;
  }

  // Promise subtyping
  if (kind_ == TypeKind::kPromise && other->kind() == TypeKind::kPromise) {
    if (element_type_ == nullptr || other->element_type_ == nullptr) {
      return true;
    }
    return element_type_->IsSubtypeOf(other->element_type_);
  }

  // Promise is a subtype of object
  if (kind_ == TypeKind::kPromise && other->kind() == TypeKind::kObject) {
    return true;
  }

  // Partial<T> is a subtype of object
  if (kind_ == TypeKind::kPartial || kind_ == TypeKind::kRequired ||
      kind_ == TypeKind::kReadonly || kind_ == TypeKind::kPick ||
      kind_ == TypeKind::kOmit || kind_ == TypeKind::kRecord) {
    if (other->kind() == TypeKind::kObject) return true;
  }

  // A type is a subtype of itself (handled at the top via pointer equality,
  // but also by kind for singletons)
  if (kind_ == other->kind_) {
    // For literal types, check value equality
    if (kind_ == TypeKind::kLiteral && literal_value_ != nullptr &&
        other->literal_value_ != nullptr) {
      return strcmp(literal_value_, other->literal_value_) == 0;
    }
    return true;
  }

  return false;
}

bool TSType::IsAssignableTo(const TSType* other) const {
  // any is assignable to everything
  if (kind_ == TypeKind::kAny) return true;

  // Everything is assignable to any
  if (other->kind_ == TypeKind::kAny) return true;

  // Use the subtype check as a base
  if (IsSubtypeOf(other)) return true;

  // Assignability is more permissive than subtyping in some cases:
  // - unknown is assignable to any (already handled above)
  // - null is assignable to nullable types
  // - undefined is assignable to void

  // null is assignable to everything except non-nullable primitives
  if (kind_ == TypeKind::kNull) {
    switch (other->kind_) {
      case TypeKind::kBoolean:
      case TypeKind::kNumber:
      case TypeKind::kString:
      case TypeKind::kSymbol:
      case TypeKind::kBigInt:
      case TypeKind::kVoid:
        return false;
      default:
        return true;
    }
  }

  // undefined is assignable to void
  if (kind_ == TypeKind::kUndefined && other->kind() == TypeKind::kVoid) {
    return true;
  }

  // For unions, check if any member is assignable
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (union_members_->at(i)->IsAssignableTo(other)) return true;
    }
    return false;
  }

  // A single type is assignable to a union if it's assignable to any member
  if (other->kind_ == TypeKind::kUnion && other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (IsAssignableTo(other->union_members_->at(i))) return true;
    }
    return false;
  }

  return false;
}

bool TSType::IsIdenticalTo(const TSType* other) const {
  if (this == other) return true;
  if (kind_ != other->kind_) return false;

  switch (kind_) {
    case TypeKind::kAny:
    case TypeKind::kUnknown:
    case TypeKind::kNever:
    case TypeKind::kBoolean:
    case TypeKind::kNumber:
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
    case TypeKind::kUndefined:
    case TypeKind::kNull:
    case TypeKind::kVoid:
    case TypeKind::kObject:
      return true;

    case TypeKind::kLiteral:
      if (literal_value_ == nullptr && other->literal_value_ == nullptr)
        return true;
      if (literal_value_ == nullptr || other->literal_value_ == nullptr)
        return false;
      if (strcmp(literal_value_, other->literal_value_) != 0) return false;
      // Fall through to check base type
      break;

    default:
      break;
  }

  // Check name for named types
  if (name_ != nullptr || other->name_ != nullptr) {
    if (name_ == nullptr || other->name_ == nullptr) return false;
    if (strcmp(name_, other->name_) != 0) return false;
  }

  // Check element type for array/promise types
  if (element_type_ != nullptr || other->element_type_ != nullptr) {
    if (element_type_ == nullptr || other->element_type_ == nullptr) return false;
    if (!element_type_->IsIdenticalTo(other->element_type_)) return false;
  }

  // Check return type for function types
  if (return_type_ != nullptr || other->return_type_ != nullptr) {
    if (return_type_ == nullptr || other->return_type_ == nullptr) return false;
    if (!return_type_->IsIdenticalTo(other->return_type_)) return false;
  }

  // Check param types for function types
  if (param_types_ != nullptr || other->param_types_ != nullptr) {
    if (param_types_ == nullptr || other->param_types_ == nullptr) return false;
    if (param_types_->length() != other->param_types_->length()) return false;
    for (int i = 0; i < param_types_->length(); i++) {
      if (!param_types_->at(i)->IsIdenticalTo(other->param_types_->at(i))) {
        return false;
      }
    }
  }

  // Check union/intersection members
  if (union_members_ != nullptr || other->union_members_ != nullptr) {
    if (union_members_ == nullptr || other->union_members_ == nullptr)
      return false;
    if (union_members_->length() != other->union_members_->length())
      return false;
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsIdenticalTo(
              other->union_members_->at(i))) {
        return false;
      }
    }
  }

  // Check properties for interface/object types
  if (properties_ != nullptr || other->properties_ != nullptr) {
    if (properties_ == nullptr || other->properties_ == nullptr) return false;
    if (properties_->length() != other->properties_->length()) return false;
    for (int i = 0; i < properties_->length(); i++) {
      const PropertyDescriptor& a = properties_->at(i);
      const PropertyDescriptor& b = other->properties_->at(i);
      if (strcmp(a.name, b.name) != 0) return false;
      if (a.is_readonly != b.is_readonly) return false;
      if (a.is_optional != b.is_optional) return false;
      if (a.type != nullptr && b.type != nullptr) {
        if (!a.type->IsIdenticalTo(b.type)) return false;
      } else if (a.type != b.type) {
        return false;
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// TSType – V8 bridge
// ---------------------------------------------------------------------------

void* TSType::ToV8Type(Zone* zone) const {
  // This bridge converts TS types to V8's internal compiler::Type
  // representation. In this initial implementation we return nullptr
  // to indicate that the conversion has not been fully wired up yet.
  // When integrated with TurboFan, this method will create the
  // corresponding compiler::Type object.

  switch (kind_) {
    case TypeKind::kAny:
    case TypeKind::kUnknown:
      // These map to V8's Any type
      return nullptr;

    case TypeKind::kNever:
      return nullptr;

    case TypeKind::kBoolean:
      return nullptr;

    case TypeKind::kNumber:
      return nullptr;

    case TypeKind::kString:
      return nullptr;

    case TypeKind::kSymbol:
      return nullptr;

    case TypeKind::kBigInt:
      return nullptr;

    case TypeKind::kUndefined:
      return nullptr;

    case TypeKind::kNull:
      return nullptr;

    case TypeKind::kVoid:
      return nullptr;

    case TypeKind::kObject:
      return nullptr;

    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kTuple:
    case TypeKind::kFunction:
    case TypeKind::kPromise:
      return nullptr;

    case TypeKind::kUnion:
    case TypeKind::kIntersection:
      return nullptr;

    case TypeKind::kLiteral:
      if (element_type_ != nullptr) {
        return element_type_->ToV8Type(zone);
      }
      return nullptr;

    default:
      return nullptr;
  }
}

// ---------------------------------------------------------------------------
// TSType – Shape hints for Map creation
// ---------------------------------------------------------------------------

bool TSType::HasKnownShape() const {
  // Interface types with properties have a known shape
  if (kind_ == TypeKind::kInterface && properties_ != nullptr) {
    return true;
  }
  // Object types with properties also have a known shape
  if (kind_ == TypeKind::kObject && properties_ != nullptr) {
    return true;
  }
  return false;
}

int TSType::GetPropertyCount() const {
  if (properties_ != nullptr) {
    return properties_->length();
  }
  return 0;
}

bool TSType::IsStable() const {
  // Basic types are stable (they don't change at runtime)
  if (IsPrimitive() || kind_ == TypeKind::kAny || kind_ == TypeKind::kUnknown ||
      kind_ == TypeKind::kNever) {
    return true;
  }

  // Literal types are stable if their base is stable
  if (kind_ == TypeKind::kLiteral && element_type_ != nullptr) {
    return element_type_->IsStable();
  }

  // Array types are stable if their element type is stable
  if (kind_ == TypeKind::kArray && element_type_ != nullptr) {
    return element_type_->IsStable();
  }

  // Promise types are stable if their value type is stable
  if (kind_ == TypeKind::kPromise && element_type_ != nullptr) {
    return element_type_->IsStable();
  }

  // Union types are stable if all members are stable
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsStable()) return false;
    }
    return true;
  }

  // Function types are stable if param and return types are stable
  if (kind_ == TypeKind::kFunction) {
    if (return_type_ != nullptr && !return_type_->IsStable()) return false;
    if (param_types_ != nullptr) {
      for (int i = 0; i < param_types_->length(); i++) {
        if (!param_types_->at(i)->IsStable()) return false;
      }
    }
    return true;
  }

  // Interface types are stable if their properties are stable
  if (kind_ == TypeKind::kInterface && properties_ != nullptr) {
    for (int i = 0; i < properties_->length(); i++) {
      if (properties_->at(i).type != nullptr &&
          !properties_->at(i).type->IsStable()) {
        return false;
      }
    }
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSTypeSystem
// ---------------------------------------------------------------------------

TSTypeSystem::TSTypeSystem(Zone* zone)
    : zone_(zone),
      registered_types_(zone),
      registered_names_(zone) {}

void TSTypeSystem::RegisterInterface(
    const char* name, ZoneList<PropertyDescriptor>* properties,
    ZoneList<TypeParameter>* type_params) {
  TSType* type = TSType::CreateInterface(zone_, name, properties, type_params);
  registered_types_.Add(type, zone_);
  registered_names_.Add(name, zone_);
}

void TSTypeSystem::RegisterClass(
    const char* name, ZoneList<PropertyDescriptor>* properties,
    ZoneList<TypeParameter>* type_params) {
  TSType* type = TSType::CreateInterface(zone_, name, properties, type_params);
  registered_types_.Add(type, zone_);
  registered_names_.Add(name, zone_);
}

void TSTypeSystem::RegisterEnum(const char* name,
                                 ZoneList<const char*>* members) {
  DCHECK_NOT_NULL(members);

  ZoneList<PropertyDescriptor>* properties =
      zone_->New<ZoneList<PropertyDescriptor>>(members->length(), zone_);

  // Create a string type base for enum members
  TSType* string_type = TSType::String(zone_);

  for (int i = 0; i < members->length(); i++) {
    PropertyDescriptor prop;
    prop.name = members->at(i);
    prop.type = string_type;
    prop.is_readonly = true;
    prop.is_optional = false;
    prop.is_public = true;
    prop.is_private = false;
    prop.is_protected = false;
    prop.has_readonly_modifier = true;
    properties->Add(prop, zone_);
  }

  TSType* type = TSType::CreateInterface(zone_, name, properties, nullptr);
  registered_types_.Add(type, zone_);
  registered_names_.Add(name, zone_);
}

TSType* TSTypeSystem::LookupType(const char* name) const {
  for (int i = 0; i < registered_names_.length(); i++) {
    if (strcmp(registered_names_.at(i), name) == 0) {
      return registered_types_.at(i);
    }
  }
  return nullptr;
}

TSType* TSTypeSystem::CreateAndCache(Zone* zone, TypeKind kind) {
  // Return cached singletons for primitive types
  switch (kind) {
    case TypeKind::kAny:
      if (cached_any_ == nullptr) {
        cached_any_ = TSType::Any(zone);
      }
      return cached_any_;

    case TypeKind::kNever:
      if (cached_never_ == nullptr) {
        cached_never_ = TSType::Never(zone);
      }
      return cached_never_;

    case TypeKind::kBoolean:
      if (cached_boolean_ == nullptr) {
        cached_boolean_ = TSType::Boolean(zone);
      }
      return cached_boolean_;

    case TypeKind::kNumber:
      if (cached_number_ == nullptr) {
        cached_number_ = TSType::Number(zone);
      }
      return cached_number_;

    case TypeKind::kString:
      if (cached_string_ == nullptr) {
        cached_string_ = TSType::String(zone);
      }
      return cached_string_;

    case TypeKind::kUndefined:
      if (cached_undefined_ == nullptr) {
        cached_undefined_ = TSType::Undefined(zone);
      }
      return cached_undefined_;

    case TypeKind::kNull:
      if (cached_null_ == nullptr) {
        cached_null_ = TSType::Null(zone);
      }
      return cached_null_;

    default:
      // For non-cached kinds, create a new instance each time
      return zone->New<TSType>(kind);
  }
}

TSType* TSTypeSystem::InferBinaryOpType(TSType* left, TSType* right,
                                         int op) {
  DCHECK_NOT_NULL(left);
  DCHECK_NOT_NULL(right);

  switch (op) {
    // Arithmetic operators (+, -, *, /, %, **)
    case 0:  // Addition (also string concatenation)
      if (left->kind() == TypeKind::kString ||
          right->kind() == TypeKind::kString) {
        return TSType::String(zone_);
      }
      if (left->kind() == TypeKind::kNumber &&
          right->kind() == TypeKind::kNumber) {
        return TSType::Number(zone_);
      }
      if (left->kind() == TypeKind::kBigInt &&
          right->kind() == TypeKind::kBigInt) {
        return TSType::BigInt(zone_);
      }
      // Mixed types, return number as default (widening)
      return TSType::Number(zone_);

    case 1:  // Subtraction
    case 2:  // Multiplication
    case 3:  // Division
    case 4:  // Modulo
    case 5:  // Exponentiation
      if (left->kind() == TypeKind::kBigInt &&
          right->kind() == TypeKind::kBigInt) {
        return TSType::BigInt(zone_);
      }
      return TSType::Number(zone_);

    // Comparison operators (==, ===, !=, !==, <, >, <=, >=)
    case 6:   // Equal
    case 7:   // StrictEqual
    case 8:   // NotEqual
    case 9:   // StrictNotEqual
    case 10:  // LessThan
    case 11:  // GreaterThan
    case 12:  // LessThanOrEqual
    case 13:  // GreaterThanOrEqual
      return TSType::Boolean(zone_);

    // Logical operators (&&, ||, !)
    case 14:  // LogicalAnd
    case 15:  // LogicalOr
      return PromoteToCommonType(left, right);

    case 16:  // LogicalNot
      return TSType::Boolean(zone_);

    // Bitwise operators
    case 17:  // BitwiseAnd
    case 18:  // BitwiseOr
    case 19:  // BitwiseXor
      if (left->kind() == TypeKind::kBigInt &&
          right->kind() == TypeKind::kBigInt) {
        return TSType::BigInt(zone_);
      }
      return TSType::Number(zone_);

    // Shift operators
    case 20:  // LeftShift
    case 21:  // RightShift
    case 22:  // UnsignedRightShift
      return TSType::Number(zone_);

    default:
      return TSType::Any(zone_);
  }
}

TSType* TSTypeSystem::InferPropertyAccessType(TSType* object,
                                               const char* property_name) {
  DCHECK_NOT_NULL(object);
  DCHECK_NOT_NULL(property_name);

  // Handle union types: if object is A | B, we look up the property on each
  // and form a union of the result types
  if (object->kind() == TypeKind::kUnion && object->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = object->GetUnionMembers();
    TSType* result = InferPropertyAccessType(members->at(0), property_name);
    for (int i = 1; i < members->length(); i++) {
      TSType* member_result =
          InferPropertyAccessType(members->at(i), property_name);
      if (result->kind() == TypeKind::kAny) {
        result = member_result;
      } else if (member_result->kind() != TypeKind::kAny) {
        result = TSType::CreateUnion(zone_, result, member_result);
      }
    }
    return result;
  }

  // Handle intersection types
  if (object->kind() == TypeKind::kIntersection &&
      object->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = object->GetUnionMembers();
    for (int i = 0; i < members->length(); i++) {
      TSType* result =
          InferPropertyAccessType(members->at(i), property_name);
      if (result->kind() != TypeKind::kAny) {
        return result;
      }
    }
    return TSType::Any(zone_);
  }

  // Handle interface types: look up the property by name
  ZoneList<PropertyDescriptor>* props = object->GetProperties();
  if (props != nullptr) {
    for (int i = 0; i < props->length(); i++) {
      if (strcmp(props->at(i).name, property_name) == 0) {
        return props->at(i).type;
      }
    }
  }

  // Handle array types: the 'length' property returns number,
  // numeric index returns element type
  if (object->kind() == TypeKind::kArray) {
    if (strcmp(property_name, "length") == 0) {
      return TSType::Number(zone_);
    }
    if (object->GetElementType() != nullptr) {
      return object->GetElementType();
    }
  }

  // Handle tuple types: numeric index returns element type at that position
  if (object->kind() == TypeKind::kTuple) {
    if (strcmp(property_name, "length") == 0) {
      return TSType::Number(zone_);
    }
    if (object->GetUnionMembers() != nullptr) {
      return object->GetUnionMembers()->at(0);
    }
  }

  // Handle Promise types: 'then' returns a function type
  if (object->kind() == TypeKind::kPromise) {
    if (strcmp(property_name, "then") == 0 ||
        strcmp(property_name, "catch") == 0 ||
        strcmp(property_name, "finally") == 0) {
      return TSType::Function(zone_);
    }
  }

  // For object type or unknown, return any
  return TSType::Any(zone_);
}

TSType* TSTypeSystem::InferCallType(TSType* callee,
                                     ZoneList<TSType*>* arg_types) {
  DCHECK_NOT_NULL(callee);

  if (callee->kind() == TypeKind::kFunction) {
    // Check if argument count matches
    ZoneList<TSType*>* param_types = callee->GetParamTypes();
    if (param_types != nullptr && arg_types != nullptr) {
      if (param_types->length() != arg_types->length()) {
        // Argument count mismatch - return any
        return TSType::Any(zone_);
      }
      // Check argument type compatibility
      for (int i = 0; i < param_types->length(); i++) {
        if (!arg_types->at(i)->IsAssignableTo(param_types->at(i))) {
          return TSType::Any(zone_);
        }
      }
    }
    return callee->GetReturnType();
  }

  // If callee is a union, try each member
  if (callee->kind() == TypeKind::kUnion && callee->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = callee->GetUnionMembers();
    for (int i = 0; i < members->length(); i++) {
      TSType* result = InferCallType(members->at(i), arg_types);
      if (result->kind() != TypeKind::kAny) {
        return result;
      }
    }
    return TSType::Any(zone_);
  }

  // Non-callable type - return any as fallback
  return TSType::Any(zone_);
}

TSType* TSTypeSystem::PromoteToCommonType(TSType* a, TSType* b) {
  DCHECK_NOT_NULL(a);
  DCHECK_NOT_NULL(b);

  if (a->IsIdenticalTo(b)) return a;

  // any dominates
  if (a->kind() == TypeKind::kAny) return a;
  if (b->kind() == TypeKind::kAny) return b;

  // never is absorbed
  if (a->kind() == TypeKind::kNever) return b;
  if (b->kind() == TypeKind::kNever) return a;

  // If one is a subtype of the other, return the supertype
  if (a->IsSubtypeOf(b)) return b;
  if (b->IsSubtypeOf(a)) return a;

  // Numeric promotion: boolean + number -> number
  if (a->IsNumberLike() && b->IsNumberLike()) {
    return TSType::Number(zone_);
  }

  // String promotion: string + anything -> string (for + operator)
  if (a->kind() == TypeKind::kString || b->kind() == TypeKind::kString) {
    return TSType::String(zone_);
  }

  // If types are incompatible, create a union
  return TSType::CreateUnion(zone_, a, b);
}

TSType* TSTypeSystem::Widening(TSType* type) {
  DCHECK_NOT_NULL(type);

  switch (type->kind()) {
    case TypeKind::kLiteral:
      // Widen literal to its base type
      if (type->GetElementType() != nullptr) {
        return type->GetElementType();
      }
      return type;

    case TypeKind::kBoolean:
      return TSType::Number(zone_);

    case TypeKind::kNumber:
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
      return type;

    default:
      return type;
  }
}

TSType* TSTypeSystem::GetAny() {
  if (cached_any_ == nullptr) {
    cached_any_ = TSType::Any(zone_);
  }
  return cached_any_;
}

TSType* TSTypeSystem::GetNever() {
  if (cached_never_ == nullptr) {
    cached_never_ = TSType::Never(zone_);
  }
  return cached_never_;
}

bool TSTypeSystem::CheckAssignment(TSType* target, TSType* source) {
  DCHECK_NOT_NULL(target);
  DCHECK_NOT_NULL(source);

  // target is any -> always succeeds
  if (target->kind() == TypeKind::kAny) return true;

  // source is any -> always succeeds (but may be unsafe)
  if (source->kind() == TypeKind::kAny) return true;

  // Use assignability check
  return source->IsAssignableTo(target);
}

bool TSTypeSystem::CheckCall(TSType* callee,
                              ZoneList<TSType*>* arg_types) {
  DCHECK_NOT_NULL(callee);

  if (callee->kind() == TypeKind::kAny) return true;

  if (callee->kind() != TypeKind::kFunction &&
      callee->kind() != TypeKind::kUnion) {
    return false;
  }

  if (callee->kind() == TypeKind::kFunction) {
    ZoneList<TSType*>* param_types = callee->GetParamTypes();
    if (param_types == nullptr) return true;
    if (arg_types == nullptr) return param_types->length() == 0;
    if (param_types->length() != arg_types->length()) return false;

    for (int i = 0; i < param_types->length(); i++) {
      if (!arg_types->at(i)->IsAssignableTo(param_types->at(i))) {
        return false;
      }
    }
    return true;
  }

  // For union types, check if any member is callable with the given args
  if (callee->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = callee->GetUnionMembers();
    for (int i = 0; i < members->length(); i++) {
      if (CheckCall(members->at(i), arg_types)) return true;
    }
  }

  return false;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8