#include "src/ts/ts-type-system.h"

#include "src/base/logging.h"

namespace v8 {
namespace internal {

class AstRawString;

namespace ts {

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
    case TypeKind::kTrue:
    case TypeKind::kFalse:
      return true;
    default:
      return false;
  }
}

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
    case TypeKind::kConstructor:
      return true;
    default:
      return false;
  }
}

static bool IsFunctionLikeKind(TypeKind kind) {
  return kind == TypeKind::kFunction;
}

static bool IsNumericKind(TypeKind kind) {
  return kind == TypeKind::kNumber || kind == TypeKind::kBoolean ||
         kind == TypeKind::kBigInt || kind == TypeKind::kTrue ||
         kind == TypeKind::kFalse;
}

// ---------------------------------------------------------------------------
// TSType - Name() for diagnostic messages
// ---------------------------------------------------------------------------

const char* TSType::Name() const {
  if (name_ != nullptr) return name_;
  switch (kind_) {
    case TypeKind::kAny: return "any";
    case TypeKind::kUnknown: return "unknown";
    case TypeKind::kNever: return "never";
    case TypeKind::kBoolean: return "boolean";
    case TypeKind::kNumber: return "number";
    case TypeKind::kString: return "string";
    case TypeKind::kSymbol: return "symbol";
    case TypeKind::kBigInt: return "bigint";
    case TypeKind::kUndefined: return "undefined";
    case TypeKind::kNull: return "null";
    case TypeKind::kVoid: return "void";
    case TypeKind::kObject: return "object";
    case TypeKind::kInterface: return "interface";
    case TypeKind::kArray: return "Array";
    case TypeKind::kTuple: return "tuple";
    case TypeKind::kFunction: return "function";
    case TypeKind::kUnion: return "union";
    case TypeKind::kIntersection: return "intersection";
    case TypeKind::kLiteral:
      return literal_value_ ? literal_value_ : "literal";
    case TypeKind::kTemplateLiteral: return "template-literal";
    case TypeKind::kConditional: return "conditional";
    case TypeKind::kMapped: return "mapped";
    case TypeKind::kIndexedAccess: return "indexed-access";
    case TypeKind::kKeyof: return "keyof";
    case TypeKind::kPartial: return "Partial";
    case TypeKind::kRequired: return "Required";
    case TypeKind::kReadonly: return "Readonly";
    case TypeKind::kPick: return "Pick";
    case TypeKind::kOmit: return "Omit";
    case TypeKind::kRecord: return "Record";
    case TypeKind::kPromise: return "Promise";
    case TypeKind::kGeneric: return "generic";
    case TypeKind::kThis: return "this";
    case TypeKind::kInferred: return "inferred";
    case TypeKind::kSatisfies: return "satisfies";
    case TypeKind::kTypeReference: return "type-reference";
    case TypeKind::kEnum: return "enum";
    case TypeKind::kNamespace: return "namespace";
    case TypeKind::kParameter: return "parameter";
    case TypeKind::kTrue: return "true";
    case TypeKind::kFalse: return "false";
    case TypeKind::kConstructor: return "constructor";
  }
  return "unknown-type";
}

// ---------------------------------------------------------------------------
// TSType - Literal / Truthiness checks
// ---------------------------------------------------------------------------

bool TSType::IsNumberLiteral() const {
  if (kind_ != TypeKind::kLiteral) return false;
  return element_type_ != nullptr &&
         (element_type_->kind() == TypeKind::kNumber ||
          element_type_->kind() == TypeKind::kBoolean);
}

bool TSType::IsStringLiteral() const {
  if (kind_ != TypeKind::kLiteral) return false;
  return element_type_ != nullptr &&
         element_type_->kind() == TypeKind::kString;
}

bool TSType::IsBooleanLiteral() const {
  if (kind_ != TypeKind::kLiteral) return false;
  return element_type_ != nullptr &&
         element_type_->kind() == TypeKind::kBoolean;
}

bool TSType::IsTruthy() const {
  switch (kind_) {
    case TypeKind::kNull:
    case TypeKind::kUndefined:
    case TypeKind::kNever:
      return false;
    case TypeKind::kLiteral:
      if (literal_value_ != nullptr) {
        if (strcmp(literal_value_, "false") == 0 ||
            strcmp(literal_value_, "0") == 0 ||
            strcmp(literal_value_, "") == 0) {
          return false;
        }
      }
      return true;
    case TypeKind::kFalse:
      return false;
    default:
      return true;
  }
}

bool TSType::IsCallable() const {
  if (kind_ == TypeKind::kFunction) return true;
  if (kind_ == TypeKind::kConstructor) return true;
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (union_members_->at(i)->IsCallable()) return true;
    }
  }
  return false;
}

