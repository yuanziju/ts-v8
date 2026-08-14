#ifndef V8_TS_TS_TYPE_CHECKER_H_
#define V8_TS_TS_TYPE_CHECKER_H_

#include "src/ts/ts-type-system.h"
#include "src/ast/ast.h"
#include "src/ast/scopes.h"

namespace v8 {
namespace internal {
namespace ts {

class TSTypeChecker : public ZoneObject {
 public:
  explicit TSTypeChecker(Zone* zone, TSTypeSystem* type_system);

  void CheckProgram(FunctionLiteral* program);

  void CheckFunction(FunctionLiteral* function);

  TSType* CheckExpression(Expression* expr);

  void CheckVariableDeclaration(Variable* var, TSType* annotated_type,
                                 Expression* initializer);

  bool CheckAssignment(TSType* target_type, Expression* value);

  bool CheckCall(TSType* callee_type, ZoneList<TSType*>* arg_types);

  bool CheckPropertyAccess(TSType* object_type, const AstRawString* property_name);

  bool CheckReturn(TSType* function_return_type, Expression* return_value);

  TSType* CheckBinaryOperation(int op, TSType* left, TSType* right);

  TSType* CheckComparison(int op, TSType* left, TSType* right);

  void ReportError(const char* message, int position);
  void ReportWarning(const char* message, int position);

  int error_count() const { return error_count_; }
  int warning_count() const { return warning_count_; }

  void SetStrictMode(bool strict) { strict_mode_ = strict; }
  void SetCheckNulls(bool check) { check_nulls_ = check; }
  void SetCheckImplicitAny(bool check) { check_implicit_any_ = check; }

  void EnterFunctionContext(FunctionLiteral* function);
  void ExitFunctionContext();
  void EnterBlockContext();
  void ExitBlockContext();

  TSType* NarrowType(TSType* type, Expression* condition, bool branch_taken);

  TSType* NarrowTypeFromCondition(Expression* condition, bool branch_taken);
  TSType* NarrowTypeFromTypePredicate(TSType* type, Expression* condition, bool branch_taken);
  TSType* NarrowFromTypeof(TSType* type, const char* typeof_result, bool branch_taken);
  TSType* NarrowFromInOperator(TSType* type, const char* key, bool branch_taken);

 private:
  TSType* InferLiteralType(Literal* literal);
  TSType* InferBinaryOpType(BinaryOperation* op);
  TSType* InferCallType(Call* call);
  TSType* InferPropertyLoadType(Property* prop);
  TSType* InferConditionalType(Conditional* cond);
  TSType* InferArrayLiteralType(ArrayLiteral* arr);
  TSType* InferObjectLiteralType(ObjectLiteral* obj);
  TSType* InferFunctionType(FunctionLiteral* func);
  TSType* InferMemberAccessType(Expression* object, const AstRawString* name);

  void CheckBlock(Block* block);
  void CheckIfStatement(IfStatement* stmt);
  void CheckForStatement(ForStatement* stmt);
  void CheckWhileStatement(WhileStatement* stmt);
  void CheckReturnStatement(ReturnStatement* stmt);
  void CheckTryStatement(TryCatchStatement* stmt);

  TSType* NarrowFromEquality(TSType* type, Expression* compared_value,
                              bool is_equal);
  TSType* NarrowFromInstanceOf(TSType* type, Expression* class_expr);
  TSType* NarrowFromTruthiness(TSType* type, bool is_truthy);

  bool IsAssignableTo(TSType* source, TSType* target);
  bool IsStructuralSubtype(TSType* source, TSType* target);
  bool CheckPropertyCompatibility(const PropertyDescriptor& source_prop,
                                   const PropertyDescriptor& target_prop);

  ZoneList<TSType*> context_stack_;
  FunctionLiteral* current_function_ = nullptr;

  TSTypeSystem* type_system_;
  Zone* zone_;

  int error_count_ = 0;
  int warning_count_ = 0;

  bool strict_mode_ = true;
  bool check_nulls_ = true;
  bool check_implicit_any_ = false;
};

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_TYPE_CHECKER_H_