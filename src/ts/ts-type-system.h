#ifndef V8_TS_TS_TYPE_SYSTEM_H_
#define V8_TS_TS_TYPE_SYSTEM_H_

#include "src/zone/zone.h"
#include "src/zone/zone-list.h"

namespace v8 {
namespace internal {
namespace ts {

class TSType;
class TSTypeSystem;

enum class TypeKind : uint8_t {
  kAny,
  kUnknown,
  kNever,
  kBoolean,
  kNumber,
  kString,
  kSymbol,
  kBigInt,
  kUndefined,
  kNull,
  kVoid,
  kObject,
  kInterface,
  kArray,
  kTuple,
  kFunction,
  kUnion,
  kIntersection,
  kLiteral,
  kTemplateLiteral,
  kConditional,
  kMapped,
  kIndexedAccess,
  kKeyof,
  kPartial,
  kRequired,
  kReadonly,
  kPick,
  kOmit,
  kRecord,
  kPromise,
  kGeneric,
  kThis,
  kInferred,
  kSatisfies,
  kTypeReference,
  kEnum,
  kNamespace,
  kParameter
};

struct PropertyDescriptor {
  const char* name;
  TSType* type;
  bool is_readonly;
  bool is_optional;
  bool is_public;
  bool is_private;
  bool is_protected;
  bool has_readonly_modifier;

  TSType* GetType() const { return type; }
  const char* GetName() const { return name; }
};

struct TypeParameter {
  const char* name;
  TSType* constraint;
  TSType* default_type;

  void set_constraint(TSType* c) { constraint = c; }
  void set_default_type(TSType* d) { default_type = d; }
};

class TSType : public ZoneObject {
 public:
  explicit TSType(Zone* zone, TypeKind kind)
      : kind_(kind),
        properties_(nullptr),
        type_params_(nullptr),
        param_types_(nullptr),
        union_members_(nullptr),
        union_right_(nullptr),
        type_arguments_(nullptr),
        element_type_(nullptr),
        return_type_(nullptr),
        referenced_type_(nullptr),
        name_(nullptr),
        literal_value_(nullptr),
        is_readonly_(false),
        is_optional_(false) {}

  TypeKind kind() const { return kind_; }

  bool IsPrimitive() const;
  bool IsObjectLike() const;
  bool IsFunctionLike() const;
  bool IsUnion() const;
  bool IsLiteral() const;
  bool IsNullable() const;
  bool IsStringLike() const;
  bool IsNumberLike() const;

  bool IsObject() const { return kind_ == TypeKind::kObject; }
  bool IsArray() const { return kind_ == TypeKind::kArray; }
  bool IsTuple() const { return kind_ == TypeKind::kTuple; }
  bool IsFunction() const { return kind_ == TypeKind::kFunction; }
  bool IsInterface() const { return kind_ == TypeKind::kInterface; }
  bool IsPromise() const { return kind_ == TypeKind::kPromise; }
  bool IsRecord() const { return kind_ == TypeKind::kRecord; }
  bool IsPartial() const { return kind_ == TypeKind::kPartial; }
  bool IsRequired() const { return kind_ == TypeKind::kRequired; }
  bool IsPick() const { return kind_ == TypeKind::kPick; }
  bool IsOmit() const { return kind_ == TypeKind::kOmit; }
  bool IsEnum() const { return kind_ == TypeKind::kEnum; }
  bool IsConditional() const { return kind_ == TypeKind::kConditional; }
  bool IsMapped() const { return kind_ == TypeKind::kMapped; }
  bool IsIndexedAccess() const { return kind_ == TypeKind::kIndexedAccess; }
  bool IsKeyof() const { return kind_ == TypeKind::kKeyof; }
  bool IsThis() const { return kind_ == TypeKind::kThis; }
  bool IsGeneric() const { return kind_ == TypeKind::kGeneric; }
  bool IsTypeReference() const { return kind_ == TypeKind::kTypeReference; }
  bool IsParameter() const { return kind_ == TypeKind::kParameter; }
  bool IsSatisfies() const { return kind_ == TypeKind::kSatisfies; }
  bool IsNamespace() const { return kind_ == TypeKind::kNamespace; }
  bool IsIntersection() const { return kind_ == TypeKind::kIntersection; }
  bool IsReadonly() const { return kind_ == TypeKind::kReadonly; }