bool TSType::IsIterable() const {
  if (kind_ == TypeKind::kArray || kind_ == TypeKind::kTuple) return true;
  if (kind_ == TypeKind::kString) return true;
  if (kind_ == TypeKind::kInterface && properties_ != nullptr) {
    for (int i = 0; i < properties_->length(); i++) {
      if (strcmp(properties_->at(i).name, "Symbol.iterator") == 0) return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// TSType - Downcast / As* methods
// ---------------------------------------------------------------------------

TSType* TSType::AsFunctionType() { return this; }
TSType* TSType::AsUnionType() { return this; }
TSType* TSType::AsObjectType() { return this; }
TSType* TSType::AsArrayType() { return this; }
TSType* TSType::AsIntersectionType() { return this; }
TSType* TSType::AsConditionalType() { return this; }

// ---------------------------------------------------------------------------
// TSType - Property lookup
// ---------------------------------------------------------------------------

PropertyDescriptor* TSType::GetProperty(const AstRawString* prop_name) const {
  if (properties_ == nullptr || prop_name == nullptr) return nullptr;
  for (int i = 0; i < properties_->length(); i++) {
    if (properties_->at(i).name != nullptr &&
        prop_name->IsOneByteEqualTo(properties_->at(i).name)) {
      return &properties_->at(i);
    }
  }
  return nullptr;
}

bool TSType::HasProperty(const AstRawString* prop_name) const {
  return GetProperty(prop_name) != nullptr;
}

// ---------------------------------------------------------------------------
// TSType - arity / size / type_at
// ---------------------------------------------------------------------------

int TSType::arity() const {
  if (kind_ == TypeKind::kUnion || kind_ == TypeKind::kIntersection) {
    return union_members_ ? union_members_->length() : 0;
  }
  if (kind_ == TypeKind::kTuple) {
    return union_members_ ? union_members_->length() : 0;
  }
  return param_types_ ? param_types_->length() : 0;
}

TSType* TSType::type_at(int index) const {
  if (kind_ == TypeKind::kUnion || kind_ == TypeKind::kIntersection ||
      kind_ == TypeKind::kTuple) {
    if (union_members_ != nullptr && index >= 0 &&
        index < union_members_->length()) {
      return union_members_->at(index);
    }
    return nullptr;
  }
  if (param_types_ != nullptr && index >= 0 &&
      index < param_types_->length()) {
    return param_types_->at(index);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// TSType - DefaultType for conditional types
// ---------------------------------------------------------------------------

TSType* TSType::DefaultType() const {
  if (kind_ == TypeKind::kConditional) {
    if (param_types_ != nullptr && param_types_->length() > 0) {
      return param_types_->at(0);
    }
  }
  return const_cast<TSType*>(this);
}

// ---------------------------------------------------------------------------
// TSType - Primitive factories
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
TSType* TSType::Function(Zone* zone) {
  return zone->New<TSType>(TypeKind::kFunction);
}
TSType* TSType::True(Zone* zone) {
  return zone->New<TSType>(TypeKind::kTrue);
}
TSType* TSType::False(Zone* zone) {
  return zone->New<TSType>(TypeKind::kFalse);
}
TSType* TSType::Constructor(Zone* zone, TSType* return_type) {
  TSType* result = zone->New<TSType>(TypeKind::kConstructor);
  result->element_type_ = return_type;
  return result;
}

// ---------------------------------------------------------------------------
// TSType - Complex factories
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
// TSType - Type queries
// ---------------------------------------------------------------------------

bool TSType::IsPrimitive() const { return IsPrimitiveKind(kind_); }
bool TSType::IsObjectLike() const { return IsObjectLikeKind(kind_); }
bool TSType::IsFunctionLike() const { return IsFunctionLikeKind(kind_); }
bool TSType::IsUnion() const { return kind_ == TypeKind::kUnion; }
bool TSType::IsIntersection() const { return kind_ == TypeKind::kIntersection; }
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
// TSType - Getters
// ---------------------------------------------------------------------------

ZoneList<PropertyDescriptor>* TSType::GetProperties() const {
  return properties_;
}
TSType* TSType::GetElementType() const { return element_type_; }
ZoneList<TSType*>* TSType::GetUnionMembers() const { return union_members_; }
TSType* TSType::GetReturnType() const { return return_type_; }
ZoneList<TSType*>* TSType::GetParamTypes() const { return param_types_; }

TSType* TSType::GetConstraint() const {
  if (type_params_ && type_params_->length() > 0) {
    return type_params_->at(0).constraint;
  }
  return nullptr;
}

TSType* TSType::GetDefaultType() const {
  if (type_params_ && type_params_->length() > 0) {
    return type_params_->at(0).default_type;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// TSType - ExcludeNull / ExcludeUndefined / GetNonNullType
// ---------------------------------------------------------------------------

TSType* TSType::ExcludeNull(Zone* zone) const {
  if (kind_ == TypeKind::kNull) {
    return zone->New<TSType>(TypeKind::kNever);
  }
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    ZoneList<TSType*>* filtered =
        zone->New<ZoneList<TSType*>>(union_members_->length(), zone);
    for (int i = 0; i < union_members_->length(); i++) {
      TSType* member = union_members_->at(i);
      if (member->kind() != TypeKind::kNull) {
        filtered->Add(member, zone);
      }
    }
    if (filtered->length() == 0) {
      return zone->New<TSType>(TypeKind::kNever);
    }
    if (filtered->length() == 1) {
      return filtered->at(0);
    }
    TSType* result = zone->New<TSType>(TypeKind::kUnion);
    result->union_members_ = filtered;
    return result;
  }
  return const_cast<TSType*>(this);
}

TSType* TSType::ExcludeUndefined(Zone* zone) const {
  if (kind_ == TypeKind::kUndefined) {
    return zone->New<TSType>(TypeKind::kNever);
  }
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    ZoneList<TSType*>* filtered =
        zone->New<ZoneList<TSType*>>(union_members_->length(), zone);
    for (int i = 0; i < union_members_->length(); i++) {
      TSType* member = union_members_->at(i);
      if (member->kind() != TypeKind::kUndefined) {
        filtered->Add(member, zone);
      }
    }
    if (filtered->length() == 0) {
      return zone->New<TSType>(TypeKind::kNever);
    }
    if (filtered->length() == 1) {
      return filtered->at(0);
    }
    TSType* result = zone->New<TSType>(TypeKind::kUnion);
    result->union_members_ = filtered;
    return result;
  }
  return const_cast<TSType*>(this);
}

TSType* TSType::GetNonNullType(Zone* zone) const {
  TSType* result = ExcludeNull(zone);
  result = result->ExcludeUndefined(zone);
  return result;
}

// ---------------------------------------------------------------------------
// TSType - IsSubtypeOf (full implementation)
// ---------------------------------------------------------------------------

bool TSType::IsSubtypeOf(const TSType* other) const {
  if (this == other) return true;
  if (kind_ == TypeKind::kNever) return true;
  if (other->kind_ == TypeKind::kAny) return true;
  if (kind_ == TypeKind::kAny) return false;
  if (other->kind_ == TypeKind::kUnknown) return true;
  if (kind_ == TypeKind::kUnknown) return false;

  if (kind_ == TypeKind::kLiteral && element_type_ != nullptr) {
    if (element_type_->IsSubtypeOf(other)) return true;
  }

  if (kind_ == TypeKind::kUndefined && other->kind() == TypeKind::kVoid) {
    return true;
  }

  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsSubtypeOf(other)) return false;
    }
    return true;
  }

  if (other->kind_ == TypeKind::kUnion && other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (IsSubtypeOf(other->union_members_->at(i))) return true;
    }
    return false;
  }

  if (kind_ == TypeKind::kIntersection && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (union_members_->at(i)->IsSubtypeOf(other)) return true;
    }
    return false;
  }

  if (other->kind_ == TypeKind::kIntersection &&
      other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (!IsSubtypeOf(other->union_members_->at(i))) return false;
    }
    return true;
  }

  if (kind_ == TypeKind::kBoolean && other->kind() == TypeKind::kNumber) {
    return true;
  }
  if ((kind_ == TypeKind::kTrue || kind_ == TypeKind::kFalse) &&
      other->kind() == TypeKind::kBoolean) {
    return true;
  }
  if ((kind_ == TypeKind::kTrue || kind_ == TypeKind::kFalse) &&
      other->kind() == TypeKind::kNumber) {
    return true;
  }

  if (kind_ == TypeKind::kLiteral && other->kind() == TypeKind::kString &&
      element_type_ != nullptr && element_type_->kind() == TypeKind::kString) {
    return true;
  }
  if (kind_ == TypeKind::kLiteral && other->kind() == TypeKind::kNumber &&
      element_type_ != nullptr &&
      (element_type_->kind() == TypeKind::kNumber ||
       element_type_->kind() == TypeKind::kBoolean)) {
    return true;
  }
  if (kind_ == TypeKind::kLiteral && other->kind() == TypeKind::kBoolean &&
      element_type_ != nullptr && element_type_->kind() == TypeKind::kBoolean) {
    return true;
  }

  if (kind_ == TypeKind::kArray && other->kind() == TypeKind::kArray) {
    if (element_type_ == nullptr && other->element_type_ == nullptr) return true;
    if (element_type_ == nullptr) return true;
    if (other->element_type_ == nullptr) return false;
    return element_type_->IsSubtypeOf(other->element_type_);
  }
  if (kind_ == TypeKind::kArray && other->kind() == TypeKind::kObject) {
    return true;
  }

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

  if (kind_ == TypeKind::kFunction && other->kind() == TypeKind::kFunction) {
    if (return_type_ != nullptr && other->return_type_ != nullptr) {
      if (!return_type_->IsSubtypeOf(other->return_type_)) return false;
    }
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
  if (kind_ == TypeKind::kFunction && other->kind() == TypeKind::kObject) {
    return true;
  }

  if (kind_ == TypeKind::kConstructor && other->kind() == TypeKind::kConstructor) {
    if (element_type_ != nullptr && other->element_type_ != nullptr) {
      return element_type_->IsSubtypeOf(other->element_type_);
    }
    return true;
  }

  if (IsObjectLike() && other->IsObjectLike()) {
    ZoneList<PropertyDescriptor>* other_props = other->GetProperties();
    if (other_props != nullptr && properties_ != nullptr) {
      for (int i = 0; i < other_props->length(); i++) {
        const PropertyDescriptor& other_prop = other_props->at(i);
        bool found = false;
        for (int j = 0; j < properties_->length(); j++) {
          const PropertyDescriptor& this_prop = properties_->at(j);
          if (strcmp(this_prop.name, other_prop.name) == 0) {
            if (!other_prop.is_optional && this_prop.is_optional) {
              return false;
            }
            if (other_prop.type != nullptr && this_prop.type != nullptr) {
              if (!this_prop.type->IsSubtypeOf(other_prop.type)) {
                return false;
              }
            }
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
    return other_props == nullptr;
  }

  if (kind_ == TypeKind::kPromise && other->kind() == TypeKind::kPromise) {
    if (element_type_ == nullptr || other->element_type_ == nullptr) return true;
    return element_type_->IsSubtypeOf(other->element_type_);
  }
  if (kind_ == TypeKind::kPromise && other->kind() == TypeKind::kObject) {
    return true;
  }

  if (kind_ == TypeKind::kPartial || kind_ == TypeKind::kRequired ||
      kind_ == TypeKind::kReadonly || kind_ == TypeKind::kPick ||
      kind_ == TypeKind::kOmit || kind_ == TypeKind::kRecord) {
    if (other->kind() == TypeKind::kObject) return true;
  }

  if (kind_ == TypeKind::kConditional && referenced_type_ != nullptr &&
      element_type_ != nullptr) {
    bool extends_check = referenced_type_->IsSubtypeOf(element_type_);
    if (extends_check) {
      if (return_type_ != nullptr) return return_type_->IsSubtypeOf(other);
    } else {
      if (param_types_ != nullptr && param_types_->length() > 0) {
        return param_types_->at(0)->IsSubtypeOf(other);
      }
    }
  }

  if (kind_ == TypeKind::kTemplateLiteral) {
    if (other->kind() == TypeKind::kString) return true;
  }

  if (kind_ == other->kind_) {
    if (kind_ == TypeKind::kLiteral && literal_value_ != nullptr &&
        other->literal_value_ != nullptr) {
      return strcmp(literal_value_, other->literal_value_) == 0;
    }
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSType - IsAssignableTo
// ---------------------------------------------------------------------------

bool TSType::IsAssignableTo(const TSType* other) const {
  if (kind_ == TypeKind::kAny) return true;
  if (other->kind_ == TypeKind::kAny) return true;
  if (IsSubtypeOf(other)) return true;

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

  if (kind_ == TypeKind::kUndefined && other->kind() == TypeKind::kVoid) {
    return true;
  }

  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (union_members_->at(i)->IsAssignableTo(other)) return true;
    }
    return false;
  }

  if (other->kind_ == TypeKind::kUnion && other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (IsAssignableTo(other->union_members_->at(i))) return true;
    }
    return false;
  }

  if (kind_ == TypeKind::kIntersection && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (union_members_->at(i)->IsAssignableTo(other)) return true;
    }
    return false;
  }

  if (other->kind_ == TypeKind::kIntersection &&
      other->union_members_ != nullptr) {
    for (int i = 0; i < other->union_members_->length(); i++) {
      if (!IsAssignableTo(other->union_members_->at(i))) return false;
    }
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSType - IsIdenticalTo
// ---------------------------------------------------------------------------

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
    case TypeKind::kTrue:
    case TypeKind::kFalse:
      return true;
    case TypeKind::kLiteral:
      if (literal_value_ == nullptr && other->literal_value_ == nullptr)
        return true;
      if (literal_value_ == nullptr || other->literal_value_ == nullptr)
        return false;
      if (strcmp(literal_value_, other->literal_value_) != 0) return false;
      break;
    default:
      break;
  }

  if (name_ != nullptr || other->name_ != nullptr) {
    if (name_ == nullptr || other->name_ == nullptr) return false;
    if (strcmp(name_, other->name_) != 0) return false;
  }

  if (element_type_ != nullptr || other->element_type_ != nullptr) {
    if (element_type_ == nullptr || other->element_type_ == nullptr) return false;
    if (!element_type_->IsIdenticalTo(other->element_type_)) return false;
  }

  if (return_type_ != nullptr || other->return_type_ != nullptr) {
    if (return_type_ == nullptr || other->return_type_ == nullptr) return false;
    if (!return_type_->IsIdenticalTo(other->return_type_)) return false;
  }

  if (param_types_ != nullptr || other->param_types_ != nullptr) {
    if (param_types_ == nullptr || other->param_types_ == nullptr) return false;
    if (param_types_->length() != other->param_types_->length()) return false;
    for (int i = 0; i < param_types_->length(); i++) {
      if (!param_types_->at(i)->IsIdenticalTo(other->param_types_->at(i))) {
        return false;
      }
    }
  }

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
// TSType - ToV8Type bridge
// ---------------------------------------------------------------------------

void* TSType::ToV8Type(Zone* zone) const {
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
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kTuple:
    case TypeKind::kFunction:
    case TypeKind::kPromise:
    case TypeKind::kConstructor:
    case TypeKind::kTrue:
    case TypeKind::kFalse:
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
// TSType - Shape hints
// ---------------------------------------------------------------------------

bool TSType::HasKnownShape() const {
  if (kind_ == TypeKind::kInterface && properties_ != nullptr) return true;
  if (kind_ == TypeKind::kObject && properties_ != nullptr) return true;
  if (kind_ == TypeKind::kPartial && referenced_type_ != nullptr) {
    return referenced_type_->HasKnownShape();
  }
  if (kind_ == TypeKind::kRequired && referenced_type_ != nullptr) {
    return referenced_type_->HasKnownShape();
  }
  if (kind_ == TypeKind::kReadonly && referenced_type_ != nullptr) {
    return referenced_type_->HasKnownShape();
  }
  return false;
}

int TSType::GetPropertyCount() const {
  if (properties_ != nullptr) return properties_->length();
  return 0;
}

bool TSType::IsStable() const {
  if (IsPrimitive() || kind_ == TypeKind::kAny || kind_ == TypeKind::kUnknown ||
      kind_ == TypeKind::kNever) {
    return true;
  }
  if (kind_ == TypeKind::kLiteral && element_type_ != nullptr) {
    return element_type_->IsStable();
  }
  if (kind_ == TypeKind::kArray && element_type_ != nullptr) {
    return element_type_->IsStable();
  }
  if (kind_ == TypeKind::kPromise && element_type_ != nullptr) {
    return element_type_->IsStable();
  }
  if (kind_ == TypeKind::kUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsStable()) return false;
    }
    return true;
  }
  if (kind_ == TypeKind::kFunction) {
    if (return_type_ != nullptr && !return_type_->IsStable()) return false;
    if (param_types_ != nullptr) {
      for (int i = 0; i < param_types_->length(); i++) {
        if (!param_types_->at(i)->IsStable()) return false;
      }
    }
    return true;
  }
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
// TSTypeSystem - Getters
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::GetThisType() {
  return zone_->New<TSType>(zone_, TypeKind::kThis);
}

TSType* TSTypeSystem::GetFunctionType() {
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kFunction);
  result->param_types_ = zone_->New<ZoneList<TSType*>>(0, zone_);
  result->return_type_ = cached_any_;
  return result;
}

// ---------------------------------------------------------------------------
// TSTypeSystem - Create* factories
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::CreateUnionType(TSType* left, TSType* right) {
  return TSType::CreateUnion(zone_, left, right);
}

TSType* TSTypeSystem::CreateUnionType(ZoneList<TSType*>* types) {
  if (types == nullptr || types->length() == 0) return cached_never_;
  if (types->length() == 1) return types->at(0);
  return NewUnion(types);
}

TSType* TSTypeSystem::CreateObjectType(ZoneList<PropertyDescriptor*>* props) {
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kObject);
  if (props != nullptr) {
    ZoneList<PropertyDescriptor>* new_props =
        zone_->New<ZoneList<PropertyDescriptor>>(props->size(), zone_);
    for (int i = 0; i < props->size(); i++) {
      new_props->Add(*props->at(i), zone_);
    }
    result->set_properties(new_props);
  }
  return result;
}

TSType* TSTypeSystem::CreateArrayType(TSType* elem_type) {
  return NewArray(elem_type);
}

TSType* TSTypeSystem::CreateFunctionType(ZoneList<TSType*>* param_types,
                                          TSType* return_type) {
  return NewFunction(param_types, return_type);
}

TSType* TSTypeSystem::CreateInstanceOfType(const AstRawString* class_name) {
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kObject);
  if (class_name != nullptr) {
    result->set_name(class_name->c_str());
  }
  return result;
}

