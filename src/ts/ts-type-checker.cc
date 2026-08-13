#include "src/ts/ts-type-checker.h"

#include <cstdio>
#include <string>

namespace v8 {
namespace internal {
namespace ts {

TSTypeChecker::TSTypeChecker(Zone* zone, TSTypeSystem* type_system)
    : context_stack_(zone),
      type_system_(type_system),
      zone_(zone) {}

void TSTypeChecker::CheckProgram(FunctionLiteral* program) {
  if (program == nullptr) return;
  EnterFunctionContext(program);
  CheckBlock(program->body());
  ExitFunctionContext();
}

void TSTypeChecker::CheckFunction(FunctionLiteral* function) {
  if (function == nullptr) return;
  EnterFunctionContext(function);
  if (function->body() != nullptr) {
    CheckBlock(function->body());
  }
  ExitFunctionContext();
}

TSType* TSTypeChecker::CheckExpression(Expression* expr) {
  if (expr == nullptr) return type_system_->GetUndefinedType();

  switch (expr->node_type()) {
    case AstNode::kLiteral: {
      Literal* lit = expr->AsLiteral();
      return InferLiteralType(lit);
    }

    case AstNode::kBinaryOperation: {
      BinaryOperation* binop = expr->AsBinaryOperation();
      return InferBinaryOpType(binop);
    }

    case AstNode::kCall: {
      Call* call = expr->AsCall();
      return InferCallType(call);
    }

    case AstNode::kCallNew: {
      CallNew* new_call = expr->AsCallNew();
      TSType* ctor_type = CheckExpression(new_call->expression());
      if (ctor_type->IsConstructor()) {
        return ctor_type->AsConstructorReturnType();
      }
      ZonePtrList<Expression>* args = new_call->arguments();
      for (int i = 0; i < static_cast<int>(args->size()); i++) {
        CheckExpression((*args)[i]);
      }
      return type_system_->GetAnyType();
    }

    case AstNode::kProperty: {
      Property* prop = expr->AsProperty();
      return InferPropertyLoadType(prop);
    }

    case AstNode::kConditional: {
      Conditional* cond = expr->AsConditional();
      return InferConditionalType(cond);
    }

    case AstNode::kArrayLiteral: {
      ArrayLiteral* arr = expr->AsArrayLiteral();
      return InferArrayLiteralType(arr);
    }

    case AstNode::kObjectLiteral: {
      ObjectLiteral* obj = expr->AsObjectLiteral();
      return InferObjectLiteralType(obj);
    }

    case AstNode::kFunctionLiteral: {
      FunctionLiteral* func = expr->AsFunctionLiteral();
      return InferFunctionType(func);
    }

    case AstNode::kCompareOperation: {
      CompareOperation* cmp = expr->AsCompareOperation();
      TSType* left_type = CheckExpression(cmp->left());
      TSType* right_type = CheckExpression(cmp->right());
      return CheckComparison(cmp->op(), left_type, right_type);
    }

    case AstNode::kVariableProxy: {
      VariableProxy* proxy = expr->AsVariableProxy();
      if (proxy->is_resolved()) {
        Variable* var = proxy->var();
        const AstRawString* var_name = var->raw_name();
        TSType* known = InferMemberAccessType(nullptr, var_name);
        if (known != nullptr) return known;
      }
      if (check_implicit_any_) {
        ReportWarning("Variable has implicit 'any' type", expr->position());
      }
      return type_system_->GetAnyType();
    }

    case AstNode::kAssignment: {
      Assignment* assign = expr->AsAssignment();
      TSType* value_type = CheckExpression(assign->value());
      Expression* target = assign->target();
      if (target->IsProperty()) {
        Property* prop = target->AsProperty();
        TSType* obj_type = CheckExpression(prop->obj());
        if (!CheckAssignment(obj_type, assign->value())) {
          ReportError(
              "Type '" + std::string(value_type->Name()) +
                  "' is not assignable to target property",
              expr->position());
        }
      } else if (target->IsVariableProxy()) {
        VariableProxy* proxy = target->AsVariableProxy();
        if (proxy->is_resolved()) {
          Variable* var = proxy->var();
          TSType* var_type =
              InferMemberAccessType(nullptr, var->raw_name());
          if (var_type != nullptr &&
              !CheckAssignment(var_type, assign->value())) {
            ReportError("Type '" + std::string(value_type->Name()) +
                            "' is not assignable to variable '" +
                            std::string(var->raw_name()->c_str()) + "'",
                        expr->position());
          }
        }
      }
      return value_type;
    }

    case AstNode::kUnaryOperation: {
      UnaryOperation* unary = expr->AsUnaryOperation();
      TSType* operand_type = CheckExpression(unary->expression());
      Token::Value op = unary->op();
      if (op == Token::kNot) {
        return type_system_->GetBooleanType();
      }
      if (op == Token::kTypeOf) {
        return type_system_->GetStringType();
      }
      if (op == Token::kVoid) {
        return type_system_->GetUndefinedType();
      }
      if (op == Token::kDelete) {
        return type_system_->GetBooleanType();
      }
      if (op == Token::kBitNot || op == Token::kNeg || op == Token::kPos) {
        if (!operand_type->IsNumber() && !operand_type->IsAny()) {
          if (strict_mode_) {
            ReportError(
                "Operand of unary numeric operation must be a number",
                expr->position());
          }
        }
        return type_system_->GetNumberType();
      }
      return operand_type;
    }

    case AstNode::kThisExpression: {
      if (current_function_ != nullptr &&
          current_function_->IsClassConstructor()) {
        return type_system_->GetThisType();
      }
      return type_system_->GetAnyType();
    }

    case AstNode::kAwait: {
      Await* await_expr = expr->AsAwait();
      TSType* awaited_type = CheckExpression(await_expr->expression());
      if (awaited_type->IsPromise()) {
        return awaited_type->AsPromiseReturnType();
      }
      return awaited_type;
    }

    case AstNode::kYield:
    case AstNode::kYieldStar:
      return type_system_->GetAnyType();

    case AstNode::kThrow: {
      Throw* throw_expr = expr->AsThrow();
      CheckExpression(throw_expr->expression());
      return type_system_->GetNeverType();
    }

    case AstNode::kConditionalChain:
    case AstNode::kOptionalChain:
      return type_system_->GetAnyType();

    case AstNode::kSpread:
      return type_system_->GetAnyType();

    case AstNode::kTemplateLiteral:
      return type_system_->GetStringType();

    default:
      return type_system_->GetAnyType();
  }
}

TSType* TSTypeChecker::InferLiteralType(Literal* literal) {
  switch (literal->type()) {
    case Literal::kSmi:
    case Literal::kHeapNumber:
      return type_system_->GetNumberType();
    case Literal::kBigInt:
      return type_system_->GetBigIntType();
    case Literal::kString:
    case Literal::kConsString:
      return type_system_->GetStringType();
    case Literal::kBoolean:
      return type_system_->GetBooleanType();
    case Literal::kUndefined:
      return type_system_->GetUndefinedType();
    case Literal::kNull:
      if (check_nulls_) {
        return type_system_->GetNullType();
      }
      return type_system_->GetAnyType();
    case Literal::kTheHole:
      return type_system_->GetUndefinedType();
  }
  return type_system_->GetAnyType();
}

TSType* TSTypeChecker::InferBinaryOpType(BinaryOperation* op) {
  TSType* left_type = CheckExpression(op->left());
  TSType* right_type = CheckExpression(op->right());
  return CheckBinaryOperation(op->op(), left_type, right_type);
}

TSType* TSTypeChecker::InferCallType(Call* call) {
  TSType* callee_type = CheckExpression(call->expression());

  if (callee_type->IsAny()) {
    return type_system_->GetAnyType();
  }

  if (callee_type->IsFunction()) {
    FunctionType* func_type = callee_type->AsFunctionType();
    const ZoneList<TSType*>* param_types = func_type->ParameterTypes();
    int param_count = param_types ? param_types->size() : 0;
    const ZonePtrList<Expression>* args = call->arguments();
    int arg_count = args ? static_cast<int>(args->size()) : 0;

    if (arg_count > param_count && !func_type->HasRestParameter()) {
      ReportError("Expected " + std::to_string(param_count) +
                      " arguments but got " + std::to_string(arg_count),
                  call->position());
    }

    for (int i = 0; i < arg_count && i < param_count; i++) {
      TSType* arg_type = CheckExpression((*args)[i]);
      TSType* param_type = param_types->at(i);
      if (!IsAssignableTo(arg_type, param_type)) {
        ReportError("Argument of type '" + std::string(arg_type->Name()) +
                        "' is not assignable to parameter of type '" +
                        std::string(param_type->Name()) + "'",
                    call->position());
      }
    }

    return func_type->ReturnType();
  }

  if (callee_type->IsUnion()) {
    UnionType* union_type = callee_type->AsUnionType();
    TSType* last_return = type_system_->GetAnyType();
    bool has_valid_overload = false;
    for (int i = 0; i < union_type->arity(); i++) {
      TSType* constituent = union_type->type_at(i);
      if (constituent->IsFunction()) {
        has_valid_overload = true;
        last_return = constituent->AsFunctionType()->ReturnType();
      }
    }
    if (has_valid_overload) return last_return;
  }

  if (callee_type->IsConditionalType()) {
    ConditionalType* cond_type = callee_type->AsConditionalType();
    return cond_type->DefaultType();
  }

  ReportError("Cannot call value of type '" +
                  std::string(callee_type->Name()) + "'",
              call->position());
  return type_system_->GetAnyType();
}

TSType* TSTypeChecker::InferPropertyLoadType(Property* prop) {
  TSType* obj_type = CheckExpression(prop->obj());
  Expression* key = prop->key();

  if (obj_type->IsAny()) {
    return type_system_->GetAnyType();
  }

  if (obj_type->IsUnion()) {
    UnionType* union_type = obj_type->AsUnionType();
    TSType* result = nullptr;
    for (int i = 0; i < union_type->arity(); i++) {
      TSType* constituent = union_type->type_at(i);
      if (constituent->IsObject()) {
        ObjectType* obj = constituent->AsObjectType();
        const AstRawString* prop_name = nullptr;
        if (key->IsVariableProxy()) {
          prop_name = key->AsVariableProxy()->raw_name();
        } else if (key->IsLiteral()) {
          Literal* lit_key = key->AsLiteral();
          if (lit_key->type() == Literal::kString ||
              lit_key->type() == Literal::kConsString) {
            prop_name = lit_key->AsRawString();
          }
        }
        if (prop_name != nullptr) {
          PropertyDescriptor* desc = obj->GetProperty(prop_name);
          if (desc != nullptr) {
            if (result == nullptr) {
              result = desc->GetType();
            } else {
              result = type_system_->CreateUnionType(result, desc->GetType());
            }
          }
        }
      }
    }
    if (result != nullptr) return result;
    return type_system_->GetAnyType();
  }

  if (obj_type->IsObject()) {
    ObjectType* obj = obj_type->AsObjectType();
    const AstRawString* prop_name = nullptr;
    if (key->IsVariableProxy()) {
      prop_name = key->AsVariableProxy()->raw_name();
    } else if (key->IsLiteral()) {
      Literal* lit_key = key->AsLiteral();
      if (lit_key->type() == Literal::kString ||
          lit_key->type() == Literal::kConsString) {
        prop_name = lit_key->AsRawString();
      }
    }
    if (prop_name != nullptr) {
      PropertyDescriptor* desc = obj->GetProperty(prop_name);
      if (desc != nullptr) {
        if (desc->IsOptional() && check_nulls_) {
          return type_system_->CreateUnionType(
              desc->GetType(), type_system_->GetUndefinedType());
        }
        return desc->GetType();
      }
      if (strict_mode_) {
        ReportError("Property '" + std::string(prop_name->c_str()) +
                        "' does not exist on type '" +
                        std::string(obj_type->Name()) + "'",
                    prop->position());
      }
    }
    if (obj->HasIndexSignature()) {
      return obj->IndexSignatureType();
    }
    if (strict_mode_) {
      ReportError("Property access on type '" +
                      std::string(obj_type->Name()) +
                      "' with unknown property",
                  prop->position());
    }
    return type_system_->GetAnyType();
  }

  if (obj_type->IsArray()) {
    ArrayType* arr_type = obj_type->AsArrayType();
    if (key->IsLiteral()) {
      Literal* lit = key->AsLiteral();
      if (lit->type() == Literal::kSmi ||
          lit->type() == Literal::kHeapNumber) {
        return arr_type->ElementType();
      }
      if (lit->type() == Literal::kString ||
          lit->type() == Literal::kConsString) {
        const AstRawString* str = lit->AsRawString();
        if (str->IsOneByteEqualTo("length")) {
          return type_system_->GetNumberType();
        }
      }
    }
    if (key->IsVariableProxy()) {
      const AstRawString* name = key->AsVariableProxy()->raw_name();
      if (name->IsOneByteEqualTo("length")) {
        return type_system_->GetNumberType();
      }
      if (name->IsOneByteEqualTo("push") ||
          name->IsOneByteEqualTo("pop") ||
          name->IsOneByteEqualTo("shift") ||
          name->IsOneByteEqualTo("unshift")) {
        return type_system_->GetFunctionType();
      }
    }
    return arr_type->ElementType();
  }

  if (obj_type->IsFunction()) {
    if (key->IsVariableProxy()) {
      const AstRawString* name = key->AsVariableProxy()->raw_name();
      if (name->IsOneByteEqualTo("length") ||
          name->IsOneByteEqualTo("prototype")) {
        return type_system_->GetAnyType();
      }
    }
    return type_system_->GetAnyType();
  }

  if (obj_type->IsString()) {
    if (key->IsVariableProxy()) {
      const AstRawString* name = key->AsVariableProxy()->raw_name();
      if (name->IsOneByteEqualTo("length")) {
        return type_system_->GetNumberType();
      }
    }
    return type_system_->GetAnyType();
  }

  if (obj_type->IsNumber()) {
    return type_system_->GetAnyType();
  }

  if (check_nulls_ &&
      (obj_type->IsNull() || obj_type->IsUndefined())) {
    ReportError("Cannot access property on " +
                    std::string(obj_type->Name()) + " value",
                prop->position());
  }

  if (strict_mode_) {
    ReportError("Property access on unknown type '" +
                    std::string(obj_type->Name()) + "'",
                prop->position());
  }

  return type_system_->GetAnyType();
}

TSType* TSTypeChecker::InferConditionalType(Conditional* cond) {
  TSType* then_type = CheckExpression(cond->then_expression());
  TSType* else_type = CheckExpression(cond->else_expression());

  if (then_type->IsAssignableTo(else_type)) return else_type;
  if (else_type->IsAssignableTo(then_type)) return then_type;

  return type_system_->CreateUnionType(then_type, else_type);
}

TSType* TSTypeChecker::InferArrayLiteralType(ArrayLiteral* arr) {
  ZonePtrList<Expression>* elements = arr->elements();
  if (elements == nullptr || elements->size() == 0) {
    return type_system_->CreateArrayType(type_system_->GetAnyType());
  }

  TSType* element_type = nullptr;
  for (int i = 0; i < static_cast<int>(elements->size()); i++) {
    TSType* elem_type = CheckExpression((*elements)[i]);
    if (element_type == nullptr) {
      element_type = elem_type;
    } else if (!elem_type->IsAssignableTo(element_type)) {
      if (!element_type->IsAssignableTo(elem_type)) {
        element_type =
            type_system_->CreateUnionType(element_type, elem_type);
      }
    }
  }

  if (element_type == nullptr) {
    element_type = type_system_->GetAnyType();
  }

  return type_system_->CreateArrayType(element_type);
}

TSType* TSTypeChecker::InferObjectLiteralType(ObjectLiteral* obj) {
  ZonePtrList<ObjectLiteralProperty>* properties = obj->properties();
  ZoneList<PropertyDescriptor*>* props = new (zone_)
      ZoneList<PropertyDescriptor*>(properties->size(), zone_);

  for (int i = 0; i < static_cast<int>(properties->size()); i++) {
    ObjectLiteralProperty* prop = (*properties)[i];
    TSType* prop_type = CheckExpression(prop->value());
    const AstRawString* prop_name =
        prop->key()->AsLiteral()->AsRawPropertyName();
    PropertyDescriptor* desc =
        new (zone_) PropertyDescriptor(prop_name, prop_type, false);
    props->Add(desc);
  }

  return type_system_->CreateObjectType(props);
}

TSType* TSTypeChecker::InferFunctionType(FunctionLiteral* func) {
  int param_count = func->parameter_count();
  ZoneList<TSType*>* param_types =
      new (zone_) ZoneList<TSType*>(param_count, zone_);

  for (int i = 0; i < param_count; i++) {
    param_types->Add(type_system_->GetAnyType());
  }

  TSType* return_type = type_system_->GetAnyType();
  return type_system_->CreateFunctionType(param_types, return_type);
}

TSType* TSTypeChecker::InferMemberAccessType(Expression* object,
                                               const AstRawString* name) {
  if (name == nullptr) return nullptr;

  for (int i = context_stack_.size() - 1; i >= 0; i--) {
    TSType* ctx_type = context_stack_.at(i);
    if (ctx_type->IsObject()) {
      ObjectType* obj = ctx_type->AsObjectType();
      PropertyDescriptor* desc = obj->GetProperty(name);
      if (desc != nullptr) return desc->GetType();
    }
  }

  if (current_function_ != nullptr) {
    DeclarationScope* scope = current_function_->scope();
    if (scope != nullptr) {
      Variable* var = scope->LookupLocal(name);
      if (var != nullptr) {
        return type_system_->GetAnyType();
      }
    }
  }

  return nullptr;
}

void TSTypeChecker::CheckBlock(Block* block) {
  if (block == nullptr) return;
  EnterBlockContext();
  ZonePtrList<Statement>* statements = block->statements();
  if (statements == nullptr) {
    ExitBlockContext();
    return;
  }

  for (int i = 0; i < static_cast<int>(statements->size()); i++) {
    Statement* stmt = (*statements)[i];
    if (stmt == nullptr) continue;

    switch (stmt->node_type()) {
      case AstNode::kExpressionStatement: {
        ExpressionStatement* expr_stmt = stmt->AsExpressionStatement();
        CheckExpression(expr_stmt->expression());
        break;
      }
      case AstNode::kReturnStatement: {
        CheckReturnStatement(stmt->AsReturnStatement());
        break;
      }
      case AstNode::kIfStatement: {
        CheckIfStatement(stmt->AsIfStatement());
        break;
      }
      case AstNode::kForStatement: {
        CheckForStatement(stmt->AsForStatement());
        break;
      }
      case AstNode::kWhileStatement: {
        CheckWhileStatement(stmt->AsWhileStatement());
        break;
      }
      case AstNode::kDoWhileStatement: {
        DoWhileStatement* do_while = stmt->AsDoWhileStatement();
        CheckBlock(do_while->body());
        TSType* cond_type = CheckExpression(do_while->condition());
        if (!cond_type->IsBoolean() && !cond_type->IsAny() &&
            strict_mode_) {
          ReportWarning("Do-while loop condition is not a boolean type",
                        do_while->condition()->position());
        }
        break;
      }
      case AstNode::kForInStatement: {
        ForInStatement* for_in = stmt->AsForInStatement();
        CheckExpression(for_in->iterable());
        CheckBlock(for_in->body());
        break;
      }
      case AstNode::kForOfStatement: {
        ForOfStatement* for_of = stmt->AsForOfStatement();
        TSType* iter_type = CheckExpression(for_of->iterable());
        if (!iter_type->IsAny() && !iter_type->IsIterable() &&
            strict_mode_) {
          ReportError("Expression is not iterable",
                      for_of->iterable()->position());
        }
        CheckBlock(for_of->body());
        break;
      }
      case AstNode::kTryCatchStatement: {
        CheckTryStatement(stmt->AsTryCatchStatement());
        break;
      }
      case AstNode::kBlock: {
        CheckBlock(stmt->AsBlock());
        break;
      }
      case AstNode::kSwitchStatement: {
        SwitchStatement* switch_stmt = stmt->AsSwitchStatement();
        CheckExpression(switch_stmt->tag());
        CheckBlock(switch_stmt->cases());
        break;
      }
      case AstNode::kFunctionDeclaration: {
        FunctionDeclaration* func_decl = stmt->AsFunctionDeclaration();
        CheckFunction(func_decl->fun());
        break;
      }
      case AstNode::kVariableDeclaration: {
        VariableDeclaration* var_decl = stmt->AsVariableDeclaration();
        Variable* var = var_decl->var();
        Expression* init = var_decl->init();
        TSType* init_type =
            init != nullptr ? CheckExpression(init) : nullptr;
        TSType* annotated = init_type;
        CheckVariableDeclaration(var, annotated, init);
        break;
      }
      case AstNode::kEmptyStatement:
      case AstNode::kDebuggerStatement:
      case AstNode::kBreakStatement:
      case AstNode::kContinueStatement:
      case AstNode::kWithStatement:
        break;
      default:
        break;
    }
  }
  ExitBlockContext();
}

void TSTypeChecker::CheckIfStatement(IfStatement* stmt) {
  if (stmt == nullptr) return;
  TSType* cond_type = CheckExpression(stmt->condition());
  if (cond_type->IsNever() && strict_mode_) {
    ReportWarning("Condition is always 'never' type",
                   stmt->condition()->position());
  }
  if (!cond_type->IsBoolean() && !cond_type->IsAny() && strict_mode_) {
    ReportWarning("If condition is not a boolean type",
                  stmt->condition()->position());
  }
  CheckBlock(stmt->then_statement());
  if (stmt->else_statement() != nullptr) {
    CheckBlock(stmt->else_statement());
  }
}

void TSTypeChecker::CheckForStatement(ForStatement* stmt) {
  if (stmt == nullptr) return;
  if (stmt->init() != nullptr) {
    CheckExpression(stmt->init());
  }
  if (stmt->condition() != nullptr) {
    TSType* cond_type = CheckExpression(stmt->condition());
    if (!cond_type->IsBoolean() && !cond_type->IsAny() && strict_mode_) {
      ReportWarning("For loop condition is not a boolean type",
                    stmt->condition()->position());
    }
  }
  if (stmt->next() != nullptr) {
    CheckExpression(stmt->next());
  }
  CheckBlock(stmt->body());
}

void TSTypeChecker::CheckWhileStatement(WhileStatement* stmt) {
  if (stmt == nullptr) return;
  TSType* cond_type = CheckExpression(stmt->condition());
  if (!cond_type->IsBoolean() && !cond_type->IsAny() && strict_mode_) {
    ReportWarning("While loop condition is not a boolean type",
                  stmt->condition()->position());
  }
  CheckBlock(stmt->body());
}

void TSTypeChecker::CheckReturnStatement(ReturnStatement* stmt) {
  if (stmt == nullptr) return;
  Expression* value = stmt->expression();
  if (value == nullptr) {
    return;
  }

  TSType* value_type = CheckExpression(value);
  if (current_function_ != nullptr) {
    DeclarationScope* scope = current_function_->scope();
    if (scope != nullptr) {
      TSType* expected_return = type_system_->GetAnyType();
      if (!IsAssignableTo(value_type, expected_return) && strict_mode_) {
        ReportError(
            "Return statement type does not match function return type",
            stmt->position());
      }
    }
  }
}

void TSTypeChecker::CheckTryStatement(TryCatchStatement* stmt) {
  if (stmt == nullptr) return;
  CheckBlock(stmt->try_block());
  if (stmt->catch_block() != nullptr) {
    CheckBlock(stmt->catch_block());
  }
  if (stmt->finally_block() != nullptr) {
    CheckBlock(stmt->finally_block());
  }
}

void TSTypeChecker::CheckVariableDeclaration(Variable* var,
                                               TSType* annotated_type,
                                               Expression* initializer) {
  if (var == nullptr) return;

  if (initializer != nullptr && annotated_type != nullptr) {
    TSType* init_type = CheckExpression(initializer);
    if (!IsAssignableTo(init_type, annotated_type)) {
      ReportError("Type '" + std::string(init_type->Name()) +
                      "' is not assignable to '" +
                      std::string(annotated_type->Name()) + "'",
                  initializer->position());
    }
  }

  if (var->mode() == VariableMode::kConst && initializer == nullptr) {
    ReportError("'const' variable must be initialized",
                kNoSourcePosition);
  }
}

bool TSTypeChecker::CheckAssignment(TSType* target_type, Expression* value) {
  TSType* value_type = CheckExpression(value);
  return IsAssignableTo(value_type, target_type);
}

bool TSTypeChecker::CheckCall(TSType* callee_type,
                               ZoneList<TSType*>* arg_types) {
  if (callee_type == nullptr) return false;

  if (!callee_type->IsFunction()) {
    ReportError("Value is not callable", kNoSourcePosition);
    return false;
  }

  FunctionType* func_type = callee_type->AsFunctionType();
  const ZoneList<TSType*>* param_types = func_type->ParameterTypes();
  int param_count = param_types ? param_types->size() : 0;
  int arg_count = arg_types ? arg_types->size() : 0;

  if (arg_count > param_count && !func_type->HasRestParameter()) {
    ReportError("Expected " + std::to_string(param_count) +
                    " arguments but got " + std::to_string(arg_count),
                kNoSourcePosition);
    return false;
  }

  bool all_valid = true;
  for (int i = 0; i < arg_count && i < param_count; i++) {
    if (!IsAssignableTo(arg_types->at(i), param_types->at(i))) {
      ReportError("Argument " + std::to_string(i + 1) +
                      " type mismatch: expected '" +
                      std::string(param_types->at(i)->Name()) +
                      "' but got '" +
                      std::string(arg_types->at(i)->Name()) + "'",
                  kNoSourcePosition);
      all_valid = false;
    }
  }

  return all_valid;
}

bool TSTypeChecker::CheckPropertyAccess(TSType* object_type,
                                         const char* property_name) {
  if (object_type == nullptr) {
    ReportError("Cannot access property on null type", kNoSourcePosition);
    return false;
  }

  if (object_type->IsAny()) return true;

  if (object_type->IsObject()) {
    ObjectType* obj = object_type->AsObjectType();
    bool found = obj->HasProperty(property_name);
    if (!found && strict_mode_) {
      ReportError("Property '" + std::string(property_name) +
                      "' does not exist on type '" +
                      std::string(object_type->Name()) + "'",
                  kNoSourcePosition);
      return false;
    }
    return true;
  }

  if (check_nulls_ &&
      (object_type->IsNull() || object_type->IsUndefined())) {
    ReportError("Cannot access property on " +
                    std::string(object_type->Name()) + " value",
                kNoSourcePosition);
    return false;
  }

  if (object_type->IsArray()) {
    return true;
  }

  if (strict_mode_) {
    ReportError("Cannot access property on type '" +
                    std::string(object_type->Name()) + "'",
                kNoSourcePosition);
    return false;
  }

  return true;
}

bool TSTypeChecker::CheckReturn(TSType* function_return_type,
                                 Expression* return_value) {
  if (return_value == nullptr) {
    if (function_return_type != nullptr &&
        !function_return_type->IsUndefined()) {
      ReportError("Function should return a value", kNoSourcePosition);
      return false;
    }
    return true;
  }

  TSType* value_type = CheckExpression(return_value);

  if (function_return_type == nullptr) {
    return true;
  }

  return IsAssignableTo(value_type, function_return_type);
}

TSType* TSTypeChecker::CheckBinaryOperation(int op, TSType* left,
                                              TSType* right) {
  Token::Value token_op = static_cast<Token::Value>(op);

  if (token_op == Token::kAdd) {
    if (left->IsString() || right->IsString()) {
      return type_system_->GetStringType();
    }
    if (left->IsNumber() && right->IsNumber()) {
      return type_system_->GetNumberType();
    }
    if (left->IsBigInt() && right->IsBigInt()) {
      return type_system_->GetBigIntType();
    }
    if (left->IsAny() || right->IsAny()) {
      return type_system_->GetAnyType();
    }
    if (strict_mode_) {
      ReportError("Operator '+' cannot be applied to types '" +
                      std::string(left->Name()) + "' and '" +
                      std::string(right->Name()) + "'",
                  kNoSourcePosition);
    }
    return type_system_->GetAnyType();
  }

  if (token_op == Token::kSub || token_op == Token::kMul ||
      token_op == Token::kDiv || token_op == Token::kMod) {
    if (left->IsNumber() && right->IsNumber()) {
      return type_system_->GetNumberType();
    }
    if (left->IsBigInt() && right->IsBigInt()) {
      return type_system_->GetBigIntType();
    }
    if (left->IsAny() || right->IsAny()) {
      return type_system_->GetAnyType();
    }
    if (strict_mode_) {
      const char* op_str = "";
      switch (token_op) {
        case Token::kSub:
          op_str = "-";
          break;
        case Token::kMul:
          op_str = "*";
          break;
        case Token::kDiv:
          op_str = "/";
          break;
        case Token::kMod:
          op_str = "%";
          break;
        default:
          op_str = "?";
          break;
      }
      ReportError("Operator '" + std::string(op_str) +
                      "' cannot be applied to types '" +
                      std::string(left->Name()) + "' and '" +
                      std::string(right->Name()) + "'",
                  kNoSourcePosition);
    }
    return type_system_->GetAnyType();
  }

  if (token_op == Token::kExp) {
    if (left->IsNumber() && right->IsNumber()) {
      return type_system_->GetNumberType();
    }
    if (left->IsAny() || right->IsAny()) {
      return type_system_->GetAnyType();
    }
    return type_system_->GetAnyType();
  }

  if (token_op == Token::kAnd) {
    return NarrowFromTruthiness(left, false);
  }

  if (token_op == Token::kOr) {
    return NarrowFromTruthiness(left, true);
  }

  if (token_op == Token::kNullish) {
    return type_system_->CreateUnionType(left, right);
  }

  if (token_op == Token::kBitOr || token_op == Token::kBitXor ||
      token_op == Token::kBitAnd || token_op == Token::kShl ||
      token_op == Token::kSar || token_op == Token::kShr) {
    if (left->IsNumber() && right->IsNumber()) {
      return type_system_->GetNumberType();
    }
    if (left->IsBigInt() && right->IsBigInt()) {
      return type_system_->GetBigIntType();
    }
    if (left->IsAny() || right->IsAny()) {
      return type_system_->GetAnyType();
    }
    if (strict_mode_) {
      ReportError("Bitwise operator cannot be applied to types '" +
                      std::string(left->Name()) + "' and '" +
                      std::string(right->Name()) + "'",
                  kNoSourcePosition);
    }
    return type_system_->GetAnyType();
  }

  if (token_op == Token::kComma) {
    return right;
  }

  return type_system_->GetAnyType();
}

TSType* TSTypeChecker::CheckComparison(int op, TSType* left,
                                        TSType* right) {
  Token::Value token_op = static_cast<Token::Value>(op);

  if (token_op == Token::kEq || token_op == Token::kEqStrict ||
      token_op == Token::kNotEq || token_op == Token::kNotEqStrict) {
    bool both_null = (left->IsNull() || left->IsUndefined()) &&
                     (right->IsNull() || right->IsUndefined());
    if (both_null) {
      return type_system_->GetBooleanType();
    }

    if (left->IsAny() || right->IsAny()) {
      return type_system_->GetBooleanType();
    }

    if (left->IsAssignableTo(right) || right->IsAssignableTo(left)) {
      return type_system_->GetBooleanType();
    }

    if (strict_mode_) {
      bool is_strict_eq =
          (token_op == Token::kEq || token_op == Token::kEqStrict);
      ReportError("Equality comparison of types '" +
                      std::string(left->Name()) + "' and '" +
                      std::string(right->Name()) +
                      "' will always be " +
                      std::string(is_strict_eq ? "false" : "true"),
                  kNoSourcePosition);
    }
    return type_system_->GetBooleanType();
  }

  if (token_op == Token::kLessThan || token_op == Token::kGreaterThan ||
      token_op == Token::kLessThanEq ||
      token_op == Token::kGreaterThanEq) {
    if (left->IsNumber() && right->IsNumber()) {
      return type_system_->GetBooleanType();
    }
    if (left->IsString() && right->IsString()) {
      return type_system_->GetBooleanType();
    }
    if (left->IsBigInt() && right->IsBigInt()) {
      return type_system_->GetBooleanType();
    }
    if (left->IsAny() || right->IsAny()) {
      return type_system_->GetBooleanType();
    }
    if (strict_mode_) {
      ReportError("Comparison operator cannot be applied to types '" +
                      std::string(left->Name()) + "' and '" +
                      std::string(right->Name()) + "'",
                  kNoSourcePosition);
    }
    return type_system_->GetBooleanType();
  }

  if (token_op == Token::kInstanceOf) {
    return type_system_->GetBooleanType();
  }

  if (token_op == Token::kIn) {
    return type_system_->GetBooleanType();
  }

  return type_system_->GetBooleanType();
}

void TSTypeChecker::ReportError(const char* message, int position) {
  error_count_++;
  if (position == kNoSourcePosition) {
    fprintf(stderr, "TS Error: %s\n", message);
  } else {
    fprintf(stderr, "TS Error at position %d: %s\n", position, message);
  }
}

void TSTypeChecker::ReportWarning(const char* message, int position) {
  warning_count_++;
  if (position == kNoSourcePosition) {
    fprintf(stderr, "TS Warning: %s\n", message);
  } else {
    fprintf(stderr, "TS Warning at position %d: %s\n", position, message);
  }
}

void TSTypeChecker::EnterFunctionContext(FunctionLiteral* function) {
  current_function_ = function;
  EnterBlockContext();
}

void TSTypeChecker::ExitFunctionContext() {
  ExitBlockContext();
  current_function_ = nullptr;
}

void TSTypeChecker::EnterBlockContext() {
  ZoneList<PropertyDescriptor*>* props =
      new (zone_) ZoneList<PropertyDescriptor*>(0, zone_);
  context_stack_.Add(type_system_->CreateObjectType(props));
}

void TSTypeChecker::ExitBlockContext() {
  if (context_stack_.size() > 0) {
    context_stack_.RemoveLast();
  }
}

TSType* TSTypeChecker::NarrowType(TSType* type, Expression* condition,
                                    bool branch_taken) {
  if (type == nullptr) return type;

  if (condition->IsCompareOperation()) {
    CompareOperation* cmp = condition->AsCompareOperation();
    Token::Value op = cmp->op();

    if (op == Token::kEqStrict || op == Token::kEq) {
      Expression* left = cmp->left();
      Expression* right = cmp->right();

      if (left->IsVariableProxy() && right->IsLiteral()) {
        return NarrowFromEquality(type, right, branch_taken);
      }
      if (right->IsVariableProxy() && left->IsLiteral()) {
        return NarrowFromEquality(type, left, branch_taken);
      }
    }

    if (op == Token::kInstanceOf) {
      Expression* class_expr = cmp->right();
      return NarrowFromInstanceOf(type, class_expr);
    }
  }

  if (condition->IsUnaryOperation()) {
    UnaryOperation* unary = condition->AsUnaryOperation();
    if (unary->op() == Token::kNot) {
      return NarrowType(type, unary->expression(), !branch_taken);
    }
  }

  if (condition->IsBinaryOperation()) {
    BinaryOperation* binop = condition->AsBinaryOperation();
    if (binop->op() == Token::kAnd) {
      if (branch_taken) {
        TSType* narrowed_left =
            NarrowType(type, binop->left(), true);
        return NarrowType(narrowed_left, binop->right(), true);
      }
    }
    if (binop->op() == Token::kOr) {
      if (!branch_taken) {
        TSType* narrowed_left =
            NarrowType(type, binop->left(), false);
        return NarrowType(narrowed_left, binop->right(), false);
      }
    }
  }

  return NarrowFromTruthiness(type, branch_taken);
}

TSType* TSTypeChecker::NarrowFromEquality(TSType* type,
                                           Expression* compared_value,
                                           bool is_equal) {
  if (!compared_value->IsLiteral()) return type;

  Literal* lit = compared_value->AsLiteral();
  if (lit->type() == Literal::kNull) {
    if (is_equal) {
      return type_system_->GetNullType();
    } else {
      if (type->IsUnion()) {
        return type->AsUnionType()->ExcludeNull();
      }
      if (type->IsNull()) {
        return type_system_->GetNeverType();
      }
      return type;
    }
  }

  if (lit->type() == Literal::kUndefined) {
    if (is_equal) {
      return type_system_->GetUndefinedType();
    } else {
      if (type->IsUnion()) {
        return type->AsUnionType()->ExcludeUndefined();
      }
      if (type->IsUndefined()) {
        return type_system_->GetNeverType();
      }
      return type;
    }
  }

  if (lit->type() == Literal::kBoolean) {
    bool bool_val = lit->AsBooleanLiteral();
    if (is_equal) {
      return bool_val ? type_system_->GetTrueType()
                      : type_system_->GetFalseType();
    }
  }

  if (is_equal) {
    return InferLiteralType(lit);
  }

  return type;
}

TSType* TSTypeChecker::NarrowFromInstanceOf(TSType* type,
                                              Expression* class_expr) {
  if (class_expr->IsProperty()) {
    Property* prop = class_expr->AsProperty();
    if (prop->key()->IsLiteral()) {
      Literal* key_lit = prop->key()->AsLiteral();
      if (key_lit->type() == Literal::kString ||
          key_lit->type() == Literal::kConsString) {
        const AstRawString* class_name = key_lit->AsRawString();
        return type_system_->CreateInstanceOfType(class_name);
      }
    }
  }
  return type;
}

TSType* TSTypeChecker::NarrowFromTruthiness(TSType* type,
                                              bool is_truthy) {
  if (type == nullptr) return type;

  if (type->IsUnion()) {
    UnionType* union_type = type->AsUnionType();
    ZoneList<TSType*>* narrowed =
        new (zone_) ZoneList<TSType*>(union_type->arity(), zone_);

    for (int i = 0; i < union_type->arity(); i++) {
      TSType* constituent = union_type->type_at(i);
      if (is_truthy) {
        if (constituent->IsNull() || constituent->IsUndefined() ||
            constituent->IsNever()) {
          continue;
        }
      } else {
        if (constituent->IsTruthy()) {
          continue;
        }
      }
      narrowed->Add(constituent);
    }

    if (narrowed->size() == 0) {
      return type_system_->GetNeverType();
    }
    if (narrowed->size() == 1) {
      return narrowed->at(0);
    }
    return type_system_->CreateUnionType(narrowed);
  }

  if (is_truthy) {
    if (type->IsNull() || type->IsUndefined() || type->IsNever()) {
      return type_system_->GetNeverType();
    }
    return type;
  }

  if (!is_truthy) {
    if (type->IsTruthy()) {
      return type_system_->GetNeverType();
    }
    return type;
  }

  return type;
}

bool TSTypeChecker::IsAssignableTo(TSType* source, TSType* target) {
  if (source == nullptr || target == nullptr) return false;

  if (source->IsAny() || target->IsAny()) return true;

  if (source->IsNever()) return true;

  if (target->IsNever()) return source->IsNever();

  if (source->IsUnknown()) return target->IsUnknown();

  if (source == target) return true;

  if (source->IsNumber() && target->IsNumber()) return true;
  if (source->IsString() && target->IsString()) return true;
  if (source->IsBoolean() && target->IsBoolean()) return true;
  if (source->IsBigInt() && target->IsBigInt()) return true;
  if (source->IsNull() && target->IsNull()) return true;
  if (source->IsUndefined() && target->IsUndefined()) return true;

  if (source->IsLiteral()) {
    if (source->IsNumberLiteral() && target->IsNumber()) return true;
    if (source->IsStringLiteral() && target->IsString()) return true;
    if (source->IsBooleanLiteral() && target->IsBoolean()) return true;
  }

  if (source->IsUnion()) {
    UnionType* union_source = source->AsUnionType();
    for (int i = 0; i < union_source->arity(); i++) {
      if (!IsAssignableTo(union_source->type_at(i), target)) {
        return false;
      }
    }
    return true;
  }

  if (target->IsUnion()) {
    UnionType* union_target = target->AsUnionType();
    for (int i = 0; i < union_target->arity(); i++) {
      if (IsAssignableTo(source, union_target->type_at(i))) {
        return true;
      }
    }
    return false;
  }

  if (source->IsIntersection()) {
    IntersectionType* intersection = source->AsIntersectionType();
    for (int i = 0; i < intersection->arity(); i++) {
      if (IsAssignableTo(intersection->type_at(i), target)) {
        return true;
      }
    }
    return false;
  }

  if (target->IsIntersection()) {
    IntersectionType* intersection = target->AsIntersectionType();
    for (int i = 0; i < intersection->arity(); i++) {
      if (!IsAssignableTo(source, intersection->type_at(i))) {
        return false;
      }
    }
    return true;
  }

  if (source->IsObject() && target->IsObject()) {
    return IsStructuralSubtype(source, target);
  }

  if (source->IsFunction() && target->IsFunction()) {
    FunctionType* src_func = source->AsFunctionType();
    FunctionType* tgt_func = target->AsFunctionType();

    const ZoneList<TSType*>* src_params = src_func->ParameterTypes();
    const ZoneList<TSType*>* tgt_params = tgt_func->ParameterTypes();
    int param_count = tgt_params ? tgt_params->size() : 0;

    if (src_params && static_cast<int>(src_params->size()) < param_count) {
      return false;
    }

    for (int i = 0; i < param_count; i++) {
      TSType* src_param = src_params ? src_params->at(i) : nullptr;
      TSType* tgt_param = tgt_params->at(i);
      if (src_param != nullptr &&
          !IsAssignableTo(tgt_param, src_param)) {
        return false;
      }
    }

    return IsAssignableTo(src_func->ReturnType(),
                          tgt_func->ReturnType());
  }

  if (source->IsArray() && target->IsArray()) {
    ArrayType* src_arr = source->AsArrayType();
    ArrayType* tgt_arr = target->AsArrayType();
    return IsAssignableTo(src_arr->ElementType(),
                          tgt_arr->ElementType());
  }

  if (source->IsArray() && target->IsObject()) {
    return IsStructuralSubtype(source, target);
  }

  if (source->IsObject() && target->IsArray()) {
    return false;
  }

  if (source->IsConditionalType()) {
    return IsAssignableTo(source->AsConditionalType()->DefaultType(),
                          target);
  }

  return false;
}

bool TSTypeChecker::IsStructuralSubtype(TSType* source, TSType* target) {
  if (!source->IsObject() || !target->IsObject()) return false;

  ObjectType* src_obj = source->AsObjectType();
  ObjectType* tgt_obj = target->AsObjectType();

  ZoneList<PropertyDescriptor*>* target_props = tgt_obj->Properties();
  if (target_props == nullptr) return true;

  for (int i = 0; i < target_props->size(); i++) {
    PropertyDescriptor* target_prop = target_props->at(i);
    const AstRawString* target_name = target_prop->GetName();
    PropertyDescriptor* source_prop = src_obj->GetProperty(target_name);

    if (source_prop == nullptr) {
      if (!target_prop->IsOptional()) {
        return false;
      }
      continue;
    }

    if (!CheckPropertyCompatibility(*source_prop, *target_prop)) {
      return false;
    }
  }

  if (tgt_obj->HasIndexSignature() && !src_obj->HasIndexSignature()) {
    return false;
  }

  if (tgt_obj->HasCallSignature() && !src_obj->HasCallSignature()) {
    return false;
  }

  return true;
}

bool TSTypeChecker::CheckPropertyCompatibility(
    const PropertyDescriptor& source_prop,
    const PropertyDescriptor& target_prop) {
  TSType* src_type = source_prop.GetType();
  TSType* tgt_type = target_prop.GetType();

  if (source_prop.IsReadonly() && !target_prop.IsReadonly()) {
    return IsAssignableTo(tgt_type, src_type);
  }

  if (!source_prop.IsReadonly() && target_prop.IsReadonly()) {
    return IsAssignableTo(src_type, tgt_type);
  }

  return IsAssignableTo(src_type, tgt_type);
}

}  // namespace ts
}  // namespace internal
}  // namespace v8