  bool IsString() const { return kind_ == TypeKind::kString; }
  bool IsNumber() const { return kind_ == TypeKind::kNumber; }
  bool IsBoolean() const { return kind_ == TypeKind::kBoolean; }
  bool IsVoid() const { return kind_ == TypeKind::kVoid; }
  bool IsNever() const { return kind_ == TypeKind::kNever; }
  bool IsAny() const { return kind_ == TypeKind::kAny; }
  bool IsUnknown() const { return kind_ == TypeKind::kUnknown; }
  bool IsUndefined() const { return kind_ == TypeKind::kUndefined; }
  bool IsNull() const { return kind_ == TypeKind::kNull; }
  bool IsSymbol() const { return kind_ == TypeKind::kSymbol; }
  bool IsBigInt() const { return kind_ == TypeKind::kBigInt; }

  const char* GetName() const;
  ZoneList<PropertyDescriptor>* GetProperties() const;
  TSType* GetElementType() const;
  ZoneList<TSType*>* GetUnionMembers() const;
  TSType* GetReturnType() const;
  ZoneList<TSType*>* GetParamTypes() const;
  const char* GetLiteralValue() const { return literal_value_; }
  TSType* GetConstraint() const {
    return type_params_ && type_params_->length() > 0
               ? type_params_->at(0)->constraint
               : nullptr;
  }
  TSType* GetDefaultType() const {
    return type_params_ && type_params_->length() > 0
               ? type_params_->at(0)->default_type
               : nullptr;
  }
  ZoneList<TSType*>* GetTypeArguments() const { return type_arguments_; }

  ZoneList<TSType*>* union_types() const { return union_members_; }
  ZoneList<TSType*>* type_arguments() const { return type_arguments_; }

  int arity() const {
    if (kind_ == TypeKind::kUnion || kind_ == TypeKind::kIntersection) {
      return union_members_ ? union_members_->length() : 0;
    }
    return param_types_ ? param_types_->length() : 0;
  }

  TSType* AsArrayType() const {
    return kind_ == TypeKind::kArray ? element_type_ : nullptr;
  }

  void set_referenced_type(TSType* t) { referenced_type_ = t; }
  void set_element_type(TSType* t) { element_type_ = t; }
  void set_return_type(TSType* t) { return_type_ = t; }

  bool IsAssignableTo(const TSType* other) const;
  bool IsSubtypeOf(const TSType* other) const;
  bool IsIdenticalTo(const TSType* other) const;

  bool HasKnownShape() const;
  int GetPropertyCount() const;
  bool IsStable() const;

  void* ToV8Type(Zone* zone) const;

  ZoneList<PropertyDescriptor>* properties() { return properties_; }
  void set_properties(ZoneList<PropertyDescriptor>* p) { properties_ = p; }

  ZoneList<TypeParameter>* type_params() { return type_params_; }
  void set_type_params(ZoneList<TypeParameter>* tp) { type_params_ = tp; }

  void set_name(const char* n) { name_ = n; }
  void set_literal_value(const char* v) { literal_value_ = v; }

  void set_is_readonly(bool v) { is_readonly_ = v; }
  void set_is_optional(bool v) { is_optional_ = v; }

  static TSType* Any(Zone* zone);
  static TSType* Unknown(Zone* zone);
  static TSType* Never(Zone* zone);
  static TSType* Boolean(Zone* zone);
  static TSType* Number(Zone* zone);
  static TSType* String(Zone* zone);
  static TSType* Symbol(Zone* zone);
  static TSType* BigInt(Zone* zone);
  static TSType* Undefined(Zone* zone);
  static TSType* Null(Zone* zone);
  static TSType* Void(Zone* zone);
  static TSType* Object(Zone* zone);
  static TSType* Function(Zone* zone) {
    return zone->New<TSType>(TypeKind::kFunction);
  }