// ---------------------------------------------------------------------------
// TSTypeSystem - Mapped Types implementation
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::CreatePartialType(TSType* source) {
  if (source == nullptr) return cached_any_;
  ZoneList<PropertyDescriptor>* src_props = source->GetProperties();
  ZoneList<PropertyDescriptor>* new_props = nullptr;
  if (src_props != nullptr) {
    new_props = zone_->New<ZoneList<PropertyDescriptor>>(src_props->length(), zone_);
    for (int i = 0; i < src_props->length(); i++) {
      PropertyDescriptor prop = src_props->at(i);
      prop.is_optional = true;
      new_props->Add(prop, zone_);
    }
  }
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kPartial);
  result->referenced_type_ = source;
  result->set_properties(new_props);
  result->set_name("Partial");
  return result;
}

TSType* TSTypeSystem::CreateRequiredType(TSType* source) {
  if (source == nullptr) return cached_any_;
  ZoneList<PropertyDescriptor>* src_props = source->GetProperties();
  ZoneList<PropertyDescriptor>* new_props = nullptr;
  if (src_props != nullptr) {
    new_props = zone_->New<ZoneList<PropertyDescriptor>>(src_props->length(), zone_);
    for (int i = 0; i < src_props->length(); i++) {
      PropertyDescriptor prop = src_props->at(i);
      prop.is_optional = false;
      new_props->Add(prop, zone_);
    }
  }
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kRequired);
  result->referenced_type_ = source;
  result->set_properties(new_props);
  result->set_name("Required");
  return result;
}

