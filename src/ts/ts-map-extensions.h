#ifndef V8_TS_TS_MAP_EXTENSIONS_H_
#define V8_TS_TS_MAP_EXTENSIONS_H_

#include "src/ts/ts-type-system.h"
#include "src/objects/map.h"
#include "src/objects/descriptor-array.h"
#include "src/objects/field-type.h"

namespace v8 {
namespace internal {

class Isolate;
class JSObject;

namespace ts {

struct TSMapMetadata {
  TSType* ts_type = nullptr;
  bool is_stable_by_ts = false;
  ZoneList<FieldType*>* field_types = nullptr;
  int expected_property_count = 0;
  bool is_pre_allocated = false;
  int creation_order = 0;
};

struct TSPropertySlot {
  int descriptor_index = -1;
  int field_index = -1;
  bool is_inobject = true;
  PropertyDetails details;
  Representation representation;
};

class TSFieldType : public ZoneObject {
 public:
  enum Kind : uint8_t {
    kNone,
    kClass,
    kAny,
    kTSPrimitive,
    kTSInterface,
    kTSUnion,
    kTSLiteral,
    kStable,
  };

  static TSFieldType* None(Zone* zone);
  static TSFieldType* Class(Zone* zone, Handle<Map> map);
  static TSFieldType* Any(Zone* zone);
  static TSFieldType* TSPrimitive(Zone* zone, TSType* type);
  static TSFieldType* TSInterface(Zone* zone, TSType* type);
  static TSFieldType* TSUnion(Zone* zone, ZoneList<TSType*>* members);
  static TSFieldType* TSLiteral(Zone* zone, TSType* type);
  static TSFieldType* Stable(Zone* zone, TSType* type);

  Kind kind() const { return kind_; }
  TSType* ts_type() const { return ts_type_; }
  Handle<Map> map() const { return map_; }

  bool IsNone() const { return kind_ == kNone; }
  bool IsAny() const { return kind_ == kAny; }
  bool IsStable() const { return kind_ == kStable; }
  bool HasTSInfo() const { return ts_type_ != nullptr; }

  bool NowContains(Object* value) const;
  TSFieldType* Merge(Zone* zone, TSFieldType* other);
  TSFieldType* Promote(Zone* zone);
  Tagged<FieldType> ToV8FieldType() const;
  bool IsGuaranteedStable() const;

 private:
  explicit TSFieldType(Kind kind) : kind_(kind) {}

  Kind kind_;
  TSType* ts_type_ = nullptr;
  Handle<Map> map_;
  ZoneList<TSType*>* union_members_ = nullptr;
};

class TSMapFactory {
 public:
  explicit TSMapFactory(Isolate* isolate);

  Handle<Map> CreateMapFromType(TSType* type, Zone* zone);
  ZoneList<Handle<Map>>* CreateMapsForType(TSType* type, Zone* zone);
  Handle<Map> CreateStableMap(TSType* type, Zone* zone);

  void AttachMetadata(Handle<Map> map, TSMapMetadata* metadata);
  TSMapMetadata* GetMetadata(Handle<Map> map);

  Handle<Map> CreateSpecializedTransition(Handle<Map> from_map,
                                           TSType* new_property_type,
                                           const char* property_name,
                                           Zone* zone);

  void PreAllocateMaps(Zone* zone);
  Handle<Map> GetPrimitiveMap(TSType* type, Zone* zone);

  ZoneList<Handle<Map>>* cached_maps() { return &cached_maps_; }

  Handle<Map> GetOrCreateMapForType(TSType* type, Zone* zone);

  Handle<JSObject> AllocateTypedObject(TSType* type, Zone* zone);

  Handle<JSObject> AllocateTypedObjectWithMap(Handle<Map> map, Zone* zone);

  int FindPropertyIndex(Handle<Map> map, const char* property_name);

  TSPropertySlot GetPropertySlot(Handle<Map> map, const char* property_name);

  Handle<Object> LoadFromDescriptor(Handle<JSObject> obj, int index,
                                     bool is_inobject);

  void StoreToDescriptor(Handle<JSObject> obj, int index, bool is_inobject,
                          Handle<Object> value);

  Handle<Object> LoadTypedProperty(Handle<JSObject> obj,
                                    const char* property_name);

  void StoreTypedProperty(Handle<JSObject> obj, const char* property_name,
                           Handle<Object> value);

  bool HasFastPropertyPath(TSType* type, const char* property_name);

  int GetInObjectPropertyCount(Handle<Map> map);

  int GetMapCacheIndex(TSType* type);

  Isolate* isolate() const { return isolate_; }

 private:
  Isolate* isolate_;
  ZoneList<Handle<Map>> cached_maps_;

  Handle<Map> CreateEmptyMap();
  Handle<Map> CreateMapWithDescriptors(ZoneList<PropertyDescriptor>* props,
                                       Zone* zone);
  void SetInobjectProperties(Handle<Map> map, int count);
  void SetFieldsAsReadonly(Handle<Map> map);

  static constexpr int kTSTypeMetadataBit = 28;

  ZoneList<TSMapMetadata*>* metadata_table_ = nullptr;
};

class TSObjectAllocator {
 public:
  explicit TSObjectAllocator(Isolate* isolate, TSMapFactory* map_factory);

  Handle<JSObject> Allocate(TSType* type, Zone* zone);

  Handle<JSObject> AllocateWithMap(Handle<Map> map, Zone* zone);

  Handle<JSObject> AllocateWithValues(
      TSType* type, Zone* zone,
      ZoneList<Handle<Object>>* initial_values);

  bool CanAllocateInline(TSType* type) const;

 private:
  Isolate* isolate_;
  TSMapFactory* map_factory_;
};

class TSICOptimizer {
 public:
  explicit TSICOptimizer(Isolate* isolate);

  int GetICKindForProperty(TSType* object_type, const char* property_name);
  bool CanSkipIC(TSType* object_type, const char* property_name);
  void* GenerateICHander(TSType* object_type, const char* property_name);
  int OptimizeFeedbackSlot(TSType* expected_type, int current_feedback);

 private:
  Isolate* isolate_;
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_MAP_EXTENSIONS_H_