  static TSType* CreateInterface(Zone* zone, const char* name,
                                 ZoneList<PropertyDescriptor>* properties,
                                 ZoneList<TypeParameter>* type_params);
  static TSType* CreateArray(Zone* zone, TSType* element_type);
  static TSType* CreateTuple(Zone* zone,
                             ZoneList<TSType*>* element_types);
  static TSType* CreateFunction(Zone* zone, ZoneList<TSType*>* param_types,
                                TSType* return_type, bool is_async);
  static TSType* CreateUnion(Zone* zone, TSType* left, TSType* right);
  static TSType* CreateIntersection(Zone* zone, TSType* left, TSType* right);
  static TSType* CreateLiteral(Zone* zone, const char* value,
                               TSType* base_type);
  static TSType* CreatePromise(Zone* zone, TSType* value_type);
  static TSType* CreateGeneric(Zone* zone, TypeParameter* param);

 private:
  TypeKind kind_;
  ZoneList<PropertyDescriptor>* properties_;
  ZoneList<TypeParameter>* type_params_;
  ZoneList<TSType*>* param_types_;
  ZoneList<TSType*>* union_members_;
  TSType* union_right_;
  ZoneList<TSType*>* type_arguments_;
  TSType* element_type_;
  TSType* return_type_;
  TSType* referenced_type_;
  const char* name_;
  const char* literal_value_;
  bool is_readonly_;
  bool is_optional_;
};

class TSTypeSystem : public ZoneObject {
 public:
  explicit TSTypeSystem(Zone* zone);

  void Initialize(Zone* zone) {
    if (initialized_) return;
    zone_ = zone;
    cached_any_ = zone->New<TSType>(zone, TypeKind::kAny);
    cached_never_ = zone->New<TSType>(zone, TypeKind::kNever);
    cached_boolean_ = zone->New<TSType>(zone, TypeKind::kBoolean);
    cached_number_ = zone->New<TSType>(zone, TypeKind::kNumber);
    cached_string_ = zone->New<TSType>(zone, TypeKind::kString);
    cached_undefined_ = zone->New<TSType>(zone, TypeKind::kUndefined);
    cached_null_ = zone->New<TSType>(zone, TypeKind::kNull);
    cached_void_ = zone->New<TSType>(zone, TypeKind::kVoid);
    cached_unknown_ = zone->New<TSType>(zone, TypeKind::kUnknown);
    cached_symbol_ = zone->New<TSType>(zone, TypeKind::kSymbol);
    cached_object_ = zone->New<TSType>(zone, TypeKind::kObject);
    initialized_ = true;
  }

  Zone* zone() const { return zone_; }

  TSType* NewAny() { return cached_any_; }
  TSType* NewNever() { return cached_never_; }
  TSType* NewBoolean() { return cached_boolean_; }
  TSType* NewNumber() { return cached_number_; }
  TSType* NewString() { return cached_string_; }
  TSType* NewUndefined() { return cached_undefined_; }
  TSType* NewNull() { return cached_null_; }
  TSType* NewVoid() { return cached_void_; }
  TSType* NewUnknown() { return cached_unknown_; }
  TSType* NewSymbol() { return cached_symbol_; }
  TSType* NewObject() { return cached_object_; }

  TSType* NewUnion(ZoneList<TSType*>* types) {
    DCHECK_GE(types->length(), 2);
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kUnion);
    result->union_members_ = zone_->New<ZoneList<TSType*>>(0, zone_);
    for (int i = 0; i < types->length(); i++) {
      result->union_members_->Add(types->at(i), zone_);
    }
    return result;
  }

  TSType* NewIntersection(ZoneList<TSType*>* types) {
    DCHECK_GE(types->length(), 2);
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kIntersection);
    result->union_members_ = zone_->New<ZoneList<TSType*>>(0, zone_);
    for (int i = 0; i < types->length(); i++) {
      result->union_members_->Add(types->at(i), zone_);
    }
    return result;
  }

