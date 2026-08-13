# TS-V8: Native TypeScript Support in V8

## Overview

Adapt V8 from the ground up to natively support TypeScript, eliminating the need for transpilation. Type information flows through every layer: parser → AST → bytecode → interpreter → JIT compilers → hidden classes.

## Architecture

```
TS Source Code
     │
     ▼
┌─────────────────────────────────┐
│  TS Parser (extends ParserBase) │ ← New AST nodes for TS constructs
│  - Type annotations              │
│  - Interfaces, Enums, Generics  │
│  - Decorators, Namespaces       │
└──────────────┬──────────────────┘
               │ AST with TypeInfo
               ▼
┌─────────────────────────────────┐
│  TS Type Checker (new module)   │ ← Type resolution + inference
│  - Structural type system       │    Integrated with V8's type lattice
│  - Flow-sensitive analysis      │
│  - Interface/class hierarchy     │
└──────────────┬──────────────────┘
               │ Type-annotated AST
               ▼
┌─────────────────────────────────┐
│  Bytecode Generator             │ ← Type-aware bytecode
│  - Skip redundant checks        │    New TypeHint carries TS types
│  - Specialized instructions     │    TS types → optimized bytecode
└──────────────┬──────────────────┘
               │ Type-rich Bytecode
               ▼
┌─────────────────────────────────┐
│  Interpreter (Ignition)         │ ← Fast-path for typed operations
│  - Direct typed paths           │    Skip IC when type is statically known
│  - TS type enforcement          │
└──────────────┬──────────────────┘
               │ Feedback + Static Types
               ▼
┌─────────────────────────────────┐
│  JIT Compilers (Maglev/TurboFan)│ ← TS types drive optimization
│  - Precise type narrowing       │    Skip Typer's speculative inference
│  - Specialized codegen          │    Direct representation selection
│  - Stable hidden classes        │    Pre-compute Map structures
└──────────────┬──────────────────┘
               │
               ▼
┌─────────────────────────────────┐
│  Hidden Class System (Maps)     │ ← Extended with TS type metadata
│  - Type-prefilled FieldTypes    │    FieldType::Class(map) from annotations
│  - Stable Map invariants       │    Skip map checks when TS guarantees stability
│  - Type-specialized transitions │    Predictable transition paths
└─────────────────────────────────┘
```

## Implementation Phases

### Phase 1: TS Parser + AST
- New AST nodes: TypeAnnotation, TypeParameter, InterfaceDeclaration, EnumDeclaration, etc.
- Extend ParserBase<TSParser> with TS syntax handling
- New Token types for TS keywords
- Scanner extensions

### Phase 2: TS Type System
- New module: src/ts/type-system.h, src/ts/type-checker.h
- Structural type representation
- Type resolution, flow analysis
- Integration with V8's Type lattice

### Phase 3: Bytecode Generation
- Extend TypeHint to carry full TS type info
- New bytecodes for typed operations
- Skip hole checks, type guards when TS types guarantee correctness
- Generate specialized paths for typed arithmetic, property access

### Phase 4: JIT Integration
- Feed TS types into TurboFan Typer (skip speculative phase)
- Maglev graph building with TS annotations
- Direct representation selection from TS types
- Skip unnecessary deoptimization points

### Phase 5: Map System Extension
- Extended Map with TS type metadata
- Pre-populated FieldType from annotations
- Stable Maps for TS-typed objects
- Type-specialized IC slots

### Phase 6: Benchmark Suite
- Major TS projects as benchmarks
- Correctness validation (traditional vs ts-v8)
- Performance comparison

## Key Design Decisions

1. **No separate transpilation step**: TS source is parsed directly, type annotations are retained through the pipeline
2. **Type erasure at bytecode level**: Runtime type checking is configurable; default is "trust types" mode
3. **Incremental type checking**: Types are checked on-the-fly during parsing/compilation, not as a separate pass
4. **Coexistence mode**: ts-v8 can run both .js and .ts files transparently
5. **TS types extend V8's type lattice**: Not a separate system, but integrated

## File Structure

```
src/ts/
  ├── ts-parser.h          # TSParser class
  ├── ts-parser.cc         # TS parser implementation
  ├── ts-type-system.h     # TS type representation
  ├── ts-type-system.cc    # TS type system implementation
  ├── ts-type-checker.h    # Type checking logic
  ├── ts-type-checker.cc   # Type checking implementation
  ├── ts-type-inference.h  # Type inference
  └── ts-type-inference.cc # Type inference implementation
```