TSType* TSTypeSystem::CreateReadonlyType(TSType* source) {
  if (source == nullptr) return cached_any_;
  ZoneList<PropertyDescriptor>* src_props = source->GetProperties();
  ZoneList<PropertyDescriptor>* new_props = nullptr;
  if (src_props != nullptr) {
    new_props = zone_->New<ZoneList<PropertyDescriptor>>(src_props->length(), zone_);
    for (int i = 0; i < src_props->length(); i++) {
      PropertyDescriptor prop = src_props->at(i);
      prop.is_readonly = true;
      prop.has_readonly_modifier = true;
      new_props->Add(prop, zone_);
    }
  }
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kReadonly);
  result->referenced_type_ = source;
  result->set_properties(new_props);
  result->set_name("Readonly");
  return result;
}

TSType* TSTypeSystem::CreatePickType(TSType* source,
                                      ZoneList<const char*>* keys) {
  if (source == nullptr || keys == nullptr) return cached_any_;
  ZoneList<PropertyDescriptor>* src_props = source->GetProperties();
  ZoneList<PropertyDescriptor>* new_props =
      zone_->New<ZoneList<PropertyDescriptor>>(keys->length(), zone_);
  if (src_props != nullptr) {
    for (int i = 0; i < keys->length(); i++) {
      const char* key = keys->at(i);
      for (int j = 0; j < src_props->length(); j++) {
        if (strcmp(src_props->at(j).name, key) == 0) {
          new_props->Add(src_props->at(j), zone_);
          break;
        }
      }
    }
  }
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kPick);
  result->referenced_type_ = source;
  result->set_properties(new_props);
  result->set_name("Pick");
  return result;
}