  TSType* NewArray(TSType* element_type) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kArray);
    result->element_type_ = element_type;
    return result;
  }

  TSType* NewTuple(ZoneList<TSType*>* elem_types) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kTuple);
    result->union_members_ = zone_->New<ZoneList<TSType*>>(0, zone_);
    for (int i = 0; i < elem_types->length(); i++) {
      result->union_members_->Add(elem_types->at(i), zone_);
    }
    return result;
  }

  TSType* NewLiteral(const char* value) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kLiteral);
    result->set_literal_value(value);
    return result;
  }

  TSType* NewTypeReference(const char* name) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kTypeReference);
    result->set_name(name);
    result->type_arguments_ = zone_->New<ZoneList<TSType*>>(0, zone_);
    return result;
  }

  TSType* NewKeyof(TSType* source) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kKeyof);
    result->referenced_type_ = source;
    return result;
  }

  TSType* NewThis() {
    return zone_->New<TSType>(zone_, TypeKind::kThis);
  }

  TSType* NewConditional(TSType* check, TSType* extends_type,
                          TSType* true_type, TSType* false_type) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kConditional);
    result->referenced_type_ = check;
    result->element_type_ = extends_type;
    result->return_type_ = true_type;
    result->param_types_ = zone_->New<ZoneList<TSType*>>(0, zone_);
    result->param_types_->Add(false_type, zone_);
    return result;
  }

  TSType* NewIndexedAccess(TSType* object_type, TSType* index_type) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kIndexedAccess);
    result->referenced_type_ = object_type;
    result->element_type_ = index_type;
    return result;
  }

  TSType* NewMapped() {
    return zone_->New<TSType>(zone_, TypeKind::kMapped);
  }

  TSType* NewFunction(ZoneList<TSType*>* param_types, TSType* return_type) {
    TSType* result = zone_->New<TSType>(zone_, TypeKind::kFunction);
    result->param_types_ = param_types;
    result->return_type_ = return_type;
    return result;
  }

  TypeParameter* NewTypeParameter(const char* name) {
    TypeParameter* param = zone_->New<TypeParameter>();
    param->name = name;
    param->constraint = nullptr;
    param->default_type = nullptr;
    return param;
  }

  bool IsAssignableTo(TSType* source, TSType* target) {
    if (source == nullptr || target == nullptr) return false;
    if (source->kind() == TypeKind::kAny ||
        target->kind() == TypeKind::kAny)
      return true;
    if (source->kind() == target->kind()) return true;
    if (source->IsLiteral() && target->kind() == TypeKind::kString) return true;
    if (source->IsLiteral() && target->kind() == TypeKind::kNumber) return true;
    if (source->IsLiteral() && target->kind() == TypeKind::kBoolean) return true;
    if (source->kind() == TypeKind::kNull ||
        source->kind() == TypeKind::kUndefined) {
      return target->IsNullable();
    }
    if (source->kind() == TypeKind::kNever) return true;
    return false;
  }

  void RegisterInterface(const char* name,
                          ZoneList<PropertyDescriptor>* properties,
                          ZoneList<TypeParameter>* type_params);

  void RegisterClass(const char* name,
                     ZoneList<PropertyDescriptor>* properties,
                     ZoneList<TypeParameter>* type_params);

  void RegisterEnum(const char* name, ZoneList<const char*>* members);

  TSType* LookupType(const char* name) const;

  TSType* CreateAndCache(Zone* zone, TypeKind kind);

  TSType* InferBinaryOpType(TSType* left, TSType* right, int op);

  TSType* InferPropertyAccessType(TSType* object,
                                   const char* property_name);

  TSType* InferCallType(TSType* callee, ZoneList<TSType*>* arg_types);

  TSType* PromoteToCommonType(TSType* a, TSType* b);

  TSType* Widening(TSType* type);

  TSType* GetAny();
  TSType* GetNever();

  bool CheckAssignment(TSType* target, TSType* source);

  bool CheckCall(TSType* callee, ZoneList<TSType*>* arg_types);

 private:
  Zone* zone_;
  bool initialized_;

  ZoneList<TSType*> registered_types_;
  ZoneList<const char*> registered_names_;

  TSType* cached_any_;
  TSType* cached_never_;
  TSType* cached_boolean_;
  TSType* cached_number_;
  TSType* cached_string_;
  TSType* cached_undefined_;
  TSType* cached_null_;
  TSType* cached_void_;
  TSType* cached_unknown_;
  TSType* cached_symbol_;
  TSType* cached_object_;
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_TYPE_SYSTEM_H_