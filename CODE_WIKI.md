# TS-V8 Code Wiki

> TypeScript 原生支持 V8 引擎 — 从解析到 JIT 的全链路类型系统集成

---

## 目录

1. [项目概述](#1-项目概述)
2. [整体架构](#2-整体架构)
3. [目录结构](#3-目录结构)
4. [编译流水线](#4-编译流水线)
5. [主要模块职责](#5-主要模块职责)
   - 5.1 [解析器模块 (Parser)](#51-解析器模块-parser)
   - 5.2 [类型系统模块 (Type System)](#52-类型系统模块-type-system)
   - 5.3 [类型检查器模块 (Type Checker)](#53-类型检查器模块-type-checker)
   - 5.4 [编译流水线模块 (Pipeline)](#54-编译流水线模块-pipeline)
   - 5.5 [字节码扩展模块 (Bytecode Extensions)](#55-字节码扩展模块-bytecode-extensions)
   - 5.6 [Map 扩展模块 (Map Extensions)](#56-map-扩展模块-map-extensions)
   - 5.7 [JIT 集成模块 (JIT Integration)](#57-jit-集成模块-jit-integration)
   - 5.8 [构建集成模块 (Build Integration)](#58-构建集成模块-build-integration)
   - 5.9 [基准测试模块 (Benchmark Suite)](#59-基准测试模块-benchmark-suite)
   - 5.10 [入口点模块 (Entry Point)](#510-入口点模块-entry-point)
6. [关键类与函数说明](#6-关键类与函数说明)
7. [依赖关系](#7-依赖关系)
8. [构建与运行方式](#8-构建与运行方式)
9. [配置说明](#9-配置说明)
10. [设计决策摘要](#10-设计决策摘要)
11. [术语表](#11-术语表)

---

## 1. 项目概述

**TS-V8** 是 V8 引擎内部的 TypeScript 原生支持模块。与传统的 "TypeScript → JavaScript 转译 → V8 执行" 流程不同，TS-V8 将 TypeScript 的类型系统直接集成到 V8 的每一层：

- **解析层** — 识别 TypeScript 语法（接口、泛型、联合类型等）
- **AST 层** — 为 AST 节点附加类型注解
- **字节码层** — 基于类型信息跳过冗余检查、生成特化指令
- **Hidden Class (Map) 层** — 基于 TS 类型元数据创建稳定的 Map
- **JIT 层** — 利用 TS 类型信息指导 TurboFan/Maglev 做更激进的优化

### 核心目标

| 目标 | 描述 |
| --- | --- |
| 性能提升 | 利用类型信息消除运行时检查，生成更特化的机器码 |
| 内存节省 | 预分配稳定 Map，减少 IC (Inline Cache) 的状态迁移 |
| 类型安全 | 在编译期捕获错误，避免运行时崩溃 |
| 零转译 | TypeScript 源码直接进入 V8 流水线，无需 `tsc` 转译 |

---

## 2. 整体架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        TypeScript Source (.ts)                           │
└──────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                          TSParser (ts-parser)                            │
│  · 解析 TS 语法: interface / class / enum / generic / union / intersection │
│  · 生成带类型注解的 AST                                                  │
└──────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     TSTypeChecker (ts-type-checker)                      │
│  · 表达式类型推断                                                        │
│  · 赋值兼容性检查                                                        │
│  · 函数返回值 / 参数类型校验                                             │
└──────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                       TSPipeline (ts-pipeline)                            │
│  · 汇总类型信息 → JIT 所需元数据                                         │
│  · 生成 MapCreationHint / TypeInfoForJIT                                 │
└──────────────────────────────────────────────────────────────────────────┘
                      │                     │                │
                      ▼                     ▼                ▼
          ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
          │ TSBytecodeBuilder │  │   TSMapFactory    │  │  TSToTurboFanBridge│
          │ 字节码特化/跳过检查 │  │  稳定 Map 创建     │  │  类型 → TurboFan   │
          └──────────────────┘  └──────────────────┘  └──────────────────┘
                      │                     │                │
                      ▼                     ▼                ▼
              解释器 (Interpreter)  IC 层 (Maps)    TurboFan / Maglev JIT
```

### 架构分层

| 层 | 关键文件 | 关键产物 |
| --- | --- | --- |
| 语法层 | `ts-parser.{h,cc}` | 带类型注解的 AST |
| 类型层 | `ts-type-system.{h,cc}` | `TSType` 对象图 |
| 检查层 | `ts-type-checker.{h,cc}` | 类型错误报告 |
| 流水线层 | `ts-pipeline.{h,cc}` | `TypeInfoForJIT` / `MapCreationHint` |
| 字节码层 | `ts-bytecode-extensions.{h,cc}` | 特化的 BytecodeArray |
| Map 层 | `ts-map-extensions.{h,cc}` | 稳定的 `Map` 对象 |
| JIT 层 | `ts-jit-integration.{h,cc}` | 带类型约束的 TurboFan/Maglev 图 |

---

## 3. 目录结构

```
/workspace
├── DESIGN.md                          # 架构设计文档（高层设计说明）
├── CODE_WIKI.md                       # 本文档：结构化 Code Wiki
├── scripts/
│   └── tsv8-demo.sh                   # 演示脚本（构建 + 运行示例）
└── src/
    └── ts/
        ├── BUILD.gn                   # GN 构建配置（source_set 依赖声明）
        ├── ts-parser.h / .cc          # TS 语法解析器
        ├── ts-type-system.h / .cc      # TS 类型系统（TSType、子类型、联合/交叉）
        ├── ts-type-checker.h / .cc    # TS 类型检查器
        ├── ts-pipeline.h / .cc        # 编译流水线（编排各阶段）
        ├── ts-bytecode-extensions.h / .cc  # 字节码扩展（特化、跳过检查）
        ├── ts-map-extensions.h / .cc  # Map 扩展（稳定 Map、IC 优化）
        ├── ts-jit-integration.h / .cc # JIT 集成（TurboFan/Maglev 桥接）
        ├── ts-build-integration.h / .cc # V8 构建集成入口
        ├── ts-benchmark-suite.h / .cc # 基准测试套件
        └── ts-entry-point.cc          # tsv8 可执行文件入口 (main)
```

---

## 4. 编译流水线

TS-V8 的一次完整编译由 `TSPipeline::Compile()` 编排，经历以下阶段：

| 阶段 | 方法 | 说明 |
| --- | --- | --- |
| 1. 检测 | `DetectTypeScript()` | 根据文件后缀 / 源码特征判断是否为 TS |
| 2. 解析 | `ParseStage()` | 调用 `TSParser` 生成带类型注解的 AST |
| 3. 类型检查 | `TypeCheckStage()` | 调用 `TSTypeChecker` 进行静态类型检查 |
| 4. AST 注解 | `AnnotateASTStage()` | 将类型信息标注到 AST 节点 |
| 5. 字节码生成 | `BytecodeGenerationStage()` | 使用 `TSBytecodeBuilder` 生成特化字节码 |
| 6. 元数据输出 | `GetTypeInfoForJIT()` / `GetMapHints()` | 生成 JIT 和 Map 所需提示信息 |

### 检测逻辑

- 文件后缀 `.ts` / `.tsx` → 自动启用 TS 模式
- 对 `.js` 文件可通过 `--force-ts` 强制启用
- `HasTypeScriptSyntax()` 通过扫描 `interface`、`:` 类型注解、`as` 表达式等关键字做启发式识别

---

## 5. 主要模块职责

### 5.1 解析器模块 (Parser)

**文件**：`src/ts/ts-parser.h` / `ts-parser.cc`

**职责**：扩展 V8 原生 `Parser`，识别 TypeScript 特有的语法结构并构建带类型注解的 AST。

#### 关键类：`TSParser`

继承自 V8 的 `Parser`（位于 `src/parsing/parser.h`）。

| 方法 | 说明 |
| --- | --- |
| `ParseTypeAnnotation()` | 解析类型注解表达式（`: number`、`: string` 等） |
| `ParseUnionType()` | 解析联合类型 `A \| B` |
| `ParseIntersectionType()` | 解析交叉类型 `A & B` |
| `ParsePrimaryType()` | 解析基本类型标识符 |
| `ParseInterfaceDeclaration()` | 解析 `interface Foo { ... }` 声明 |
| `ParseEnumDeclaration()` | 解析 `enum Color { ... }` 声明 |
| `ParseClassDeclaration()` | 解析带 TS 特性的类（访问修饰符、implements） |
| `ParseTypeParameterDeclaration()` | 解析泛型参数 `<T extends ...>` |
| `ParseGenericCallExpression()` | 解析泛型调用 `foo<T>(...)` |
| `ParseAsExpression()` | 解析类型断言 `x as T` |
| `ParseSatisfiesExpression()` | 解析 `x satisfies T` |

#### 关键设计

- 复用 V8 原生 lexer/tokenizer，仅扩展 TS 相关 token 语义
- 解析过程中即时创建 `TSType` 对象并写入 `TSTypeSystem`
- 出错时通过 V8 的 `error_handler()` 报告

---

### 5.2 类型系统模块 (Type System)

**文件**：`src/ts/ts-type-system.h` / `ts-type-system.cc`

**职责**：定义 TypeScript 类型的内存表示、子类型关系、联合/交叉类型构造。

#### 关键枚举：`TypeKind`

```
kUndefined / kNull / kBoolean / kNumber / kString / kSymbol / kBigInt
kAny / kUnknown / kNever / kVoid
kObject / kArray / kTuple / kFunction
kInterface / kClass / kEnum
kUnion / kIntersection / kLiteral / kMapped / kConditional
```

#### 关键类：`TSType`

表示任意 TypeScript 类型的不可变（逻辑上）节点。

| 字段 / 方法 | 说明 |
| --- | --- |
| `kind_` (`TypeKind`) | 类型的分类标签 |
| `properties_` (`ZoneList<PropertyDescriptor>*`) | 对象/接口的属性表 |
| `element_type_` (`TSType*`) | 数组元素类型 |
| `return_type_` / `param_types_` | 函数签名 |
| `union_members_` / `intersection_members_` | 联合/交叉成员 |
| `IsSubtypeOf(other)` | 子类型判断（实现 TS 的结构类型规则） |
| `IsAssignableTo(other)` | 赋值兼容性（含 `any`、`never` 等特殊规则） |
| `IsPrimitive()` / `IsObject()` / `IsUnion()` 等 | 类型谓词 |
| `ToString()` | 调试用的人类可读表示 |

#### 关键类：`PropertyDescriptor`

描述对象类型的单个属性。

| 字段 | 说明 |
| --- | --- |
| `name` | 属性名 |
| `type` (`TSType*`) | 属性类型 |
| `readonly` | 是否只读 |
| `optional` | 是否可选（`?:`） |

#### 关键类：`TSTypeSystem`

类型工厂与全局注册表。

| 方法 | 说明 |
| --- | --- |
| `NewPrimitive(kind)` | 创建基本类型单例 |
| `NewObject(props)` | 创建对象类型 |
| `NewArray(elem_type)` | 创建数组类型 |
| `NewUnion(members)` | 创建联合类型（自动扁平化） |
| `NewIntersection(members)` | 创建交叉类型 |
| `NewFunction(ret, params)` | 创建函数类型 |
| `NewInterface(name, props)` | 创建命名接口类型 |
| `NewClass(name, props, bases)` | 创建类类型 |
| `NewLiteral(value)` | 创建字面量类型 |
| `GetCommonSupertype(a, b)` | 计算公共父类型 |
| `FlattenUnion(type)` | 扁平化嵌套联合 |

---

### 5.3 类型检查器模块 (Type Checker)

**文件**：`src/ts/ts-type-checker.h` / `ts-type-checker.cc`

**职责**：遍历带类型注解的 AST，执行类型推断与类型兼容性校验，报告类型错误。

#### 关键类：`TSTypeChecker`

| 方法 | 说明 |
| --- | --- |
| `CheckProgram(program)` | 检查整个程序（顶层入口） |
| `CheckFunction(func)` | 检查函数体，推断/验证返回类型 |
| `CheckExpression(expr)` | 表达式类型推断 |
| `CheckAssignment(target, source)` | 赋值兼容性校验 |
| `CheckCall(func, args)` | 函数调用检查（参数个数、类型） |
| `CheckPropertyAccess(obj, name)` | 属性访问检查（含可选链） |
| `CheckBinaryOp(op, left, right)` | 二元运算类型检查 |
| `CheckComparison(op, left, right)` | 比较运算检查 |
| `CheckReturnStatement(ret, expected)` | 返回语句校验 |
| `CheckVariableDeclaration(var)` | 变量声明检查（含 `const` / 可选类型） |

#### 类型推断规则示例

- 字面量 `42` → 初始推断为 `kNumber`（在严格模式下可能收窄为 `kLiteral(42)`）
- `[]` → `kArray<kNever>`
- `[1, 2]` → `kArray<kNumber>`
- `x + y` 其中 `x: string, y: number` → `kString`（遵循 TS 宽松规则）

#### 错误报告

- 通过 V8 原生化 `TypeError` / `SyntaxError` 风格的错误消息
- 错误信息以 `filename:line:col` 形式定位
- 错误汇总写入 `error_count_`，用于基准测试

---

### 5.4 编译流水线模块 (Pipeline)

**文件**：`src/ts/ts-pipeline.h` / `ts-pipeline.cc`

**职责**：编排 TS-V8 的各编译阶段，输出供解释器 / JIT / Map 层消费的元数据。

#### 关键结构：`TSCompilationConfig`

```cpp
struct TSCompilationConfig {
  bool is_typescript       = false;   // 当前文件是否按 TS 解析
  bool type_check          = true;    // 是否执行类型检查
  bool strict_mode         = true;    // 是否启用严格模式
  bool check_nulls         = true;    // 启用 null 检查
  bool check_implicit_any  = false;   // 是否禁止隐式 any
  bool skip_type_erasure   = false;  // 跳过类型擦除（保留注解用于 JIT）
  bool generate_runtime_checks = false; // 是否生成运行时类型断言
  bool trust_types         = true;    // 是否信任类型注解（不额外做运行时检查）
};
```

#### 关键类：`TSPipeline`

| 方法 | 说明 |
| --- | --- |
| `Initialize(ParseInfo*)` | 绑定到一次 V8 解析上下文 |
| `Compile(isolate, info, script, outer_scope)` | 执行完整流水线 |
| `GetTypeInfoForJIT(zone)` | 产出 JIT 编译所需变量/参数/返回类型 |
| `GetMapHints(zone)` | 产出 Map 创建提示 |
| `IsTypeScriptFile(filename)` | 静态判断文件后缀 |
| `HasTypeScriptSyntax(source, len)` | 静态启发式 TS 语法检测 |

#### 内部子阶段

- `ParseStage` → `TypeCheckStage` → `AnnotateASTStage` → `BytecodeGenerationStage`
- `CollectTypeAnnotations` / `PropagateTypes` 递归遍历 AST 完成类型信息收集

#### 关键结构：`TypeInfoForJIT`

```cpp
struct TypeInfoForJIT {
  ZoneList<std::pair<int, TSType*>>* variable_types;   // 变量槽位 → 类型
  ZoneList<std::pair<int, TSType*>>* parameter_types; // 参数索引 → 类型
  TSType* return_type;
  bool types_are_stable;                               // 类型是否稳定（可跳过检查）
};
```

#### 关键结构：`MapCreationHint`

```cpp
struct MapCreationHint {
  const char* name;
  ZoneList<PropertyDescriptor>* properties;
  bool is_stable;
  int expected_inobject_properties;
};
```

---

### 5.5 字节码扩展模块 (Bytecode Extensions)

**文件**：`src/ts/ts-bytecode-extensions.h` / `ts-bytecode-extensions.cc`

**职责**：在 V8 的 `BytecodeGenerator` 基础上，基于 TS 类型信息生成特化的字节码、跳过冗余检查。

#### 关键结构：`TSTypeHint`

为每条字节码附加的类型提示（位 flag + 精确类型指针）。

| Flag | 说明 |
| --- | --- |
| `kBoolean / kString / kNumber` | 基本类型标记 |
| `kObject / kArray / kFunction` | 对象类标记 |
| `kUndefined / kNull` | null-ish 标记 |
| `kAny` | 全 flag 组合（等价于无提示） |

#### 关键结构：`TSBytecodeConfig`

```cpp
struct TSBytecodeConfig {
  TSBytecodeStrategy strategy;    // kSkipTypeChecks / kSkipHoleChecks / ...
  bool skip_toboolean_conversion;
  bool skip_tonumber_conversion;
  bool skip_string_conversion;
  bool use_smi_fast_paths;
  bool skip_map_checks;
  bool inline_property_access;
};
```

#### 关键类：`TSBytecodeBuilder`

对 `BytecodeArrayBuilder` 的 TS 特化封装。

| 方法 | 说明 |
| --- | --- |
| `LoadTypedVariable(name, slot, expected)` | 带类型提示的变量加载 |
| `LoadTypedProperty(obj, name, slot, expected)` | 带类型提示的属性加载 |
| `StoreTypedVariable(...)` | 带类型提示的变量存储 |
| `StoreTypedProperty(...)` | 带类型提示的属性存储 |
| `BinaryOperationTyped(op, reg, slot, operand)` | 特化二元运算（如 number+number → Int32Add） |
| `CompareOperationTyped(op, reg, slot, operand)` | 特化比较 |
| `ReturnTyped(mode, return_type)` | 带返回类型的 return |
| `JumpIfTyped(label, cond_type)` | 带类型条件的跳转 |
| `CreateTypedObject(object_type, slot)` | 基于类型创建对象（触发 MapFactory） |
| `CreateTypedArray(elem_type, slot)` | 基于元素类型创建数组 |
| `CreateTypedFunction(function_type, slot)` | 基于签名创建函数 |
| `SkipHoleCheck(variable)` | 对已声明变量跳过 hole 检查 |

#### 关键类：`TSBytecodeIntegrator`

静态方法集合，以插件形式介入 `BytecodeGenerator`。

| 方法 | 说明 |
| --- | --- |
| `Initialize(generator, type_system, config)` | 挂接到生成器 |
| `TrySpecializeBinaryOp(generator, node, hint)` | 尝试将二元运算替换为特化版本 |
| `TrySpecializePropertyLoad(generator, node, hint)` | 尝试内联属性加载 |
| `TrySkipHoleCheck(generator, node)` | 对 `const` 变量跳过 hole 检查 |
| `TrySkipReturnTypeCheck(generator, node)` | 对有显式返回类型的函数跳过运行时检查 |

---

### 5.6 Map 扩展模块 (Map Extensions)

**文件**：`src/ts/ts-map-extensions.h` / `ts-map-extensions.cc`

**职责**：基于 TS 类型元数据预创建稳定的 V8 `Map`（Hidden Class），并优化 IC (Inline Cache)。

#### 关键结构：`TSMapMetadata`

附加在 V8 `Map` 上的元数据（通过 `kTSTypeMetadataBit = 28` 的 bit 位在 `Map::bit_field3` 中标记）。

```cpp
struct TSMapMetadata {
  TSType* ts_type;
  bool is_stable_by_ts;
  ZoneList<FieldType*>* field_types;
  int expected_property_count;
  bool is_pre_allocated;
  int creation_order;
};
```

#### 关键类：`TSFieldType`

扩展 V8 的 `FieldType`，带有 TS 特有的种类。

| 种类 | 说明 |
| --- | --- |
| `kNone` | 未知字段类型 |
| `kClass` | 指向具体 Map 的类字段 |
| `kAny` | 等价于动态类型 |
| `kTSPrimitive` | TS 基本类型 |
| `kTSInterface` | TS 接口 |
| `kTSUnion` | TS 联合类型 |
| `kTSLiteral` | TS 字面量类型 |
| `kStable` | TS 保证的稳定字段 |

| 方法 | 说明 |
| --- | --- |
| `NowContains(value)` | 判断值是否满足该字段类型 |
| `Merge(zone, other)` | 合并两种字段类型（用于 IC 反馈泛化） |
| `Promote(zone)` | 向更通用的类型提升 |
| `ToV8FieldType()` | 转换为 V8 原生 `FieldType` |
| `IsGuaranteedStable()` | TS 是否保证该字段类型稳定 |

#### 关键类：`TSMapFactory`

| 方法 | 说明 |
| --- | --- |
| `CreateMapFromType(type, zone)` | 从 TS 类型创建对应 Map |
| `CreateMapsForType(type, zone)` | 为联合类型创建 Map 集合 |
| `CreateStableMap(type, zone)` | 创建带稳定 bit 的 Map |
| `AttachMetadata(map, metadata)` | 挂载 TS 元数据到 Map |
| `GetMetadata(map)` | 读取 Map 的 TS 元数据 |
| `CreateSpecializedTransition(from_map, new_prop_type, prop_name, zone)` | 创建带 TS 特化的 transition |
| `PreAllocateMaps(zone)` | 预分配常用 Map（启动阶段） |
| `GetPrimitiveMap(type, zone)` | 获取某基本类型的规范 Map |

#### 关键类：`TSICOptimizer`

| 方法 | 说明 |
| --- | --- |
| `GetICKindForProperty(object_type, prop_name)` | 根据 TS 类型选择 IC 类型 |
| `CanSkipIC(object_type, prop_name)` | TS 类型稳定时可跳过 IC |
| `GenerateICHander(object_type, prop_name)` | 基于 TS 类型生成特化的 IC handler |
| `OptimizeFeedbackSlot(expected_type, current_feedback)` | 基于期望类型优化反馈槽 |

---

### 5.7 JIT 集成模块 (JIT Integration)

**文件**：`src/ts/ts-jit-integration.h` / `ts-jit-integration.cc`

**职责**：将 TS 类型信息注入 TurboFan / Maglev 编译器，实现类型驱动的特化优化。

#### 关键结构：`TypeInfoForJIT`

```cpp
struct TypeInfoForJIT {
  TSType* return_type;
  ZoneList<TSType*>* param_types;
  TSType* this_type;
  bool has_explicit_return_type;
  bool has_explicit_param_types;
  bool is_strict;
  bool should_skip_type_checks;
};
```

#### 关键类：`TSToTurboFanBridge`

TS 类型 → TurboFan `compiler::Type` 的转换桥。

| 方法 | 说明 |
| --- | --- |
| `Convert(ts_type)` | 顶层转换入口（带缓存） |
| `ConvertFunctionSignature(return, params)` | 转换函数签名 |
| `ApplyTypeConstraints(graph, function_node, info)` | 向 TF 图注入类型约束 |
| `PreTypeGraph(graph, info)` | 在 TyperPhase 之前预着色节点类型 |
| `ShouldSkipSpeculativeType(ts_type)` | TS 保证类型稳定时跳过推测 |
| `ConvertPrimitive / ConvertObject / ConvertArray / ConvertFunction / ConvertUnion / ConvertIntersection / ConvertLiteral` | 各类型子类转换 |
| `CreateTypeAnchor(graph, node, type)` | 创建类型锚点，用于防止类型漂移 |
| `RemoveTypeChecksForNode(graph, node, guaranteed)` | 移除已保证类型的节点的检查 |

#### 关键类：`TSTurboFanIntegration`

在 TurboFan 流水线特定阶段注入 TS 信息。

| 方法 | 说明 |
| --- | --- |
| `BeforeTyperPhase(pipeline, type_system, zone)` | 类型着色前置 |
| `AfterTyperPhase(pipeline, type_system, zone)` | 类型细化后置 |
| `DuringGraphBuild(pipeline, info, zone)` | 构图期间注入 TS 类型 |
| `ShouldUseTSOptimization(function)` | 判断函数是否启用 TS 优化 |

#### 关键类：`TSMaglevIntegration`

Maglev 编译器对应集成。

| 方法 | 说明 |
| --- | --- |
| `BeforeGraphBuild(info, type_system, zone)` | Maglev 构图前置 |
| `DuringGraphBuild(info, info, zone)` | Maglev 构图期间注入类型 |
| `OptimizePhiSelection(info, zone)` | 基于 TS 类型优化 Phi 选择 |
| `ShouldSkipMapCheck(object_type)` | TS 保证稳定时跳过 Map 检查 |
| `ShouldSkipNumberCheck(value_type)` | TS 保证 number 时跳过检查 |
| `ShouldSkipBooleanCheck(value_type)` | TS 保证 boolean 时跳过检查 |

#### 关键类：`TSRepresentationSelector`

基于 TS 类型选择最优机器表示。

| 方法 | 说明 |
| --- | --- |
| `SelectRepresentation(ts_type)` | 选择机器表示（Smi / HeapNumber / Word32 等） |
| `CanBeSmi / CanBeHeapNumber / CanBeWord32` | 类型表示可行性判断 |
| `GetBestRepresentation(ts_type)` | 返回最优表示 |

#### 独立函数

- `TSTypeToCompilerType(broker, ts_type, zone)` — 便捷转换入口

---

### 5.8 构建集成模块 (Build Integration)

**文件**：`src/ts/ts-build-integration.h` / `ts-build-integration.cc`

**职责**：将 TS-V8 挂入 V8 的原生解析流程，是 V8 与 TS 模块的桥梁。

#### 关键类：`TSBuildIntegration`

所有方法均为 `static`，通过 V8 的解析命名空间 `v8::internal::parsing` 调用。

| 方法 | 说明 |
| --- | --- |
| `Initialize(isolate)` | 在 V8 启动时初始化 TS-V8 |
| `ShouldUseTSV8(info)` | 对 `ParseInfo` 做 TS 识别判断 |
| `ParseProgram(info, script, outer_scope, isolate, mode)` | 接管 Program 解析 |
| `ParseFunction(info, shared_info, isolate, mode)` | 接管 Function 解析 |
| `Enable(enabled)` / `IsEnabled()` | 全局开关 |
| `SetConfig(config)` / `GetConfig()` | 全局编译配置 |
| `ForceTSMode(force)` | 强制将下一次解析识别为 TS |

#### 关键内部命名空间：`v8::internal::ts::parsing`

提供与 V8 `parsing::ParseProgram / ParseFunction` 同名的入口，实现透明接管。

---

### 5.9 基准测试模块 (Benchmark Suite)

**文件**：`src/ts/ts-benchmark-suite.h` / `ts-benchmark-suite.cc`

**职责**：预置一组 TS 用例，覆盖正确性验证与性能对比（TS-V8 vs 传统 V8）。

#### 关键结构：`BenchmarkCase`

| 字段 | 说明 |
| --- | --- |
| `name` / `description` | 用例名称与描述 |
| `source_code` | TS 源码 |
| `expected_output` / `expected_stdout` | 期望输出 |
| `should_pass` | 期望通过 |
| `expected_error_count` | 期望错误数 |
| `expected_min_ops` / `expected_max_ops` | 期望字节码操作数范围 |
| `extension` | 文件扩展名（默认 `.ts`） |

#### 关键结构：`BenchmarkResult`

运行后填充的结果记录，含编译/类型检查/执行耗时。

#### 关键类：`TSBenchmarkSuite`

| 方法 | 说明 |
| --- | --- |
| `AddCase(test_case)` | 注册单用例 |
| `AddAllBuiltInCases()` | 注册所有内建用例 |
| `RunAll()` | 运行全部用例，返回结果列表 |
| `RunCase(test_case)` | 运行单用例 |
| `CompareWithTraditional()` | 与传统 V8 流水线对比 |
| `GenerateJSONReport(filename)` | 生成 JSON 报告 |
| `GenerateMarkdownReport(filename)` | 生成 Markdown 报告 |

#### 内建用例分类（由 `AddAllBuiltInCases` 触发）

- `AddTypeSystemTests` — 基本类型、联合、交叉、字面量
- `AddClassTests` — 类、继承、访问修饰符
- `AddInterfaceTests` — 接口、结构类型
- `AddGenericTests` — 泛型函数、泛型类、约束
- `AddEnumTests` — 常量枚举、计算枚举
- `AddAdvancedTypeTests` — 条件类型、映射类型、`satisfies`
- `AddPerformanceTests` — 递归、热点函数、IC 压力测试

---

### 5.10 入口点模块 (Entry Point)

**文件**：`src/ts/ts-entry-point.cc`

**职责**：`tsv8` 可执行程序的 `main()`，处理命令行、驱动编译/执行/基准测试。

#### 关键结构：`CommandLineOptions`

```cpp
struct CommandLineOptions {
  std::string filename;
  bool run_benchmark;
  bool check_only;
  bool compare_mode;
  bool show_help;
  bool show_version;
  bool use_strict;
  bool use_type_check;
  bool force_ts;
  int warmup_iterations;
  int measurement_iterations;
  std::string benchmark_output_dir;
  bool json_output;
};
```

#### 关键类：`TSV8Runner`

封装 V8 `Isolate` 的启动、TS-V8 初始化、执行上下文创建。

| 方法 | 说明 |
| --- | --- |
| `Start()` | 创建 Platform + Isolate，初始化 TS-V8 |
| `ExecuteFile(filename, force_ts)` | 完整编译并运行 `.ts` 文件 |
| `CheckFile(filename)` | 仅做类型检查 |
| `CompareFile(filename, warmup, measure)` | TS-V8 vs 传统 V8 多次对比 |
| `RunBenchmark(opts)` | 运行全套基准测试 |
| `GenerateJSONResults(results)` | 将结果序列化为 JSON |

#### 关键自由函数

- `TSV8Main(argc, argv)` — 业务主函数，解析命令行并调度 `TSV8Runner`
- `main(argc, argv)` — 实际的 C 入口点，转调 `TSV8Main`
- `ParseCommandLine(argc, argv)` — 命令行解析
- `PrintUsage(program_name)` / `PrintVersion()` — 帮助与版本输出
- `ReadFileContents(filename)` / `WriteFileContents(filename, contents)` — 文件 I/O
- `EscapeJSON(s)` — JSON 字符串转义

---

## 6. 关键类与函数说明

### 类型系统核心 API

| 类 / 函数 | 位置 | 用途 |
| --- | --- | --- |
| `v8::internal::ts::TSTypeSystem` | `ts-type-system.h` | 类型工厂、子类型计算、联合/交叉构造 |
| `v8::internal::ts::TSType` | `ts-type-system.h` | 类型节点，提供 `IsSubtypeOf / IsAssignableTo` |
| `v8::internal::ts::PropertyDescriptor` | `ts-type-system.h` | 对象属性描述 |

### 解析 / 检查核心 API

| 类 / 函数 | 位置 | 用途 |
| --- | --- | --- |
| `v8::internal::ts::TSParser` | `ts-parser.h` | TS 语法解析 |
| `v8::internal::ts::TSTypeChecker` | `ts-type-checker.h` | 类型检查与推断 |
| `v8::internal::ts::TSPipeline` | `ts-pipeline.h` | 编译流水线编排 |

### 字节码 / Map 核心 API

| 类 / 函数 | 位置 | 用途 |
| --- | --- | --- |
| `v8::internal::ts::TSBytecodeBuilder` | `ts-bytecode-extensions.h` | 特化字节码生成 |
| `v8::internal::ts::TSBytecodeIntegrator` | `ts-bytecode-extensions.h` | BytecodeGenerator 插件 |
| `v8::internal::ts::TSMapFactory` | `ts-map-extensions.h` | 稳定 Map 创建 |
| `v8::internal::ts::TSICOptimizer` | `ts-map-extensions.h` | IC 特化 |

### JIT 核心 API

| 类 / 函数 | 位置 | 用途 |
| --- | --- | --- |
| `v8::internal::ts::TSToTurboFanBridge` | `ts-jit-integration.h` | TS → TurboFan 类型桥 |
| `v8::internal::ts::TSTurboFanIntegration` | `ts-jit-integration.h` | TurboFan 阶段钩子 |
| `v8::internal::ts::TSMaglevIntegration` | `ts-jit-integration.h` | Maglev 阶段钩子 |
| `v8::internal::ts::TSRepresentationSelector` | `ts-jit-integration.h` | 机器表示选择 |
| `v8::internal::ts::TSTypeToCompilerType` | `ts-jit-integration.h` | 便捷转换函数 |

### 构建 / 入口

| 类 / 函数 | 位置 | 用途 |
| --- | --- | --- |
| `v8::internal::ts::TSBuildIntegration` | `ts-build-integration.h` | 与 V8 解析管线集成 |
| `v8::internal::ts::TSBenchmarkSuite` | `ts-benchmark-suite.h` | 基准测试运行器 |
| `v8::internal::ts::TSV8Runner` | `ts-entry-point.cc` | 可执行文件运行器 |
| `v8::internal::ts::TSV8Main` | `ts-entry-point.cc` | 可执行业务入口 |

---

## 7. 依赖关系

### 外部依赖（V8 内部组件）

TS-V8 通过 `BUILD.gn` 显式依赖以下 V8 组件：

| 依赖目标 | 提供的能力 |
| --- | --- |
| `../../:v8` | V8 核心引擎（`Isolate`、`Handle`、`Script` 等） |
| `../../:v8_libbase` | V8 基础库（`Zone`、`Time` 等） |
| `../../:v8_libplatform` | V8 平台抽象（`Platform`） |
| `../../src/ast:ast` | V8 AST 节点（`FunctionLiteral`、`AstNode` 等） |
| `../../src/objects:objects` | V8 对象系统（`Map`、`DescriptorArray`、`FieldType`） |
| `../../src/compiler:compiler` | TurboFan 编译器（`TFGraph`、`Node`、`compiler::Type`） |
| `../../src/maglev:maglev` | Maglev 编译器（`MaglevCompilationInfo`） |
| `../../src/interpreter:interpreter` | 解释器字节码生成（`BytecodeArrayBuilder`） |
| `../../src/wasm:wasm`（可选） | 当 `v8_enable_temporal_support` 启用时附加 |

### 内部依赖（模块间）

```
ts-entry-point.cc
    ├── ts-benchmark-suite.h
    ├── ts-build-integration.h
    ├── ts-pipeline.h
    ├── ts-type-checker.h
    └── ts-type-system.h

ts-build-integration.{h,cc}
    ├── ts-pipeline.h
    └── src/parsing/parsing.h

ts-pipeline.{h,cc}
    ├── ts-parser.h
    ├── ts-type-checker.h
    └── ts-type-system.h

ts-parser.{h,cc}
    └── ts-type-system.h

ts-type-checker.{h,cc}
    └── ts-type-system.h

ts-bytecode-extensions.{h,cc}
    └── ts-type-system.h

ts-map-extensions.{h,cc}
    └── ts-type-system.h

ts-jit-integration.{h,cc}
    └── ts-type-system.h
```

### 头文件包含拓扑（简化）

`TSTypeSystem` ← `TSType` ← 几乎所有其他模块
`TSParser` / `TSTypeChecker` ← `TSPipeline`
`TSPipeline` ← `TSBuildIntegration` ← `TSV8Runner`

---

## 8. 构建与运行方式

### 构建

项目使用 V8 标准 GN + Ninja 构建系统。`src/ts/BUILD.gn` 定义了三个 source_set：

| 目标 | 产物 | 说明 |
| --- | --- | --- |
| `ts_v8` | 静态库 | TS 核心模块（解析、类型系统、流水线、字节码、Map、JIT、基准、构建集成） |
| `tsv8_lib` | 可执行 `tsv8` | 独立的 TypeScript 运行器 |
| `tsv8_benchmark` | 可执行 `tsv8_benchmark` | 基准测试专用二进制 |

#### 构建步骤

```bash
# 1. 确保 V8 构建环境就绪（GN 已生成 build 目录，例如 out.gn/x64.release）
# 2. 编译 ts_v8 目标
ninja -C out.gn/x64.release ts_v8

# 3. 编译 tsv8 可执行文件
ninja -C out.gn/x64.release tsv8_lib
# 输出：out.gn/x64.release/tsv8

# 4. 编译基准测试可执行文件
ninja -C out.gn/x64.release tsv8_benchmark
# 输出：out.gn/x64.release/tsv8_benchmark
```

#### 启用宏

`BUILD.gn` 中的 `ts_v8_config` 定义了以下宏：

- `V8_ENABLE_TS_SUPPORT` — 启用 TS 支持
- `V8_ENABLE_TS_TYPE_CHECKING` — 启用 TS 类型检查

### 运行

#### 基本用法

```bash
# 编译并运行 TypeScript 文件
./out.gn/x64.release/tsv8 app.ts

# 仅做类型检查（不执行）
./out.gn/x64.release/tsv8 --check-only app.ts

# 强制将 JavaScript 文件当作 TypeScript 处理
./out.gn/x64.release/tsv8 --force-ts app.js

# 与传统 V8 流水线进行性能对比
./out.gn/x64.release/tsv8 --compare app.ts

# 运行全套内置基准测试
./out.gn/x64.release/tsv8 --benchmark

# 自定义基准参数
./out.gn/x64.release/tsv8 --benchmark --warmup 5 --measure 20 --output-dir ./results

# 以 JSON 格式输出基准测试结果
./out.gn/x64.release/tsv8 --benchmark --json

# 查看帮助
./out.gn/x64.release/tsv8 --help

# 查看版本
./out.gn/x64.release/tsv8 --version
```

#### 演示脚本

`scripts/tsv8-demo.sh` 提供了一键演示：自动创建测试 `.ts` 文件 → 调用 `tsv8` 执行 → 输出性能指标。

```bash
bash scripts/tsv8-demo.sh
```

### 输出示例

```
TS-V8: TypeScript V8 Integration
=================================

Execution result: 42

--- Performance ---
  Compilation:  1.234 ms
  Execution:    0.056 ms
  Total:        1.290 ms
```

---

## 9. 配置说明

### `TSCompilationConfig` 字段说明

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `is_typescript` | `false` | 是否按 TS 语义解析 |
| `type_check` | `true` | 是否执行类型检查 |
| `strict_mode` | `true` | 是否启用严格模式 |
| `check_nulls` | `true` | 是否启用 null/undefined 检查 |
| `check_implicit_any` | `false` | 是否禁止隐式 `any` |
| `skip_type_erasure` | `false` | 是否保留 TS 注解供 JIT 使用 |
| `generate_runtime_checks` | `false` | 是否生成运行时类型断言（不推荐） |
| `trust_types` | `true` | 是否信任类型注解（跳过运行时检查） |

### `TSBytecodeConfig` 字段说明

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `strategy` | `kSkipTypeChecks` | 默认跳过类型检查策略 |
| `skip_toboolean_conversion` | `true` | 跳过 `!!x` 转换（当 TS 保证 boolean 时） |
| `skip_tonumber_conversion` | `true` | 跳过 `+x` / `-x` 转换 |
| `skip_string_conversion` | `true` | 跳过 `""+x` 转换 |
| `use_smi_fast_paths` | `true` | 对 number 使用 Smi 快速路径 |
| `skip_map_checks` | `true` | 跳过稳定 Map 的检查 |
| `inline_property_access` | `true` | 内联属性访问 |

### `BenchmarkConfig` 字段说明

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `run_correctness_tests` | `true` | 运行正确性测试 |
| `run_performance_tests` | `true` | 运行性能测试 |
| `compare_with_legacy` | `true` | 与传统 V8 对比 |
| `warmup_iterations` | `3` | 预热次数 |
| `measurement_iterations` | `10` | 测量次数 |
| `output_dir` | `/workspace/ts-benchmark-results` | 报告输出目录 |
| `generate_report` | `true` | 是否生成报告 |

---

## 10. 设计决策摘要

| 决策 | 选择 | 原因 |
| --- | --- | --- |
| 类型系统风格 | 结构类型 (structural) | 与 TypeScript 官方行为一致 |
| 类型承载位置 | 独立 `TSType` 对象，由 `Zone` 分配 | 避免 GC 压力，允许跨阶段共享 |
| Map 扩展方式 | 通过 `Map::bit_field3` 的 bit 28 标记 + 侧表 `metadata_table_` | 侵入最小，可按需启用 |
| JIT 桥接 | 显式 `TSToTurboFanBridge` 而非修改 TurboFan 源码 | 关注点分离，便于跟进 V8 上游 |
| 字节码扩展 | 包装 `BytecodeArrayBuilder`，不直接修改字节码定义 | 避免破坏 V8 现有字节码兼容性 |
| 开关粒度 | 全局 `Enable/Disable` + 单文件 `ForceTSMode` | 便于混合 TS/JS 项目 |
| 构建集成 | 通过 `src/parsing/parsing.h` 命名空间重载接管 | 对 V8 其他模块透明 |

---

## 11. 术语表

| 术语 | 说明 |
| --- | --- |
| V8 | Google 开发的 JavaScript 引擎 |
| JIT | Just-In-Time 编译（TurboFan、Maglev） |
| IC | Inline Cache，V8 属性访问优化机制 |
| Hidden Class (Map) | V8 中描述对象"形状"的元数据结构 |
| TF / TurboFan | V8 的优化编译器之一，面向长线运行 |
| Maglev | V8 的新一代快速 JIT 编译器 |
| Smi | Small Integer，V8 的小整数内联表示 |
| HeapNumber | V8 的堆分配数字表示 |
| Zone | V8 的分段内存分配器，用于临时分配 |
| TSType | TS-V8 中对 TypeScript 类型的运行时表示 |
| MapCreationHint | TS-V8 为 V8 Map 创建提供的提示信息 |
| TypeInfoForJIT | TS-V8 提供给 TurboFan/Maglev 的类型元数据 |

---

*本文档由对 TS-V8 项目仓库的静态分析生成，与 `DESIGN.md` 互补：`DESIGN.md` 关注高层设计动机与路线图，本文档关注代码层面的结构与 API。*