TSType* TSTypeSystem::CreateOmitType(TSType* source,
                                      ZoneList<const char*>* keys) {
  if (source == nullptr) return cached_any_;
  ZoneList<PropertyDescriptor>* src_props = source->GetProperties();
  int count = src_props ? src_props->length() : 0;
  ZoneList<PropertyDescriptor>* new_props =
      zone_->New<ZoneList<PropertyDescriptor>>(count, zone_);
  if (src_props != nullptr) {
    for (int i = 0; i < src_props->length(); i++) {
      bool should_omit = false;
      for (int j = 0; j < keys->length(); j++) {
        if (strcmp(src_props->at(i).name, keys->at(j)) == 0) {
          should_omit = true;
          break;
        }
      }
      if (!should_omit) {
        new_props->Add(src_props->at(i), zone_);
      }
    }
  }
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kOmit);
  result->referenced_type_ = source;
  result->set_properties(new_props);
  result->set_name("Omit");
  return result;
}

TSType* TSTypeSystem::CreateRecordType(TSType* key_type, TSType* value_type) {
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kRecord);
  result->referenced_type_ = key_type;
  result->element_type_ = value_type;
  result->set_name("Record");
  return result;
}

TSType* TSTypeSystem::CreateMappedType(TSType* source, TypeKind mapped_kind) {
  switch (mapped_kind) {
    case TypeKind::kPartial:
      return CreatePartialType(source);
    case TypeKind::kRequired:
      return CreateRequiredType(source);
    case TypeKind::kReadonly:
      return CreateReadonlyType(source);
    default:
      return source;
  }
}

// ---------------------------------------------------------------------------
// TSTypeSystem - ResolveConditionalType
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::ResolveConditionalType(
    TSType* check, TSType* extends_type, TSType* true_type, TSType* false_type,
    ZoneList<TypeParameter*>* type_params) {
  if (check == nullptr || extends_type == nullptr) {
    return false_type ? false_type : cached_any_;
  }

  TSType* resolved_check = check;
  TSType* resolved_extends = extends_type;

  if (type_params != nullptr && type_params->length() > 0) {
    ZoneList<TSType*>* args = zone_->New<ZoneList<TSType*>>(0, zone_);
    for (int i = 0; i < type_params->length(); i++) {
      TypeParameter* param = &type_params->at(i);
      if (param->constraint != nullptr) {
        args->Add(param->constraint, zone_);
      } else {
        args->Add(cached_any_, zone_);
      }
    }
    resolved_check = SubstituteTypeParameters(check, type_params, args);
    resolved_extends = SubstituteTypeParameters(extends_type, type_params, args);
  }

  if (resolved_check->IsSubtypeOf(resolved_extends)) {
    return true_type ? true_type : cached_any_;
  }

  if (resolved_check->IsUnion() && resolved_check->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = resolved_check->GetUnionMembers();
    bool all_extend = true;
    bool any_extend = false;
    for (int i = 0; i < members->length(); i++) {
      if (members->at(i)->IsSubtypeOf(resolved_extends)) {
        any_extend = true;
      } else {
        all_extend = false;
      }
    }
    if (all_extend) return true_type ? true_type : cached_any_;
    if (!any_extend) return false_type ? false_type : cached_any_;
    return cached_any_;
  }

  return false_type ? false_type : cached_any_;
}

