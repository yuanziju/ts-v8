#!/bin/bash
# TS-V8 Quick Demo Script
# Demonstrates how to use TS-V8's native TypeScript support

echo "=== TS-V8: Native TypeScript in V8 ==="
echo ""

# Create a test TypeScript file
cat > /tmp/test-tsv8.ts << 'EOF'
// TypeScript with native V8 support
// No transpilation needed - V8 understands TS directly!

interface User {
  name: string;
  age: number;
  email?: string;
  readonly id: number;
}

class UserService {
  private users: Map<number, User> = new Map();

  addUser(user: User): void {
    this.users.set(user.id, user);
  }

  getUser(id: number): User | undefined {
    return this.users.get(id);
  }

  getAdultUsers(): User[] {
    return Array.from(this.users.values()).filter(u => u.age >= 18);
  }
}

// Generic function with type constraints
function merge<T extends object, U extends object>(a: T, b: U): T & U {
  return { ...a, ...b };
}

// Enum
enum Status {
  Active = "active",
  Inactive = "inactive",
  Pending = "pending"
}

// Usage
const service = new UserService();
const user: User = { name: "Alice", age: 25, id: 1 };
service.addUser(user);

const merged = merge({ x: 1 }, { y: "hello" });
const status: Status = Status.Active;

console.log("User:", JSON.stringify(user));
console.log("Adult users:", service.getAdultUsers().length);
console.log("Merged:", JSON.stringify(merged));
console.log("Status:", status);
console.log("TypeScript compiled and executed natively in V8!");
EOF

echo "Created test file: /tmp/test-tsv8.ts"
echo ""

# Try to run with tsv8 if available
if command -v tsv8 &> /dev/null; then
    echo "Running with TS-V8 native engine..."
    tsv8 /tmp/test-tsv8.ts
elif [ -f "/workspace/v8/out/x64.release/tsv8" ]; then
    echo "Running with TS-V8 from build..."
    /workspace/v8/out/x64.release/tsv8 /tmp/test-tsv8.ts
else
    echo "TS-V8 executable not yet built."
    echo "Build instructions:"
    echo "  1. gclient sync"
    echo "  2. gn gen out/x64.release"
    echo "  3. ninja -C out/x64.release tsv8"
    echo ""
    echo "The TS-V8 module provides:"
    echo "  - Native TypeScript parsing without transpilation"
    echo "  - Type information flows through the entire compilation pipeline"
    echo "  - Optimized bytecode based on type annotations"
    echo "  - Enhanced JIT compilation with type-driven specialization"
    echo "  - Extended hidden classes carrying TS type metadata"
    echo ""
    echo "Source files in src/ts/:"
    echo "  ts-parser.h/cc          - TypeScript parser extension"
    echo "  ts-type-system.h/cc     - Core TS type representation"
    echo "  ts-type-checker.h/cc    - Type checking and validation"
    echo "  ts-pipeline.h/cc        - TS-aware compilation pipeline"
    echo "  ts-bytecode-extensions.h/cc - Type-aware bytecode generation"
    echo "  ts-map-extensions.h/cc - Hidden class extensions for TS types"
    echo "  ts-jit-integration.h/cc - JIT compiler integration"
    echo "  ts-benchmark-suite.h/cc - Benchmark and validation framework"
    echo "  ts-build-integration.h/cc - Build system integration"
    echo "  ts-entry-point.cc       - Standalone tsv8 executable"
fi

echo ""
echo "=== Key Architectural Features ==="
echo ""
echo "1. Parser Layer: TS syntax parsed natively (no transpilation)"
echo "   - Type annotations, interfaces, generics, enums"
echo "   - Decorators, namespaces, as/satisfies expressions"
echo ""
echo "2. Type System: Full TS type lattice integrated with V8"
echo "   - Structural subtyping, flow-sensitive analysis"
echo "   - Union/intersection/conditional/mapped types"
echo "   - Interface/class hierarchy resolution"
echo ""
echo "3. Bytecode Layer: Type-aware bytecode generation"
echo "   - Skip redundant type checks when TS types guarantee correctness"
echo "   - Specialized paths for typed arithmetic, property access"
echo "   - Hole check elimination for typed variables"
echo ""
echo "4. JIT Layer: TS types drive TurboFan/Maglev optimization"
echo "   - Skip speculative inference when types are statically known"
echo "   - Direct representation selection from TS types"
echo "   - Eliminate unnecessary deoptimization points"
echo ""
echo "5. Hidden Class Layer: Extended Maps with TS type metadata"
echo "   - Pre-populated FieldType from annotations"
echo "   - Stable Map invariants for TS-typed objects"
echo "   - Type-specialized inline caches"
