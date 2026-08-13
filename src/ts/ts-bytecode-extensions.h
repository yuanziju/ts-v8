#ifndef V8_TS_TS_BYTECODE_EXTENSIONS_H_
#define V8_TS_TS_BYTECODE_EXTENSIONS_H_

#include "src/ts/ts-type-system.h"
#include "src/interpreter/bytecode-generator.h"
#include "src/interpreter/bytecodes.h"

namespace v8 {
namespace internal {
namespace ts {

struct TSTypeHint {
  enum Flags : uint8_t {
    kNone = 0,
    kBoolean = 1 << 0,
    kString = 1 << 1,
    kNumber = 1 << 2,
    kObject = 1 << 3,
    kArray = 1 << 4,
    kFunction = 1 << 5,
    kUndefined = 1 << 6,
    kNull = 1 << 7,
    kAny = kBoolean | kString | kNumber | kObject | kArray | kFunction | kUndefined | kNull,
  };

  uint8_t flags = kNone;
  TSType* precise_type = nullptr;

  bool HasFlag(Flags flag) const { return (flags & flag) != 0; }
  bool IsBoolean() const { return HasFlag(kBoolean); }
  bool IsString() const { return HasFlag(kString); }
  bool IsNumber() const { return HasFlag(kNumber); }
  bool IsObject() const { return HasFlag(kObject); }
  bool IsArray() const { return HasFlag(kArray); }
  bool IsFunction() const { return HasFlag(kFunction); }
  bool IsNone() const { return flags == kNone && precise_type == nullptr; }
  bool IsPrecise() const { return precise_type != nullptr; }
};

enum class TSBytecodeStrategy : uint8_t {
  kDefault,
  kSkipTypeChecks,
  kSkipHoleChecks,
  kSpecializedPath,
  kFullCheck,
};

struct TSBytecodeConfig {
  TSBytecodeStrategy strategy = TSBytecodeStrategy::kSkipTypeChecks;
  bool skip_toboolean_conversion = true;
  bool skip_tonumber_conversion = true;
  bool skip_string_conversion = true;
  bool use_smi_fast_paths = true;
  bool skip_map_checks = true;
  bool inline_property_access = true;
};

class TSBytecodeBuilder {
 public:
  explicit TSBytecodeBuilder(BytecodeArrayBuilder* builder,
                              const TSBytecodeConfig& config);

  void LoadTypedVariable(const AstRawString* name, int feedback_slot,
                          TSType* expected_type);
  void LoadTypedProperty(Register object, const AstRawString* name,
                          int feedback_slot, TSType* expected_type);

  void StoreTypedVariable(const AstRawString* name, int feedback_slot,
                           TSType* value_type);
  void StoreTypedProperty(Register object, const AstRawString* name,
                           int feedback_slot, TSType* value_type);

  void BinaryOperationTyped(Token::Value op, Register reg,
                              int feedback_slot, TSType* operand_type);
  void CompareOperationTyped(Token::Value op, Register reg,
                              int feedback_slot, TSType* operand_type);

  void ReturnTyped(ToBooleanMode mode, TSType* return_type);
  void JumpIfTyped(BytecodeLabel* label, TSType* condition_type);
  void JumpIfFalseTyped(BytecodeLabel* label, TSType* condition_type);
  void JumpIfTrueTyped(BytecodeLabel* label, TSType* condition_type);

  void CreateTypedObject(TSType* object_type, int feedback_slot);
  void CreateTypedArray(TSType* element_type, int feedback_slot);
  void CreateTypedFunction(TSType* function_type, int feedback_slot);

  void SkipHoleCheck(Variable* variable);

  BytecodeArrayBuilder* builder() { return builder_; }
  const TSBytecodeConfig& config() const { return config_; }

 private:
  BytecodeArrayBuilder* builder_;
  TSBytecodeConfig config_;

  bool IsTypeStable(TSType* type) const;
  bool IsPrimitiveType(TSType* type) const;
  TSBytecodeStrategy SelectStrategy(TSType* type) const;
};

class TSBytecodeIntegrator {
 public:
  static void Initialize(BytecodeGenerator* generator,
                          TSTypeSystem* type_system,
                          const TSBytecodeConfig& config);

  static bool TrySpecializeBinaryOp(BytecodeGenerator* generator,
                                      BinaryOperation* node,
                                      TSTypeHint* hint);

  static bool TrySpecializePropertyLoad(BytecodeGenerator* generator,
                                          Property* node,
                                          TSTypeHint* hint);

  static bool TrySkipHoleCheck(BytecodeGenerator* generator,
                                VariableDeclaration* node);

  static bool TrySkipReturnTypeCheck(BytecodeGenerator* generator,
                                      ReturnStatement* node);
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_BYTECODE_EXTENSIONS_H_