// ---------------------------------------------------------------------------
// TSTypeSystem - ResolveTemplateLiteralType
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::ResolveTemplateLiteralType(
    ZoneList<const char*>* parts, ZoneList<TSType*>* expression_types) {
  TSType* result = zone_->New<TSType>(zone_, TypeKind::kTemplateLiteral);

  if (parts == nullptr || expression_types == nullptr) {
    return cached_string_;
  }

  int total_parts = parts->length();
  int expr_count = expression_types->length();

  if (total_parts < 2 || expr_count == 0) {
    return cached_string_;
  }

  ZoneList<TSType*>* current =
      zone_->New<ZoneList<TSType*>>(1, zone_);
  TSType* init_lit = zone_->New<TSType>(zone_, TypeKind::kLiteral);
  init_lit->set_literal_value("");
  init_lit->element_type_ = cached_string_;
  current->Add(init_lit, zone_);

  for (int i = 0; i < expr_count; i++) {
    TSType* expr_type = expression_types->at(i);
    if (expr_type == nullptr) continue;

    ZoneList<TSType*>* next =
        zone_->New<ZoneList<TSType*>>(0, zone_);

    const char* prefix = parts->at(i);
    const char* suffix = (i + 1 < total_parts) ? parts->at(i + 1) : "";

    if (expr_type->IsLiteral() && expr_type->GetLiteralValue() != nullptr) {
      for (int c = 0; c < current->length(); c++) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s%s",
                 prefix, expr_type->GetLiteralValue(), suffix);
        TSType* lit = zone_->New<TSType>(zone_, TypeKind::kLiteral);
        lit->set_literal_value(buf);
        lit->element_type_ = cached_string_;
        next->Add(lit, zone_);
      }
    } else if (expr_type->IsString() || expr_type->IsNumber() ||
               expr_type->IsBoolean()) {
      for (int c = 0; c < current->length(); c++) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s", prefix, suffix);
        TSType* lit = zone_->New<TSType>(zone_, TypeKind::kLiteral);
        lit->set_literal_value(buf);
        lit->element_type_ = expr_type;
        next->Add(lit, zone_);
      }
    } else if (expr_type->IsUnion() && expr_type->GetUnionMembers() != nullptr) {
      ZoneList<TSType*>* members = expr_type->GetUnionMembers();
      for (int m = 0; m < members->length(); m++) {
        TSType* member = members->at(m);
        if (member->IsLiteral() && member->GetLiteralValue() != nullptr) {
          for (int c = 0; c < current->length(); c++) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s%s%s",
                     prefix, member->GetLiteralValue(), suffix);
            TSType* lit = zone_->New<TSType>(zone_, TypeKind::kLiteral);
            lit->set_literal_value(buf);
            lit->element_type_ = cached_string_;
            next->Add(lit, zone_);
          }
        }
      }
    }

    current = next;
  }

  if (current->length() == 0) {
    return cached_string_;
  }

  if (current->length() == 1) {
    return current->at(0);
  }

  result->union_members_ = current;
  return result;
}

// ---------------------------------------------------------------------------
// TSTypeSystem - Generic resolution
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::InstantiateGenericType(
    TSType* generic, ZoneList<TSType*>* type_arguments) {
  if (generic == nullptr) return cached_any_;
  if (type_arguments == nullptr) return generic;

  ZoneList<TypeParameter*>* params = generic->type_params_;
  if (params == nullptr || params->length() == 0) return generic;

  return SubstituteTypeParameters(generic, params, type_arguments);
}

