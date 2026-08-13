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
        type_arguments_(nullptr),
        element_type_(nullptr),
        return_type_(nullptr),
        referenced_type_(nullptr),
        name_(nullptr),
        literal_value_(nullptr),
        is_readonly_(false),
        is_optional_(false) {}

  TypeKind kind() const { return kind_; }

  bool IsPrimitive() const {
    return kind_ == TypeKind::kBoolean || kind_ == TypeKind::kNumber ||
           kind_ == TypeKind::kString || kind_ == TypeKind::kSymbol ||
           kind_ == TypeKind::kBigInt || kind_ == TypeKind::kUndefined ||
           kind_ == TypeKind::kNull || kind_ == TypeKind::kVoid;
  }

  bool IsObjectLike() const {
    return kind_ == TypeKind::kObject || kind_ == TypeKind::kInterface ||
           kind_ == TypeKind::kArray || kind_ == TypeKind::kTuple;
  }

  bool IsFunctionLike() const { return kind_ == TypeKind::kFunction; }

  bool IsUnion() const { return kind_ == TypeKind::kUnion; }

  bool IsLiteral() const { return kind_ == TypeKind::kLiteral; }

  bool IsNullable() const {
    return kind_ == TypeKind::kNull || kind_ == TypeKind::kUndefined ||
           kind_ == TypeKind::kAny;
  }

  bool IsStringLike() const { return kind_ == TypeKind::kString; }
  bool IsNumberLike() const { return kind_ == TypeKind::kNumber; }

  const char* GetName() const { return name_; }

  ZoneList<PropertyDescriptor>* GetProperties() const { return properties_; }

  TSType* GetElementType() const { return element_type_; }

  ZoneList<TSType*>* GetUnionMembers() const { return union_members_; }

  TSType* GetReturnType() const { return return_type_; }

  ZoneList<TSType*>* GetParamTypes() const { return param_types_; }

  ZoneList<TSType*>* union_types() const { return union_members_; }

  ZoneList<TSType*>* type_arguments() const { return type_arguments_; }

  void set_referenced_type(TSType* t) { referenced_type_ = t; }
  void set_element_type(TSType* t) { element_type_ = t; }
  void set_return_type(TSType* t) { return_type_ = t; }

  bool IsAssignableTo(const TSType* other) const {
    if (kind_ == TypeKind::kAny || other->kind_ == TypeKind::kAny) return true;
    if (kind_ == other->kind_) return true;
    if (IsLiteral() && other->kind_ == TypeKind::kString) return true;
    if (IsLiteral() && other->kind_ == TypeKind::kNumber) return true;
    if (IsLiteral() && other->kind_ == TypeKind::kBoolean) return true;
    if (kind_ == TypeKind::kNull || kind_ == TypeKind::kUndefined) {
      return other->IsNullable();
    }
    return false;
  }

  bool IsSubtypeOf(const TSType* other) const {
    return IsAssignableTo(other);
  }

  bool IsIdenticalTo(const TSType* other) const {
    return kind_ == other->kind_ && name_ == other->name_;
  }

  bool HasKnownShape() const { return kind_ == TypeKind::kInterface; }
  int GetPropertyCount() const { return properties_ ? properties_->length() : 0; }
  bool IsStable() const { return true; }

  void* ToV8Type(Zone* zone) const { return nullptr; }

  ZoneList<PropertyDescriptor>* properties() { return properties_; }
  void set_properties(ZoneList<PropertyDescriptor>* p) { properties_ = p; }

  ZoneList<TypeParameter*>* type_params() { return type_params_; }
  void set_type_params(ZoneList<TypeParameter>* tp) { type_params_ = tp; }

  void set_name(const char* n) { name_ = n; }
  void set_literal_value(const char* v) { literal_value_ = v; }

  void set_is_readonly(bool v) { is_readonly_ = v; }
  void set_is_optional(bool v) { is_optional_ = v; }

 private:
  TypeKind kind_;
  ZoneList<PropertyDescriptor>* properties_;
  ZoneList<TypeParameter>* type_params_;
  ZoneList<TSType*>* param_types_;
  ZoneList<TSType*>* union_members_;
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
  explicit TSTypeSystem(Zone* zone)
      : zone_(zone),
        initialized_(false),
        cached_any_(nullptr),
        cached_never_(nullptr),
        cached_boolean_(nullptr),
        cached_number_(nullptr),
        cached_string_(nullptr),
        cached_undefined_(nullptr),
        cached_null_(nullptr),
        cached_void_(nullptr),
        cached_unknown_(nullptr),
        cached_symbol_(nullptr),
        cached_object_(nullptr) {}

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
                          ZoneList<TypeParameter>* type_params) {
    registered_names_.Add(name, zone_);
    TSType* iface = zone_->New<TSType>(zone_, TypeKind::kInterface);
    iface->set_name(name);
    iface->set_properties(properties);
    iface->set_type_params(type_params);
    registered_types_.Add(iface, zone_);
  }

  void RegisterClass(const char* name,
                     ZoneList<PropertyDescriptor>* properties,
                     ZoneList<TypeParameter>* type_params) {
    registered_names_.Add(name, zone_);
    TSType* cls = zone_->New<TSType>(zone_, TypeKind::kInterface);
    cls->set_name(name);
    cls->set_properties(properties);
    cls->set_type_params(type_params);
    registered_types_.Add(cls, zone_);
  }

  void RegisterEnum(const char* name, ZoneList<const char*>* members) {
    registered_names_.Add(name, zone_);
    TSType* enum_type = zone_->New<TSType>(zone_, TypeKind::kEnum);
    enum_type->set_name(name);
    registered_types_.Add(enum_type, zone_);
  }

  TSType* LookupType(const char* name) const {
    for (int i = 0; i < registered_names_.length(); i++) {
      if (strcmp(registered_names_.at(i), name) == 0) {
        return registered_types_.at(i);
      }
    }
    return nullptr;
  }

  TSType* CreateAndCache(Zone* zone, TypeKind kind) {
    return zone->New<TSType>(zone, kind);
  }

  TSType* InferBinaryOpType(TSType* left, TSType* right, int op) {
    if (op == 0 || op == 1 || op == 2) {
      return NewNumber();
    }
    return left;
  }

  TSType* InferPropertyAccessType(TSType* object,
                                   const char* property_name) {
    return NewAny();
  }

  TSType* InferCallType(TSType* callee, ZoneList<TSType*>* arg_types) {
    return NewAny();
  }

  TSType* PromoteToCommonType(TSType* a, TSType* b) {
    if (a->kind() == b->kind()) return a;
    return NewAny();
  }

  TSType* Widening(TSType* type) { return type; }

  TSType* GetAny() { return NewAny(); }
  TSType* GetNever() { return NewNever(); }

  bool CheckAssignment(TSType* target, TSType* source) {
    return IsAssignableTo(source, target);
  }

  bool CheckCall(TSType* callee, ZoneList<TSType*>* arg_types) { return true; }

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