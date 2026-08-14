// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ts/ts-map-extensions.h"

#include "src/base/logging.h"
#include "src/handles/handles-inl.h"
#include "src/heap/factory.h"
#include "src/objects/descriptor-array.h"
#include "src/objects/field-type.h"
#include "src/objects/map.h"
#include "src/objects/object-predicates.h"
#include "src/objects/property.h"
#include "src/objects/property-details.h"
#include "src/objects/smi.h"
#include "src/objects/objects-inl.h"
#include "src/roots/roots.h"
#include "src/ts/ts-type-system.h"

#include "src/objects/js-objects.h"
#include "src/objects/js-objects-inl.h"

namespace v8 {
namespace internal {

namespace ts {

// ---------------------------------------------------------------------------
// Helper: check whether a TS type can be represented by a V8 Map directly
// ---------------------------------------------------------------------------
static bool TypeHasKnownV8Representation(TSType* type) {
  if (type == nullptr) return false;
  TypeKind kind = type->kind();
  return kind == TypeKind::kBoolean || kind == TypeKind::kNumber ||
         kind == TypeKind::kString || kind == TypeKind::kSymbol ||
         kind == TypeKind::kBigInt || kind == TypeKind::kObject ||
         kind == TypeKind::kInterface || kind == TypeKind::kArray;
}

// ---------------------------------------------------------------------------
// Helper: check whether a value conforms to a primitive TS type kind
// ---------------------------------------------------------------------------
static bool IsValueOfPrimitiveKind(Tagged<Object> value, TypeKind kind) {
  switch (kind) {
    case TypeKind::kBoolean:
      return IsBoolean(value);
    case TypeKind::kNumber:
      return IsNumber(value);
    case TypeKind::kString:
      return IsString(value);
    case TypeKind::kSymbol:
      return IsSymbol(value);
    case TypeKind::kBigInt:
      return IsBigInt(value);
    case TypeKind::kNull:
      return IsNull(value);
    case TypeKind::kUndefined:
      return IsUndefined(value);
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Helper: get the default V8 Map for a primitive TS type
// ---------------------------------------------------------------------------
static Handle<Map> GetPrimitiveV8Map(Isolate* isolate, TypeKind kind) {
  ReadOnlyRoots roots(isolate);
  switch (kind) {
    case TypeKind::kBoolean:
      return roots.boolean_map();
    case TypeKind::kNumber:
      return roots.heap_number_map();
    case TypeKind::kString:
      return roots.string_map();
    case TypeKind::kSymbol:
      return roots.symbol_map();
    case TypeKind::kBigInt:
      return roots.bigint_map();
    case TypeKind::kUndefined:
      return roots.undefined_map();
    case TypeKind::kNull:
      return roots.null_map();
    default:
      return Handle<Map>();
  }
}

// ---------------------------------------------------------------------------
// Helper: convert TS property descriptor to V8 PropertyAttributes
// ---------------------------------------------------------------------------
static PropertyAttributes TSPropsToV8Attributes(const PropertyDescriptor& prop) {
  PropertyAttributes attrs = NONE;
  if (prop.is_readonly) {
    attrs = static_cast<PropertyAttributes>(attrs | READ_ONLY);
  }
  if (!prop.is_public) {
    attrs = static_cast<PropertyAttributes>(attrs | DONT_ENUM);
  }
  return attrs;
}

// ---------------------------------------------------------------------------
// Helper: select the best V8 Representation for a TS type
// ---------------------------------------------------------------------------
static Representation SelectRepresentationForTSType(TSType* type) {
  if (type == nullptr) return Representation::Tagged();
  switch (type->kind()) {
    case TypeKind::kBoolean:
      return Representation::Smi();
    case TypeKind::kNumber:
      return Representation::Double();
    case TypeKind::kString:
    case TypeKind::kSymbol:
    case TypeKind::kBigInt:
    case TypeKind::kObject:
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kFunction:
      return Representation::HeapObject();
    case TypeKind::kAny:
    case TypeKind::kUnknown:
      return Representation::Tagged();
    default:
      return Representation::Tagged();
  }
}

// ---------------------------------------------------------------------------
// Helper: get the number of own descriptors from a Map
// ---------------------------------------------------------------------------
static int GetMapOwnDescriptors(Handle<Map> map) {
  return map->NumberOfOwnDescriptors();
}

// ---------------------------------------------------------------------------
// TSFieldType – Factory methods
// ---------------------------------------------------------------------------

TSFieldType* TSFieldType::None(Zone* zone) {
  TSFieldType* result = zone->New<TSFieldType>(kNone);
  return result;
}

TSFieldType* TSFieldType::Class(Zone* zone, Handle<Map> map) {
  TSFieldType* result = zone->New<TSFieldType>(kClass);
  result->map_ = map;
  return result;
}

TSFieldType* TSFieldType::Any(Zone* zone) {
  TSFieldType* result = zone->New<TSFieldType>(kAny);
  return result;
}

TSFieldType* TSFieldType::TSPrimitive(Zone* zone, TSType* type) {
  TSFieldType* result = zone->New<TSFieldType>(kTSPrimitive);
  result->ts_type_ = type;
  return result;
}

TSFieldType* TSFieldType::TSInterface(Zone* zone, TSType* type) {
  TSFieldType* result = zone->New<TSFieldType>(kTSInterface);
  result->ts_type_ = type;
  return result;
}

TSFieldType* TSFieldType::TSUnion(Zone* zone,
                                  ZoneList<TSType*>* members) {
  TSFieldType* result = zone->New<TSFieldType>(kTSUnion);
  result->union_members_ = members;
  return result;
}

TSFieldType* TSFieldType::TSLiteral(Zone* zone, TSType* type) {
  TSFieldType* result = zone->New<TSFieldType>(kTSLiteral);
  result->ts_type_ = type;
  return result;
}

TSFieldType* TSFieldType::Stable(Zone* zone, TSType* type) {
  TSFieldType* result = zone->New<TSFieldType>(kStable);
  result->ts_type_ = type;
  return result;
}

// ---------------------------------------------------------------------------
// TSFieldType – Type checking logic
// ---------------------------------------------------------------------------

bool TSFieldType::NowContains(Object* value) const {
  if (kind_ == kNone) return false;
  if (kind_ == kAny) return true;

  Tagged<Object> tagged_value(value);

  if (kind_ == kClass) {
    if (value == nullptr) return false;
    if (!IsHeapObject(tagged_value)) return false;
    Tagged<Map> value_map = HeapObject::cast(tagged_value)->map();
    return value_map == *map_;
  }

  if (kind_ == kTSPrimitive && ts_type_ != nullptr) {
    return IsValueOfPrimitiveKind(tagged_value, ts_type_->kind());
  }

  if (kind_ == kTSLiteral && ts_type_ != nullptr) {
    TypeKind base_kind = TypeKind::kAny;
    if (ts_type_->GetElementType() != nullptr) {
      base_kind = ts_type_->GetElementType()->kind();
    }
    return IsValueOfPrimitiveKind(tagged_value, base_kind);
  }

  if (kind_ == kStable && ts_type_ != nullptr) {
    if (ts_type_->IsPrimitive()) {
      return IsValueOfPrimitiveKind(tagged_value, ts_type_->kind());
    }
    if (!IsHeapObject(tagged_value)) return false;
    Tagged<Map> value_map = HeapObject::cast(tagged_value)->map();
    return value_map->NumberOfOwnDescriptors() >= 0;
  }

  if (kind_ == kTSInterface && ts_type_ != nullptr) {
    if (value == nullptr || !IsHeapObject(tagged_value)) return false;
    Tagged<Map> value_map = HeapObject::cast(tagged_value)->map();
    ZoneList<PropertyDescriptor>* props = ts_type_->GetProperties();
    if (props != nullptr && props->length() > 0) {
      if (value_map->NumberOfOwnDescriptors() < props->length()) {
        return false;
      }
    }
    return true;
  }

  if (kind_ == kTSUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      TSType* member_type = union_members_->at(i);
      if (member_type->IsPrimitive()) {
        if (IsValueOfPrimitiveKind(tagged_value, member_type->kind())) {
          return true;
        }
      }
    }
    return false;
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSFieldType – Merge (for IC feedback)
// ---------------------------------------------------------------------------
TSFieldType* TSFieldType::Merge(Zone* zone, TSFieldType* other) {
  if (other == nullptr) return this;
  if (kind_ == kNone) return other;
  if (other->kind_ == kNone) return this;
  if (kind_ == kAny || other->kind_ == kAny) return Any(zone);

  if (kind_ == other->kind_) {
    if (kind_ == kClass) {
      if (map_.is_identical_to(other->map_)) return this;
      return Any(zone);
    }
    if (kind_ == kTSPrimitive || kind_ == kTSLiteral || kind_ == kStable) {
      if (ts_type_ != nullptr && other->ts_type_ != nullptr) {
        if (ts_type_->IsIdenticalTo(other->ts_type_)) return this;
      }
      return Any(zone);
    }
    if (kind_ == kTSUnion) {
      return this;
    }
    if (kind_ == kTSInterface) {
      if (ts_type_ != nullptr && other->ts_type_ != nullptr) {
        if (ts_type_->IsIdenticalTo(other->ts_type_)) return this;
      }
      return Any(zone);
    }
  }

  if (kind_ == kTSUnion && other->kind_ == kTSUnion) {
    ZoneList<TSType*>* merged =
        zone->New<ZoneList<TSType*>>(union_members_->length() +
                                     other->union_members_->length(),
                                     zone);
    for (int i = 0; i < union_members_->length(); i++) {
      merged->Add(union_members_->at(i), zone);
    }
    for (int i = 0; i < other->union_members_->length(); i++) {
      merged->Add(other->union_members_->at(i), zone);
    }
    return TSUnion(zone, merged);
  }

  if (kind_ == kTSUnion) {
    ZoneList<TSType*>* merged =
        zone->New<ZoneList<TSType*>>(union_members_->length() + 1, zone);
    for (int i = 0; i < union_members_->length(); i++) {
      merged->Add(union_members_->at(i), zone);
    }
    if (other->ts_type_ != nullptr) {
      merged->Add(other->ts_type_, zone);
    }
    return TSUnion(zone, merged);
  }

  if (other->kind_ == kTSUnion) {
    ZoneList<TSType*>* merged =
        zone->New<ZoneList<TSType*>>(other->union_members_->length() + 1,
                                     zone);
    if (ts_type_ != nullptr) {
      merged->Add(ts_type_, zone);
    }
    for (int i = 0; i < other->union_members_->length(); i++) {
      merged->Add(other->union_members_->at(i), zone);
    }
    return TSUnion(zone, merged);
  }

  return Any(zone);
}

// ---------------------------------------------------------------------------
// TSFieldType – Promote to less precise type
// ---------------------------------------------------------------------------
TSFieldType* TSFieldType::Promote(Zone* zone) {
  switch (kind_) {
    case kNone:
      return Any(zone);

    case kClass:
      return Any(zone);

    case kTSPrimitive:
      if (ts_type_ != nullptr) {
        if (ts_type_->kind() == TypeKind::kBoolean) {
          TSType* widened = zone->New<TSType>(zone, TypeKind::kNumber);
          return TSPrimitive(zone, widened);
        }
        return Any(zone);
      }
      return Any(zone);

    case kTSLiteral:
      if (ts_type_ != nullptr && ts_type_->GetElementType() != nullptr) {
        return TSPrimitive(zone, ts_type_->GetElementType());
      }
      return Any(zone);

    case kStable:
      if (ts_type_ != nullptr) {
        return TSPrimitive(zone, ts_type_);
      }
      return Any(zone);

    case kTSInterface:
      return Any(zone);

    case kTSUnion:
      if (union_members_ != nullptr && union_members_->length() > 0) {
        return TSPrimitive(zone, union_members_->at(0));
      }
      return Any(zone);

    case kAny:
      return this;
  }

  return Any(zone);
}

// ---------------------------------------------------------------------------
// TSFieldType – Convert to V8's native FieldType
// ---------------------------------------------------------------------------
Tagged<FieldType> TSFieldType::ToV8FieldType() const {
  switch (kind_) {
    case kNone:
      return FieldType::None();

    case kAny:
      return FieldType::Any();

    case kClass:
      if (!map_.is_null()) {
        return FieldType::Class(*map_);
      }
      return FieldType::Any();

    case kTSPrimitive:
    case kTSInterface:
    case kTSLiteral:
    case kStable:
      if (ts_type_ != nullptr && ts_type_->IsPrimitive()) {
        return FieldType::Any();
      }
      if (kind_ == kStable && !map_.is_null()) {
        return FieldType::Class(*map_);
      }
      return FieldType::Any();

    case kTSUnion:
      return FieldType::Any();
  }

  return FieldType::Any();
}

// ---------------------------------------------------------------------------
// TSFieldType – Stability check
// ---------------------------------------------------------------------------
bool TSFieldType::IsGuaranteedStable() const {
  if (kind_ == kNone) return true;
  if (kind_ == kAny) return false;

  if (kind_ == kClass) {
    if (!map_.is_null()) {
      return map_->is_stable();
    }
    return false;
  }

  if (kind_ == kStable) return true;

  if (kind_ == kTSPrimitive && ts_type_ != nullptr) {
    return ts_type_->IsStable();
  }

  if (kind_ == kTSLiteral && ts_type_ != nullptr) {
    return ts_type_->IsStable();
  }

  if (kind_ == kTSInterface && ts_type_ != nullptr) {
    return ts_type_->IsStable();
  }

  if (kind_ == kTSUnion && union_members_ != nullptr) {
    for (int i = 0; i < union_members_->length(); i++) {
      if (!union_members_->at(i)->IsStable()) return false;
    }
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Constructor
// ---------------------------------------------------------------------------

TSMapFactory::TSMapFactory(Isolate* isolate)
    : isolate_(isolate),
      cached_maps_(0, nullptr),
      metadata_table_(16, nullptr) {}

// ---------------------------------------------------------------------------
// TSMapFactory – Create an empty Map
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::CreateEmptyMap() {
  Handle<Map> map = isolate_->factory()->NewMap();
  return map;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Create a Map with descriptors from TS properties
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::CreateMapWithDescriptors(
    ZoneList<PropertyDescriptor>* props, Zone* zone) {
  DCHECK_NOT_NULL(props);
  DCHECK_NOT_NULL(zone);

  if (props->length() == 0) {
    return CreateEmptyMap();
  }

  Handle<Map> map = isolate_->factory()->NewMap();

  Handle<Map> current = map;
  for (int i = 0; i < props->length(); i++) {
    const PropertyDescriptor& prop = props->at(i);

    MaybeHandle<String> maybe_name =
        isolate_->factory()->NewStringFromUtf8(
            base::StrVector(prop.name));
    Handle<String> name_handle = maybe_name.ToHandleChecked();
    Handle<Name> name_as_name = name_handle;

    Representation repr = SelectRepresentationForTSType(prop.type);
    PropertyAttributes attrs = TSPropsToV8Attributes(prop);
    PropertyConstness constness =
        prop.is_readonly ? PropertyConstness::kConst
                         : PropertyConstness::kMutable;

    Handle<FieldType> field_type = handle(FieldType::Any(), isolate_);

    MaybeHandle<Map> result = Map::CopyWithField(
        isolate_, current, name_as_name, field_type, attrs, constness, repr,
        INSERT_TRANSITION);

    if (!result.is_null()) {
      current = result.ToHandleChecked();
    }
  }

  return current;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Set inobject properties count
// ---------------------------------------------------------------------------

void TSMapFactory::SetInobjectProperties(Handle<Map> map, int count) {
  DCHECK_GE(count, 0);
  if (count > JSObject::kMaxInObjectProperties) {
    count = JSObject::kMaxInObjectProperties;
  }
  int instance_size = JSObject::kHeaderSize + kTaggedSize * count;
  map->set_instance_size(instance_size);
  map->SetInObjectPropertiesStartInWords(JSObject::kHeaderSize / kTaggedSize);
  map->set_unused_property_fields(count);
}

// ---------------------------------------------------------------------------
// TSMapFactory – Mark all fields as readonly
// ---------------------------------------------------------------------------

void TSMapFactory::SetFieldsAsReadonly(Handle<Map> map) {
  int own_descriptors = map->NumberOfOwnDescriptors();
  if (own_descriptors == 0) return;

  Tagged<DescriptorArray> descriptors =
      Cast<DescriptorArray>(map->instance_descriptors());

  for (int i = 0; i < own_descriptors; i++) {
    InternalIndex idx(i);
    PropertyDetails details = descriptors->GetDetails(idx);
    details = details.set_attributes(
        static_cast<PropertyAttributes>(details.attributes() | READ_ONLY));
    descriptors->SetDetails(idx, details);
  }
}

// ---------------------------------------------------------------------------
// TSMapFactory – Create Map from TS type
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::CreateMapFromType(TSType* type, Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  if (!TypeHasKnownV8Representation(type)) {
    return CreateEmptyMap();
  }

  switch (type->kind()) {
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
    case TypeKind::kObject:
    case TypeKind::kFunction:
    case TypeKind::kPromise: {
      Handle<Map> map = isolate_->factory()->NewMap();
      TSMapMetadata* metadata = zone->New<TSMapMetadata>();
      metadata->ts_type = type;
      metadata->is_stable_by_ts = type->IsStable();
      metadata->expected_property_count = type->GetPropertyCount();
      metadata->creation_order = static_cast<int>(cached_maps_.length());
      AttachMetadata(map, metadata, zone);
      return map;
    }

    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kTuple:
    case TypeKind::kLiteral:
    case TypeKind::kUnion:
    case TypeKind::kIntersection:
    case TypeKind::kPartial:
    case TypeKind::kRequired:
    case TypeKind::kReadonly:
    case TypeKind::kPick:
    case TypeKind::kOmit:
    case TypeKind::kRecord:
    case TypeKind::kGeneric:
    case TypeKind::kTypeReference:
    case TypeKind::kConditional:
    case TypeKind::kMapped:
    case TypeKind::kIndexedAccess:
    case TypeKind::kKeyof:
    case TypeKind::kThis:
    case TypeKind::kInferred:
    case TypeKind::kSatisfies:
    case TypeKind::kEnum:
    case TypeKind::kNamespace:
    case TypeKind::kParameter:
    case TypeKind::kTemplateLiteral: {
      ZoneList<PropertyDescriptor>* props = type->GetProperties();
      if (props != nullptr && props->length() > 0) {
        Handle<Map> map = CreateMapWithDescriptors(props, zone);
        TSMapMetadata* metadata = zone->New<TSMapMetadata>();
        metadata->ts_type = type;
        metadata->is_stable_by_ts = type->IsStable();
        metadata->expected_property_count = props->length();
        metadata->creation_order = static_cast<int>(cached_maps_.length());
        AttachMetadata(map, metadata, zone);
        cached_maps_.Add(map, zone);
        return map;
      }

      if (type->kind() == TypeKind::kArray && type->GetElementType() != nullptr) {
        Handle<Map> map = isolate_->factory()->NewMap();
        TSMapMetadata* metadata = zone->New<TSMapMetadata>();
        metadata->ts_type = type;
        metadata->is_stable_by_ts = type->IsStable();
        metadata->expected_property_count = 1;
        metadata->creation_order = static_cast<int>(cached_maps_.length());
        AttachMetadata(map, metadata, zone);
        cached_maps_.Add(map, zone);
        return map;
      }

      Handle<Map> map = isolate_->factory()->NewMap();
      TSMapMetadata* metadata = zone->New<TSMapMetadata>();
      metadata->ts_type = type;
      metadata->is_stable_by_ts = type->IsStable();
      metadata->expected_property_count = 0;
      metadata->creation_order = static_cast<int>(cached_maps_.length());
      AttachMetadata(map, metadata, zone);
      cached_maps_.Add(map, zone);
      return map;
    }
  }

  Handle<Map> map = CreateEmptyMap();
  return map;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Create Maps for all properties of a type
// ---------------------------------------------------------------------------

ZoneList<Handle<Map>>* TSMapFactory::CreateMapsForType(TSType* type,
                                                        Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  ZoneList<Handle<Map>>* result =
      zone->New<ZoneList<Handle<Map>>>(4, zone);

  ZoneList<PropertyDescriptor>* props = type->GetProperties();
  if (props == nullptr || props->length() == 0) {
    result->Add(CreateMapFromType(type, zone), zone);
    return result;
  }

  for (int i = 0; i < props->length(); i++) {
    PropertyDescriptor& prop = props->at(i);
    if (prop.type != nullptr) {
      Handle<Map> prop_map = CreateMapFromType(prop.type, zone);
      result->Add(prop_map, zone);
    }
  }

  Handle<Map> main_map = CreateMapFromType(type, zone);
  result->Add(main_map, zone);

  return result;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Create a stable Map
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::CreateStableMap(TSType* type, Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  Handle<Map> map = CreateMapFromType(type, zone);

  TSMapMetadata* metadata = GetMetadata(map);
  if (metadata == nullptr) {
    metadata = zone->New<TSMapMetadata>();
    metadata->ts_type = type;
  }

  metadata->is_stable_by_ts = true;
  metadata->is_pre_allocated = true;

  AttachMetadata(map, metadata, zone);

  SetFieldsAsReadonly(map);

  return map;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Attach metadata to Map
// ---------------------------------------------------------------------------

void TSMapFactory::AttachMetadata(Handle<Map> map,
                                  TSMapMetadata* metadata,
                                  Zone* zone) {
  DCHECK_NOT_NULL(map);
  DCHECK_NOT_NULL(metadata);

  metadata_table_.Add(metadata, zone);

  map->set_bit_field3(map->bit_field3() |
                      (1u << kTSTypeMetadataBit));
}

// ---------------------------------------------------------------------------
// TSMapFactory – Get metadata from Map
// ---------------------------------------------------------------------------

TSMapMetadata* TSMapFactory::GetMetadata(Handle<Map> map) {
  DCHECK_NOT_NULL(map);

  uint32_t bit_field3 = map->bit_field3();
  if (!(bit_field3 & (1u << kTSTypeMetadataBit))) {
    return nullptr;
  }

  for (int i = metadata_table_.length() - 1; i >= 0; i--) {
    TSMapMetadata* md = metadata_table_.at(i);
    if (md != nullptr && md->ts_type != nullptr) {
      return md;
    }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Create specialized transition
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::CreateSpecializedTransition(
    Handle<Map> from_map, TSType* new_property_type,
    const char* property_name, Zone* zone) {
  DCHECK_NOT_NULL(from_map);
  DCHECK_NOT_NULL(new_property_type);
  DCHECK_NOT_NULL(property_name);
  DCHECK_NOT_NULL(zone);

  MaybeHandle<String> maybe_name =
      isolate_->factory()->NewStringFromUtf8(
          base::StrVector(property_name));
  Handle<String> name_handle = maybe_name.ToHandleChecked();
  Handle<Name> name_as_name = name_handle;

  Representation repr = SelectRepresentationForTSType(new_property_type);
  PropertyAttributes attrs = NONE;
  PropertyConstness constness = PropertyConstness::kMutable;

  Handle<FieldType> field_type = handle(FieldType::Any(), isolate_);

  MaybeHandle<Map> result = Map::CopyWithField(
      isolate_, from_map, name_as_name, field_type, attrs, constness, repr,
      INSERT_TRANSITION);

  if (!result.is_null()) {
    Handle<Map> new_map = result.ToHandleChecked();

    TSMapMetadata* existing = GetMetadata(from_map);
    if (existing != nullptr) {
      TSMapMetadata* new_metadata = zone->New<TSMapMetadata>();
      new_metadata->ts_type = existing->ts_type;
      new_metadata->is_stable_by_ts = existing->is_stable_by_ts;
      new_metadata->expected_property_count =
          existing->expected_property_count + 1;
      new_metadata->creation_order = existing->creation_order + 1;

      if (existing->field_types != nullptr) {
        new_metadata->field_types =
            zone->New<ZoneList<FieldType*>>(
                existing->field_types->length() + 1, zone);
        for (int i = 0; i < existing->field_types->length(); i++) {
          new_metadata->field_types->Add(existing->field_types->at(i), zone);
        }
      }

      AttachMetadata(new_map, new_metadata, zone);
    }

    return new_map;
  }

  return from_map;
}

// ---------------------------------------------------------------------------
// TSMapFactory – Pre-allocate Maps for common TS patterns
// ---------------------------------------------------------------------------

void TSMapFactory::PreAllocateMaps(Zone* zone) {
  DCHECK_NOT_NULL(zone);

  cached_maps_.Clear();

  TSTypeSystem type_system(zone);
  type_system.Initialize(zone);

  Handle<Map> bool_map = CreateMapFromType(type_system.NewBoolean(), zone);
  cached_maps_.Add(bool_map, zone);

  Handle<Map> number_map = CreateMapFromType(type_system.NewNumber(), zone);
  cached_maps_.Add(number_map, zone);

  Handle<Map> string_map = CreateMapFromType(type_system.NewString(), zone);
  cached_maps_.Add(string_map, zone);

  Handle<Map> symbol_map = CreateMapFromType(type_system.NewSymbol(), zone);
  cached_maps_.Add(symbol_map, zone);

  Handle<Map> bigint_map = CreateMapFromType(type_system.NewBigInt(), zone);
  cached_maps_.Add(bigint_map, zone);

  Handle<Map> void_map = CreateMapFromType(type_system.NewVoid(), zone);
  cached_maps_.Add(void_map, zone);

  Handle<Map> undefined_map =
      CreateMapFromType(type_system.NewUndefined(), zone);
  cached_maps_.Add(undefined_map, zone);

  Handle<Map> null_map = CreateMapFromType(type_system.NewNull(), zone);
  cached_maps_.Add(null_map, zone);

  Handle<Map> any_map = CreateMapFromType(type_system.NewAny(), zone);
  cached_maps_.Add(any_map, zone);

  Handle<Map> unknown_map = CreateMapFromType(type_system.NewUnknown(), zone);
  cached_maps_.Add(unknown_map, zone);

  Handle<Map> never_map = CreateMapFromType(type_system.NewNever(), zone);
  cached_maps_.Add(never_map, zone);

  Handle<Map> object_map = CreateMapFromType(type_system.NewObject(), zone);
  cached_maps_.Add(object_map, zone);

  Handle<Map> function_map =
      CreateMapFromType(type_system.NewFunction(nullptr, nullptr), zone);
  cached_maps_.Add(function_map, zone);

  Handle<Map> promise_map =
      CreateMapFromType(type_system.NewPromise(type_system.NewAny()), zone);
  cached_maps_.Add(promise_map, zone);

  Handle<Map> array_map =
      CreateMapFromType(type_system.NewArray(type_system.NewAny()), zone);
  cached_maps_.Add(array_map, zone);

  Handle<Map> map_map = CreateEmptyMap();
  cached_maps_.Add(map_map, zone);
}

// ---------------------------------------------------------------------------
// TSMapFactory – Get or create Map for a primitive TS type
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::GetPrimitiveMap(TSType* type, Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  if (type->IsPrimitive()) {
    Handle<Map> existing = GetPrimitiveV8Map(isolate_, type->kind());
    if (!existing.is_null()) {
      return existing;
    }
  }

  for (int i = 0; i < cached_maps_.length(); i++) {
    Handle<Map> cached = cached_maps_.at(i);
    TSMapMetadata* md = GetMetadata(cached);
    if (md != nullptr && md->ts_type == type) {
      return cached;
    }
  }

  Handle<Map> new_map = CreateMapFromType(type, zone);
  cached_maps_.Add(new_map, zone);
  return new_map;
}

// ---------------------------------------------------------------------------
// TSICOptimizer – Constructor
// ---------------------------------------------------------------------------

TSICOptimizer::TSICOptimizer(Isolate* isolate) : isolate_(isolate) {}

// ---------------------------------------------------------------------------
// TSICOptimizer – Determine IC kind for a typed property access
// ---------------------------------------------------------------------------

int TSICOptimizer::GetICKindForProperty(TSType* object_type,
                                        const char* property_name) {
  DCHECK_NOT_NULL(object_type);
  DCHECK_NOT_NULL(property_name);

  TypeKind kind = object_type->kind();

  if (kind == TypeKind::kAny || kind == TypeKind::kUnknown) {
    return 1;
  }

  if (object_type->HasKnownShape()) {
    return 3;
  }

  if (kind == TypeKind::kNumber || kind == TypeKind::kString ||
      kind == TypeKind::kBoolean) {
    return 2;
  }

  if (kind == TypeKind::kFunction) {
    return 4;
  }

  if (kind == TypeKind::kArray) {
    if (strcmp(property_name, "length") == 0) {
      return 5;
    }
    return 6;
  }

  if (kind == TypeKind::kUnion) {
    return 7;
  }

  return 1;
}

// ---------------------------------------------------------------------------
// TSICOptimizer – Check if IC can be skipped entirely
// ---------------------------------------------------------------------------

bool TSICOptimizer::CanSkipIC(TSType* object_type,
                              const char* property_name) {
  DCHECK_NOT_NULL(object_type);
  DCHECK_NOT_NULL(property_name);

  if (object_type->kind() == TypeKind::kAny ||
      object_type->kind() == TypeKind::kUnknown) {
    return false;
  }

  if (object_type->IsPrimitive()) {
    return true;
  }

  if (object_type->HasKnownShape()) {
    ZoneList<PropertyDescriptor>* props = object_type->GetProperties();
    if (props != nullptr) {
      for (int i = 0; i < props->length(); i++) {
        if (strcmp(props->at(i).name, property_name) == 0) {
          if (props->at(i).is_readonly) {
            return true;
          }
          return false;
        }
      }
    }
  }

  if (object_type->kind() == TypeKind::kArray) {
    if (strcmp(property_name, "length") == 0) {
      return true;
    }
  }

  if (object_type->kind() == TypeKind::kString) {
    if (strcmp(property_name, "length") == 0) {
      return true;
    }
    if (strcmp(property_name, "charAt") == 0 ||
        strcmp(property_name, "charCodeAt") == 0) {
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// TSICOptimizer – Generate pre-compiled IC handler
// ---------------------------------------------------------------------------

void* TSICOptimizer::GenerateICHander(TSType* object_type,
                                       const char* property_name) {
  DCHECK_NOT_NULL(object_type);
  DCHECK_NOT_NULL(property_name);

  TypeKind kind = object_type->kind();

  if (kind == TypeKind::kAny || kind == TypeKind::kUnknown) {
    return nullptr;
  }

  if (object_type->HasKnownShape()) {
    ZoneList<PropertyDescriptor>* props = object_type->GetProperties();
    if (props != nullptr) {
      for (int i = 0; i < props->length(); i++) {
        if (strcmp(props->at(i).name, property_name) == 0) {
          PropertyDescriptor& prop = props->at(i);

          if (prop.is_readonly && prop.type != nullptr) {
            if (prop.type->IsPrimitive()) {
              return reinterpret_cast<void*>(static_cast<intptr_t>(0x1));
            }
          }

          if (prop.type != nullptr && prop.type->IsStable()) {
            return reinterpret_cast<void*>(static_cast<intptr_t>(0x2));
          }

          return reinterpret_cast<void*>(static_cast<intptr_t>(0x3));
        }
      }
    }
  }

  if (kind == TypeKind::kArray && strcmp(property_name, "length") == 0) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(0x4));
  }

  if (kind == TypeKind::kString && strcmp(property_name, "length") == 0) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(0x5));
  }

  if (kind == TypeKind::kNumber) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(0x6));
  }

  if (kind == TypeKind::kBoolean) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(0x7));
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// TSICOptimizer – Optimize feedback slot based on TS type stability
// ---------------------------------------------------------------------------

int TSICOptimizer::OptimizeFeedbackSlot(TSType* expected_type,
                                        int current_feedback) {
  DCHECK_NOT_NULL(expected_type);

  if (current_feedback <= 0) {
    return current_feedback;
  }

  if (expected_type->kind() == TypeKind::kAny ||
      expected_type->kind() == TypeKind::kUnknown) {
    return current_feedback;
  }

  if (expected_type->IsStable()) {
    return current_feedback;
  }

  if (expected_type->IsPrimitive()) {
    return current_feedback + 1;
  }

  if (expected_type->HasKnownShape()) {
    if (current_feedback < 3) {
      return current_feedback + 1;
    }
  }

  if (expected_type->IsUnion()) {
    ZoneList<TSType*>* members = expected_type->GetUnionMembers();
    if (members != nullptr) {
      int stable_count = 0;
      for (int i = 0; i < members->length(); i++) {
        if (members->at(i)->IsStable()) stable_count++;
      }
      if (stable_count == members->length()) {
        return current_feedback;
      }
    }
  }

  return current_feedback;
}

// ===========================================================================
// TSMapFactory – Zero-Cost Abstraction: Static Map Allocation & Direct
// Descriptor Access
// ===========================================================================

// ---------------------------------------------------------------------------
// TSMapFactory::GetOrCreateMapForType
// Retrieves a cached Map for a TS type, or creates + caches one if absent.
// This is the central hub for Map caching – the same TS type always gets
// the same V8 Map object, enabling object layout stability.
// ---------------------------------------------------------------------------

Handle<Map> TSMapFactory::GetOrCreateMapForType(TSType* type, Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  for (int i = 0; i < cached_maps_.length(); i++) {
    Handle<Map> cached = cached_maps_.at(i);
    TSMapMetadata* md = GetMetadata(cached);
    if (md != nullptr && md->ts_type == type) {
      return cached;
    }
  }

  Handle<Map> new_map = CreateMapFromType(type, zone);

  if (!new_map.is_null()) {
    cached_maps_.Add(new_map, zone);
  }

  return new_map;
}

// ---------------------------------------------------------------------------
// TSMapFactory::AllocateTypedObject
// Creates a JSObject with the pre-built Map for the given TS type.
// This bypasses CreateEmptyObjectLiteral + dynamic property additions.
// The object is born with its complete property layout.
// ---------------------------------------------------------------------------

Handle<JSObject> TSMapFactory::AllocateTypedObject(TSType* type,
                                                    Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  if (!type->HasKnownShape()) {
    return Handle<JSObject>();
  }

  Handle<Map> map = GetOrCreateMapForType(type, zone);
  if (map.is_null()) {
    return Handle<JSObject>();
  }

  return AllocateTypedObjectWithMap(map, zone);
}

// ---------------------------------------------------------------------------
// TSMapFactory::AllocateTypedObjectWithMap
// Low-level allocation: creates a JSObject from a pre-built Map.
// This calls Factory::NewJSObjectFromMap, which:
//   1. Allocates the object with the exact instance_size from the Map
//   2. Initializes all in-object property slots to the Map's initial value
//      (the filler or undefined)
//   3. Sets the Map pointer
// No IC transitions happen – the object has its final shape from birth.
// ---------------------------------------------------------------------------

Handle<JSObject> TSMapFactory::AllocateTypedObjectWithMap(Handle<Map> map,
                                                           Zone* zone) {
  DCHECK_NOT_NULL(map);

  int inobject_count = GetInObjectPropertyCount(map);
  if (inobject_count > 0) {
    SetInobjectProperties(map, inobject_count);
  }

  Handle<JSObject> obj = isolate_->factory()->NewJSObjectFromMap(map);
  return obj;
}

// ---------------------------------------------------------------------------
// TSMapFactory::GetInObjectPropertyCount
// Returns the number of in-object properties based on the Map's
// NumberOfOwnDescriptors. For TS types with known shapes, all properties
// are stored in-object (no out-of-object properties needed).
// ---------------------------------------------------------------------------

int TSMapFactory::GetInObjectPropertyCount(Handle<Map> map) {
  DCHECK_NOT_NULL(map);
  int own_descriptors = map->NumberOfOwnDescriptors();
  return own_descriptors;
}

// ---------------------------------------------------------------------------
// TSMapFactory::GetMapCacheIndex
// Returns a stable index for caching Maps by type. This allows the bytecode
// builder to reference a Map by index rather than embedding the full Map
// object, enabling compact bytecode and fast lookups.
// ---------------------------------------------------------------------------

int TSMapFactory::GetMapCacheIndex(TSType* type) {
  DCHECK_NOT_NULL(type);
  for (int i = 0; i < cached_maps_.length(); i++) {
    Handle<Map> cached = cached_maps_.at(i);
    TSMapMetadata* meta = GetMetadata(cached);
    if (meta != nullptr && meta->ts_type == type) {
      return i;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// TSMapFactory::FindPropertyIndex
// Searches the Map's DescriptorArray for a property by name.
// Returns the descriptor index (>=0) or -1 if not found.
// This bypasses V8's standard property lookup – we go straight to the
// descriptor array.
// ---------------------------------------------------------------------------

int TSMapFactory::FindPropertyIndex(Handle<Map> map,
                                     const char* property_name) {
  DCHECK_NOT_NULL(map);
  DCHECK_NOT_NULL(property_name);

  int own_descriptors = map->NumberOfOwnDescriptors();
  if (own_descriptors == 0) return -1;

  Tagged<DescriptorArray> descriptors =
      Cast<DescriptorArray>(map->instance_descriptors());

  Handle<String> search_name =
      isolate_->factory()->NewStringFromUtf8(base::StrVector(property_name))
          .ToHandleChecked();

  for (int i = 0; i < own_descriptors; i++) {
    InternalIndex idx(i);
    Tagged<Name> key = descriptors->GetKey(idx);
    if (key.IsEqual(*search_name)) {
      return i;
    }
  }

  return -1;
}

// ---------------------------------------------------------------------------
// TSMapFactory::GetPropertySlot
// Returns complete property slot information for direct field access.
// This is the bridge between TS type information and raw memory access:
// given a property name, we get the exact descriptor index, whether it's
// in-object or out-of-object, and its representation (Smi, Double, HeapObject).
// ---------------------------------------------------------------------------

TSPropertySlot TSMapFactory::GetPropertySlot(Handle<Map> map,
                                               const char* property_name) {
  DCHECK_NOT_NULL(map);
  DCHECK_NOT_NULL(property_name);

  TSPropertySlot slot;
  slot.descriptor_index = -1;
  slot.field_index = -1;
  slot.is_inobject = true;

  int index = FindPropertyIndex(map, property_name);
  if (index < 0) return slot;

  slot.descriptor_index = index;

  Tagged<DescriptorArray> descriptors =
      Cast<DescriptorArray>(map->instance_descriptors());
  InternalIndex idx(index);
  slot.details = descriptors->GetDetails(idx);
  slot.representation = slot.details.representation();

  int first_inobject =
      map->GetInObjectPropertiesStartInWords();
  int inobject_count = GetInObjectPropertyCount(map);

  if (index < inobject_count) {
    slot.is_inobject = true;
    slot.field_index = index;
  } else {
    slot.is_inobject = false;
    slot.field_index = index - inobject_count;
  }

  return slot;
}

// ---------------------------------------------------------------------------
// TSMapFactory::LoadFromDescriptor
// Direct field load bypassing IC. Uses the DescriptorArray to determine
// the field location and reads it directly.
//
// For Smi fields: returns the tagged Smi directly (no HeapNumber allocation)
// For Double fields: returns a HeapNumber (or Smi if small enough)
// For HeapObject fields: returns the tagged pointer
// ---------------------------------------------------------------------------

Handle<Object> TSMapFactory::LoadFromDescriptor(Handle<JSObject> obj,
                                                  int index,
                                                  bool is_inobject) {
  DCHECK_NOT_NULL(obj);

  Handle<Map> map = handle(obj->map(), isolate_);
  Tagged<DescriptorArray> descriptors =
      Cast<DescriptorArray>(map->instance_descriptors());
  InternalIndex idx(index);
  PropertyDetails details = descriptors->GetDetails(idx);
  Representation repr = details.representation();

  if (is_inobject) {
    int field_offset = JSObject::kHeaderSize + kTaggedSize * index;

    Tagged<Object> raw = obj->RawFieldAccess(field_offset);

    if (repr.IsSmi()) {
      if (IsSmi(raw)) {
        return handle(raw, isolate_);
      }
      if (IsHeapNumber(raw)) {
        return handle(raw, isolate_);
      }
      return isolate_->factory()->NewNumber(0);
    }

    if (repr.IsDouble()) {
      if (IsHeapNumber(raw)) {
        return handle(raw, isolate_);
      }
      if (IsSmi(raw)) {
        return handle(raw, isolate_);
      }
      return isolate_->factory()->NewNumber(0);
    }

    if (repr.IsHeapObject()) {
      if (IsHeapObject(raw)) {
        return handle(raw, isolate_);
      }
      return isolate_->factory()->undefined_value();
    }

    return handle(raw, isolate_);
  }

  int outobject_offset = -1;
  int inobject_count = GetInObjectPropertyCount(map);
  int outobject_index = index - inobject_count;

  Tagged<PropertyArray> properties = obj->property_array();
  Tagged<Object> raw =
      properties->get(outobject_index + PropertyArray::kHeaderSize / kTaggedSize);

  if (repr.IsSmi()) {
    if (IsSmi(raw)) return handle(raw, isolate_);
    return isolate_->factory()->NewNumber(0);
  }

  if (repr.IsHeapObject()) {
    if (IsHeapObject(raw)) return handle(raw, isolate_);
    return isolate_->factory()->undefined_value();
  }

  return handle(raw, isolate_);
}

// ---------------------------------------------------------------------------
// TSMapFactory::StoreToDescriptor
// Direct field store bypassing IC.
//
// For Smi fields: stores the value directly (must be Smi-compatible)
// For Double fields: converts and stores as HeapNumber or Double
// For HeapObject fields: stores the tagged pointer directly
// ---------------------------------------------------------------------------

void TSMapFactory::StoreToDescriptor(Handle<JSObject> obj,
                                      int index,
                                      bool is_inobject,
                                      Handle<Object> value) {
  DCHECK_NOT_NULL(obj);
  DCHECK_NOT_NULL(value);

  Handle<Map> map = handle(obj->map(), isolate_);
  Tagged<DescriptorArray> descriptors =
      Cast<DescriptorArray>(map->instance_descriptors());
  InternalIndex idx(index);
  PropertyDetails details = descriptors->GetDetails(idx);
  Representation repr = details.representation();

  if (is_inobject) {
    int field_offset = JSObject::kHeaderSize + kTaggedSize * index;

    if (repr.IsSmi()) {
      if (IsSmi(*value)) {
        obj->set_raw_field(field_offset, *value);
        return;
      }
      if (IsHeapNumber(*value)) {
        double num = HeapNumber::cast(*value).value();
        if (Smi::IsValid(num)) {
          obj->set_raw_field(field_offset, Smi::FromInt(
              static_cast<int>(num)));
          return;
        }
        Handle<Object> boxed = isolate_->factory()->NewNumber(num);
        obj->set_raw_field(field_offset, *boxed);
        return;
      }
      obj->set_raw_field(field_offset, Smi::FromInt(0));
      return;
    }

    if (repr.IsDouble()) {
      if (IsSmi(*value)) {
        Handle<Object> boxed =
            isolate_->factory()->NewNumber(Smi::ToInt(*value));
        obj->set_raw_field(field_offset, *boxed);
        return;
      }
      obj->set_raw_field(field_offset, *value);
      return;
    }

    if (repr.IsHeapObject()) {
      obj->set_raw_field(field_offset, *value);
      return;
    }

    obj->set_raw_field(field_offset, *value);
    return;
  }

  int inobject_count = GetInObjectPropertyCount(map);
  int outobject_index = index - inobject_count;

  Tagged<PropertyArray> properties = obj->property_array();

  if (repr.IsSmi()) {
    if (IsSmi(*value)) {
      properties->set(outobject_index, *value);
      return;
    }
    if (IsHeapNumber(*value)) {
      double num = HeapNumber::cast(*value).value();
      if (Smi::IsValid(num)) {
        properties->set(outobject_index,
                         Smi::FromInt(static_cast<int>(num)));
        return;
      }
      Handle<Object> boxed = isolate_->factory()->NewNumber(num);
      properties->set(outobject_index, *boxed);
      return;
    }
    properties->set(outobject_index, Smi::FromInt(0));
    return;
  }

  properties->set(outobject_index, *value);
}

// ---------------------------------------------------------------------------
// TSMapFactory::LoadTypedProperty
// High-level typed property load: find the property slot by name on the
// object's Map, then perform a direct descriptor load.
// This completely bypasses V8's named property accessor, the IC system,
// and any potential prototype chain walk.
// ---------------------------------------------------------------------------

Handle<Object> TSMapFactory::LoadTypedProperty(
    Handle<JSObject> obj, const char* property_name) {
  DCHECK_NOT_NULL(obj);
  DCHECK_NOT_NULL(property_name);

  Handle<Map> map = handle(obj->map(), isolate_);
  TSPropertySlot slot = GetPropertySlot(map, property_name);

  if (slot.descriptor_index < 0) {
    return isolate_->factory()->undefined_value();
  }

  return LoadFromDescriptor(obj, slot.descriptor_index, slot.is_inobject);
}

// ---------------------------------------------------------------------------
// TSMapFactory::StoreTypedProperty
// High-level typed property store: find the property slot by name on the
// object's Map, then perform a direct descriptor store.
// No IC transitions, no map checks, no property additions.
// ---------------------------------------------------------------------------

void TSMapFactory::StoreTypedProperty(Handle<JSObject> obj,
                                       const char* property_name,
                                       Handle<Object> value) {
  DCHECK_NOT_NULL(obj);
  DCHECK_NOT_NULL(property_name);
  DCHECK_NOT_NULL(value);

  Handle<Map> map = handle(obj->map(), isolate_);
  TSPropertySlot slot = GetPropertySlot(map, property_name);

  if (slot.descriptor_index < 0) {
    return;
  }

  StoreToDescriptor(obj, slot.descriptor_index, slot.is_inobject, value);
}

// ---------------------------------------------------------------------------
// TSMapFactory::HasFastPropertyPath
// Checks whether a property on a given TS type can be accessed via the
// fast descriptor path (i.e., the type has a known shape and the property
// is a defined own property). Returns true for zero-cost abstraction paths.
// ---------------------------------------------------------------------------

bool TSMapFactory::HasFastPropertyPath(TSType* type,
                                        const char* property_name) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(property_name);

  if (!type->HasKnownShape()) return false;

  ZoneList<PropertyDescriptor>* props = type->GetProperties();
  if (props == nullptr) return false;

  for (int i = 0; i < props->length(); i++) {
    if (strcmp(props->at(i).name, property_name) == 0) {
      return true;
    }
  }

  return false;
}

// ===========================================================================
// TSObjectAllocator – Type-Locked Object Allocator
// ===========================================================================

TSObjectAllocator::TSObjectAllocator(Isolate* isolate,
                                     TSMapFactory* map_factory)
    : isolate_(isolate), map_factory_(map_factory) {}

// ---------------------------------------------------------------------------
// TSObjectAllocator::CanAllocateInline
// Determines if a TS type can be allocated inline with its pre-built Map.
// Only types with known shapes (interfaces/classes with properties)
// qualify for the zero-cost abstraction path.
// ---------------------------------------------------------------------------

bool TSObjectAllocator::CanAllocateInline(TSType* type) const {
  if (type == nullptr) return false;
  if (!type->HasKnownShape()) return false;
  if (type->GetPropertyCount() == 0) return false;
  return true;
}

// ---------------------------------------------------------------------------
// TSObjectAllocator::Allocate
// Allocates a type-locked object: the object is born with its complete
// Map, all property slots are pre-sized, and zero runtime property
// lookups or IC transitions occur.
// ---------------------------------------------------------------------------

Handle<JSObject> TSObjectAllocator::Allocate(TSType* type,
                                               Zone* zone) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  if (!CanAllocateInline(type)) {
    return Handle<JSObject>();
  }

  return map_factory_->AllocateTypedObject(type, zone);
}

// ---------------------------------------------------------------------------
// TSObjectAllocator::AllocateWithMap
// Directly allocates an object using a pre-built Map. This is the ultimate
// zero-cost path: the Map was created at type-checking time, the object
// is allocated with exactly the right number of in-object properties, and
// no property additions or IC transitions ever occur.
// ---------------------------------------------------------------------------

Handle<JSObject> TSObjectAllocator::AllocateWithMap(Handle<Map> map,
                                                     Zone* zone) {
  DCHECK_NOT_NULL(map);
  DCHECK_NOT_NULL(zone);

  return map_factory_->AllocateTypedObjectWithMap(map, zone);
}

// ---------------------------------------------------------------------------
// TSObjectAllocator::AllocateWithValues
// Allocates a type-locked object and pre-fills property slots with initial
// values. The values must match the property order defined in the TS type.
// This enables `new User(name, age)` where the constructor body is
// completely eliminated by the compiler.
// ---------------------------------------------------------------------------

Handle<JSObject> TSObjectAllocator::AllocateWithValues(
    TSType* type, Zone* zone,
    ZoneList<Handle<Object>>* initial_values) {
  DCHECK_NOT_NULL(type);
  DCHECK_NOT_NULL(zone);

  if (!CanAllocateInline(type)) {
    return Handle<JSObject>();
  }

  if (initial_values == nullptr || initial_values->length() == 0) {
    return Allocate(type, zone);
  }

  Handle<JSObject> obj = map_factory_->AllocateTypedObject(type, zone);
  if (obj.is_null()) return obj;

  Handle<Map> map = handle(obj->map(), isolate_);
  int inobject_count = map_factory_->GetInObjectPropertyCount(map);

  int count = initial_values->length();
  if (count > inobject_count) count = inobject_count;

  for (int i = 0; i < count; i++) {
    Handle<Object> val = initial_values->at(i);
    map_factory_->StoreToDescriptor(obj, i, true, val);
  }

  return obj;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8