TSType* TSTypeSystem::SubstituteTypeParameters(
    TSType* type, ZoneList<TypeParameter*>* params,
    ZoneList<TSType*>* args) {
  if (type == nullptr || params == nullptr || args == nullptr) return type;
  if (params->length() == 0) return type;

  for (int i = 0; i < params->length() && i < args->length(); i++) {
    if (type->kind_ == TypeKind::kParameter &&
        type->name_ != nullptr &&
        strcmp(type->name_, params->at(i).name) == 0) {
      return args->at(i);
    }
  }

  switch (type->kind_) {
    case TypeKind::kUnion:
    case TypeKind::kIntersection: {
      if (type->union_members_ == nullptr) return type;
      ZoneList<TSType*>* new_members =
          zone_->New<ZoneList<TSType*>>(type->union_members_->length(), zone_);
      for (int i = 0; i < type->union_members_->length(); i++) {
        new_members->Add(
            SubstituteTypeParameters(type->union_members_->at(i), params, args),
            zone_);
      }
      TSType* result = zone_->New<TSType>(zone_, type->kind_);
      result->union_members_ = new_members;
      result->name_ = type->name_;
      return result;
    }
    case TypeKind::kArray: {
      if (type->element_type_ == nullptr) return type;
      TSType* new_elem = SubstituteTypeParameters(type->element_type_, params, args);
      TSType* result = zone_->New<TSType>(zone_, TypeKind::kArray);
      result->element_type_ = new_elem;
      return result;
    }
    case TypeKind::kFunction: {
      ZoneList<TSType*>* new_params = nullptr;
      if (type->param_types_ != nullptr) {
        new_params = zone_->New<ZoneList<TSType*>>(type->param_types_->length(), zone_);
        for (int i = 0; i < type->param_types_->length(); i++) {
          new_params->Add(
              SubstituteTypeParameters(type->param_types_->at(i), params, args),
              zone_);
        }
      }
      TSType* new_return = type->return_type_ != nullptr
                               ? SubstituteTypeParameters(type->return_type_, params, args)
                               : nullptr;
      TSType* result = zone_->New<TSType>(zone_, TypeKind::kFunction);
      result->param_types_ = new_params;
      result->return_type_ = new_return;
      return result;
    }
    case TypeKind::kPromise: {
      if (type->element_type_ == nullptr) return type;
      TSType* new_elem = SubstituteTypeParameters(type->element_type_, params, args);
      TSType* result = zone_->New<TSType>(zone_, TypeKind::kPromise);
      result->element_type_ = new_elem;
      return result;
    }
    case TypeKind::kInterface:
    case TypeKind::kObject: {
      if (type->properties_ == nullptr) return type;
      ZoneList<PropertyDescriptor>* new_props =
          zone_->New<ZoneList<PropertyDescriptor>>(type->properties_->length(), zone_);
      for (int i = 0; i < type->properties_->length(); i++) {
        PropertyDescriptor prop = type->properties_->at(i);
        if (prop.type != nullptr) {
          prop.type = SubstituteTypeParameters(prop.type, params, args);
        }
        new_props->Add(prop, zone_);
      }
      TSType* result = zone_->New<TSType>(zone_, type->kind_);
      result->set_properties(new_props);
      result->set_name(type->name_);
      return result;
    }
    case TypeKind::kConditional: {
      TSType* new_check = type->referenced_type_ != nullptr
                              ? SubstituteTypeParameters(type->referenced_type_, params, args)
                              : nullptr;
      TSType* new_extends = type->element_type_ != nullptr
                               ? SubstituteTypeParameters(type->element_type_, params, args)
                               : nullptr;
      TSType* new_true = type->return_type_ != nullptr
                             ? SubstituteTypeParameters(type->return_type_, params, args)
                             : nullptr;
      TSType* new_false = nullptr;
      if (type->param_types_ != nullptr && type->param_types_->length() > 0) {
        new_false = SubstituteTypeParameters(type->param_types_->at(0), params, args);
      }
      return ResolveConditionalType(new_check, new_extends, new_true, new_false, nullptr);
    }
    case TypeKind::kPartial:
    case TypeKind::kRequired:
    case TypeKind::kReadonly:
    case TypeKind::kPick:
    case TypeKind::kOmit: {
      if (type->referenced_type_ == nullptr) return type;
      TSType* new_source = SubstituteTypeParameters(type->referenced_type_, params, args);
      return CreateMappedType(new_source, type->kind_);
    }
    case TypeKind::kTuple: {
      if (type->union_members_ == nullptr) return type;
      ZoneList<TSType*>* new_elems =
          zone_->New<ZoneList<TSType*>>(type->union_members_->length(), zone_);
      for (int i = 0; i < type->union_members_->length(); i++) {
        new_elems->Add(
            SubstituteTypeParameters(type->union_members_->at(i), params, args),
            zone_);
      }
      TSType* result = zone_->New<TSType>(zone_, TypeKind::kTuple);
      result->union_members_ = new_elems;
      return result;
    }
    case TypeKind::kTypeReference: {
      if (type->type_arguments_ != nullptr) {
        ZoneList<TSType*>* new_args =
            zone_->New<ZoneList<TSType*>>(type->type_arguments_->length(), zone_);
        for (int i = 0; i < type->type_arguments_->length(); i++) {
          new_args->Add(
              SubstituteTypeParameters(type->type_arguments_->at(i), params, args),
              zone_);
        }
        TSType* result = zone_->New<TSType>(zone_, TypeKind::kTypeReference);
        result->set_name(type->name_);
        result->type_arguments_ = new_args;
        return result;
      }
      return type;
    }
    default:
      return type;
  }
}

// ---------------------------------------------------------------------------
// TSTypeSystem - Core operations
// ---------------------------------------------------------------------------

bool TSTypeSystem::IsAssignableTo(TSType* source, TSType* target) {
  if (source == nullptr || target == nullptr) return false;
  return source->IsAssignableTo(target);
}

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
  switch (kind) {
    case TypeKind::kAny:
      if (cached_any_ == nullptr) cached_any_ = TSType::Any(zone);
      return cached_any_;
    case TypeKind::kNever:
      if (cached_never_ == nullptr) cached_never_ = TSType::Never(zone);
      return cached_never_;
    case TypeKind::kBoolean:
      if (cached_boolean_ == nullptr) cached_boolean_ = TSType::Boolean(zone);
      return cached_boolean_;
    case TypeKind::kNumber:
      if (cached_number_ == nullptr) cached_number_ = TSType::Number(zone);
      return cached_number_;
    case TypeKind::kString:
      if (cached_string_ == nullptr) cached_string_ = TSType::String(zone);
      return cached_string_;
    case TypeKind::kUndefined:
      if (cached_undefined_ == nullptr) cached_undefined_ = TSType::Undefined(zone);
      return cached_undefined_;
    case TypeKind::kNull:
      if (cached_null_ == nullptr) cached_null_ = TSType::Null(zone);
      return cached_null_;
    default:
      return zone->New<TSType>(kind);
  }
}

// ---------------------------------------------------------------------------
// TSTypeSystem - Binary / Property / Call inference
// ---------------------------------------------------------------------------

TSType* TSTypeSystem::InferBinaryOpType(TSType* left, TSType* right,
                                         int op) {
  DCHECK_NOT_NULL(left);
  DCHECK_NOT_NULL(right);
  switch (op) {
    case 0:
      if (left->kind() == TypeKind::kString ||
          right->kind() == TypeKind::kString) {
        return cached_string_;
      }
      if (left->kind() == TypeKind::kNumber &&
          right->kind() == TypeKind::kNumber) {
        return cached_number_;
      }
      if (left->kind() == TypeKind::kBigInt &&
          right->kind() == TypeKind::kBigInt) {
        return cached_bigint_;
      }
      return cached_number_;
    case 1: case 2: case 3: case 4: case 5:
      if (left->kind() == TypeKind::kBigInt &&
          right->kind() == TypeKind::kBigInt) {
        return cached_bigint_;
      }
      return cached_number_;
    case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13:
      return cached_boolean_;
    case 14: case 15:
      return PromoteToCommonType(left, right);
    case 16:
      return cached_boolean_;
    case 17: case 18: case 19:
      if (left->kind() == TypeKind::kBigInt &&
          right->kind() == TypeKind::kBigInt) {
        return cached_bigint_;
      }
      return cached_number_;
    case 20: case 21: case 22:
      return cached_number_;
    default:
      return cached_any_;
  }
}

TSType* TSTypeSystem::InferPropertyAccessType(TSType* object,
                                               const char* property_name) {
  DCHECK_NOT_NULL(object);
  DCHECK_NOT_NULL(property_name);

  if (object->kind() == TypeKind::kUnion && object->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = object->GetUnionMembers();
    TSType* result = InferPropertyAccessType(members->at(0), property_name);
    for (int i = 1; i < members->length(); i++) {
      TSType* member_result = InferPropertyAccessType(members->at(i), property_name);
      if (result->kind() == TypeKind::kAny) {
        result = member_result;
      } else if (member_result->kind() != TypeKind::kAny) {
        result = TSType::CreateUnion(zone_, result, member_result);
      }
    }
    return result;
  }

  if (object->kind() == TypeKind::kIntersection &&
      object->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = object->GetUnionMembers();
    for (int i = 0; i < members->length(); i++) {
      TSType* result = InferPropertyAccessType(members->at(i), property_name);
      if (result->kind() != TypeKind::kAny) return result;
    }
    return cached_any_;
  }

  ZoneList<PropertyDescriptor>* props = object->GetProperties();
  if (props != nullptr) {
    for (int i = 0; i < props->length(); i++) {
      if (strcmp(props->at(i).name, property_name) == 0) {
        return props->at(i).type;
      }
    }
  }

  if (object->kind() == TypeKind::kArray) {
    if (strcmp(property_name, "length") == 0) return cached_number_;
    if (object->GetElementType() != nullptr) return object->GetElementType();
  }

  if (object->kind() == TypeKind::kTuple) {
    if (strcmp(property_name, "length") == 0) return cached_number_;
    if (object->GetUnionMembers() != nullptr) return object->GetUnionMembers()->at(0);
  }

  if (object->kind() == TypeKind::kPromise) {
    if (strcmp(property_name, "then") == 0 ||
        strcmp(property_name, "catch") == 0 ||
        strcmp(property_name, "finally") == 0) {
      return GetFunctionType();
    }
  }

  if (object->kind() == TypeKind::kPartial ||
      object->kind() == TypeKind::kRequired ||
      object->kind() == TypeKind::kReadonly ||
      object->kind() == TypeKind::kPick ||
      object->kind() == TypeKind::kOmit) {
    if (object->referenced_type_ != nullptr) {
      return InferPropertyAccessType(object->referenced_type_, property_name);
    }
  }

  return cached_any_;
}

TSType* TSTypeSystem::InferCallType(TSType* callee,
                                     ZoneList<TSType*>* arg_types) {
  DCHECK_NOT_NULL(callee);
  if (callee->kind() == TypeKind::kFunction) {
    ZoneList<TSType*>* param_types = callee->GetParamTypes();
    if (param_types != nullptr && arg_types != nullptr) {
      if (param_types->length() != arg_types->length()) {
        return cached_any_;
      }
      for (int i = 0; i < param_types->length(); i++) {
        if (!arg_types->at(i)->IsAssignableTo(param_types->at(i))) {
          return cached_any_;
        }
      }
    }
    return callee->GetReturnType();
  }

  if (callee->kind() == TypeKind::kUnion && callee->GetUnionMembers() != nullptr) {
    ZoneList<TSType*>* members = callee->GetUnionMembers();
    for (int i = 0; i < members->length(); i++) {
      TSType* result = InferCallType(members->at(i), arg_types);
      if (result->kind() != TypeKind::kAny) return result;
    }
    return cached_any_;
  }

  if (callee->kind() == TypeKind::kConditional &&
      callee->referenced_type_ != nullptr && callee->element_type_ != nullptr) {
    TSType* resolved = ResolveConditionalType(
        callee->referenced_type_, callee->element_type_,
        callee->return_type_,
        callee->param_types_ != nullptr && callee->param_types_->length() > 0
            ? callee->param_types_->at(0) : nullptr,
        nullptr);
    return InferCallType(resolved, arg_types);
  }

  return cached_any_;
}

TSType* TSTypeSystem::PromoteToCommonType(TSType* a, TSType* b) {
  DCHECK_NOT_NULL(a);
  DCHECK_NOT_NULL(b);
  if (a->IsIdenticalTo(b)) return a;
  if (a->kind() == TypeKind::kAny) return a;
  if (b->kind() == TypeKind::kAny) return b;
  if (a->kind() == TypeKind::kNever) return b;
  if (b->kind() == TypeKind::kNever) return a;
  if (a->IsSubtypeOf(b)) return b;
  if (b->IsSubtypeOf(a)) return a;
  if (a->IsNumberLike() && b->IsNumberLike()) return cached_number_;
  if (a->kind() == TypeKind::kString || b->kind() == TypeKind::kString) {
    return cached_string_;
  }
  return TSType::CreateUnion(zone_, a, b);
}

TSType* TSTypeSystem::Widening(TSType* type) {
  DCHECK_NOT_NULL(type);
  switch (type->kind()) {
    case TypeKind::kLiteral:
      if (type->GetElementType() != nullptr) return type->GetElementType();
      return type;
    case TypeKind::kBoolean:
      return cached_number_;
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
  if (cached_any_ == nullptr) cached_any_ = TSType::Any(zone_);
  return cached_any_;
}

TSType* TSTypeSystem::GetNever() {
  if (cached_never_ == nullptr) cached_never_ = TSType::Never(zone_);
  return cached_never_;
}

bool TSTypeSystem::CheckAssignment(TSType* target, TSType* source) {
  DCHECK_NOT_NULL(target);
  DCHECK_NOT_NULL(source);
  if (target->kind() == TypeKind::kAny) return true;
  if (source->kind() == TypeKind::kAny) return true;
  return source->IsAssignableTo(target);
}

bool TSTypeSystem::CheckCall(TSType* callee, ZoneList<TSType*>* arg_types) {
  DCHECK_NOT_NULL(callee);
  if (callee->kind() == TypeKind::kAny) return true;
  if (callee->kind() != TypeKind::kFunction &&
      callee->kind() != TypeKind::kUnion) return false;
  if (callee->kind() == TypeKind::kFunction) {
    ZoneList<TSType*>* param_types = callee->GetParamTypes();
    if (param_types == nullptr) return true;
    if (arg_types == nullptr) return param_types->length() == 0;
    if (param_types->length() != arg_types->length()) return false;
    for (int i = 0; i < param_types->length(); i++) {
      if (!arg_types->at(i)->IsAssignableTo(param_types->at(i))) return false;
    }
    return true;
  }
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