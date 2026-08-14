#include "src/ts/ts-pipeline.h"

#include <algorithm>
#include <cstring>

#include "src/ast/ast.h"
#include "src/ast/ast-value-factory.h"
#include "src/ast/scopes.h"
#include "src/base/logging.h"
#include "src/common/globals.h"
#include "src/handles/handles.h"
#include "src/objects/scope-info.h"
#include "src/objects/script.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parser.h"
#include "src/parsing/scanner.h"
#include "src/parsing/token.h"
#include "src/zone/zone-list-inl.h"

namespace v8 {
namespace internal {
namespace ts {

TSPipeline::TSPipeline(const TSCompilationConfig& config)
    : config_(config),
      type_system_(nullptr),
      type_checker_(nullptr),
      ts_parser_(nullptr),
      variable_types_(nullptr),
      parameter_types_(nullptr),
      function_return_type_(nullptr),
      map_hints_(nullptr),
      is_typescript_(config.is_typescript),
      zone_(nullptr) {}

bool TSPipeline::IsTypeScriptFile(const char* filename) {
  if (filename == nullptr) return false;
  size_t len = strlen(filename);
  if (len < 3) return false;

  const char* ext = filename + len - 3;
  if (strcmp(ext, ".ts") == 0) return true;

  if (len >= 5) {
    const char* ext5 = filename + len - 5;
    if (strcmp(ext5, ".tsx") == 0) return true;
  }

  if (len >= 5) {
    const char* ext5 = filename + len - 5;
    if (strcmp(ext5, ".mts") == 0) return true;
  }

  if (len >= 5) {
    const char* ext5 = filename + len - 5;
    if (strcmp(ext5, ".cts") == 0) return true;
  }

  return false;
}

bool TSPipeline::HasTypeScriptSyntax(const char* source, size_t length) {
  if (source == nullptr || length == 0) return false;

  static const char* ts_keywords[] = {
      "interface", "type ",  "enum ",  "namespace", "import ",
      "export ",   "implements", "abstract", "readonly", "as ",
      "satisfies", "keyof", "infer",   "declare ", "module ",
      "unique",
  };

  static const int num_keywords =
      sizeof(ts_keywords) / sizeof(ts_keywords[0]);

  for (int i = 0; i < num_keywords; i++) {
    size_t kw_len = strlen(ts_keywords[i]);
    if (kw_len > length) continue;
    for (size_t j = 0; j <= length - kw_len; j++) {
      bool match = true;
      for (size_t k = 0; k < kw_len; k++) {
        if (source[j + k] != ts_keywords[i][k]) {
          match = false;
          break;
        }
      }
      if (match) {
        if (j > 0) {
          char prev = source[j - 1];
          if (isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
            continue;
          }
        }
        if (j + kw_len < length) {
          char next = source[j + kw_len];
          if (isalnum(static_cast<unsigned char>(next)) || next == '_') {
            continue;
          }
        }
        return true;
      }
    }
  }

  for (size_t i = 0; i < length - 1; i++) {
    if (source[i] == ':' && (source[i + 1] == ' ' || source[i + 1] == '\t')) {
      if (i > 0) {
        char prev = source[i - 1];
        if (isalpha(static_cast<unsigned char>(prev)) || prev == '_') {
          return true;
        }
      }
    }
  }

  for (size_t i = 0; i < length - 2; i++) {
    if (source[i] == '<' && source[i + 1] == 'T' && source[i + 2] == '>') {
      return true;
    }
  }

  return false;
}

bool TSPipeline::DetectTypeScript(ParseInfo* info) {
  if (config_.is_typescript) return true;

  if (info == nullptr) return is_typescript_;

  return is_typescript_;
}

void TSPipeline::Initialize(ParseInfo* info) {
  if (info == nullptr) return;

  zone_ = info->zone();
  is_typescript_ = DetectTypeScript(info);

  type_system_ = zone_->New<TSTypeSystem>(zone_);
  type_system_->Initialize(zone_);

  type_checker_ = zone_->New<TSTypeChecker>(zone_, type_system_);
  type_checker_->SetStrictMode(config_.strict_mode);
  type_checker_->SetCheckNulls(config_.check_nulls);
  type_checker_->SetCheckImplicitAny(config_.check_implicit_any);

  if (is_typescript_) {
    ts_parser_ = new TSParser(nullptr, info);
  } else {
    ts_parser_ = nullptr;
  }

  variable_types_ = zone_->New<ZoneList<std::pair<const char*, TSType*>>>(
      0, zone_);
  parameter_types_ =
      zone_->New<ZoneList<std::pair<int, TSType*>>>(0, zone_);
  map_hints_ = zone_->New<ZoneList<MapCreationHint>>(0, zone_);
}

bool TSPipeline::Compile(
    Isolate* isolate, ParseInfo* info, DirectHandle<Script> script,
    MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info) {
  if (info == nullptr || zone_ == nullptr) return false;

  if (!ParseStage(isolate, info, script, maybe_outer_scope_info)) {
    return false;
  }

  FunctionLiteral* program = info->literal();
  if (program == nullptr) return false;

  if (is_typescript_ && config_.type_check) {
    if (!TypeCheckStage(program)) {
      return false;
    }
  }

  if (is_typescript_) {
    AnnotateASTStage(program);
  }

  if (!BytecodeGenerationStage(isolate, info, program)) {
    return false;
  }

  return true;
}

bool TSPipeline::ParseStage(
    Isolate* isolate, ParseInfo* info, DirectHandle<Script> script,
    MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info) {
  if (is_typescript_ && ts_parser_ != nullptr) {
    ts_parser_->ParseProgram(isolate, script, info,
                             maybe_outer_scope_info);
  } else {
    Parser parser(nullptr, info);
    parser.ParseProgram(isolate, script, info, maybe_outer_scope_info);
  }

  return info->literal() != nullptr ||
         !info->pending_error_handler()->has_pending_errors();
}

bool TSPipeline::TypeCheckStage(FunctionLiteral* program) {
  if (program == nullptr) return false;

  type_checker_->CheckProgram(program);

  CollectCheckerInferredTypes(program);

  return type_checker_->error_count() == 0 || !config_.strict_mode;
}

bool TSPipeline::AnnotateASTStage(FunctionLiteral* program) {
  if (program == nullptr) return false;

  CollectTypeAnnotations(program);

  PropagateTypes(program);

  GenerateMapHints(program);

  GenerateVariableTypeMap(program);

  return true;
}

bool TSPipeline::BytecodeGenerationStage(Isolate* isolate, ParseInfo* info,
                                          FunctionLiteral* program) {
  if (isolate == nullptr || info == nullptr || program == nullptr) {
    return false;
  }

  return true;
}

void TSPipeline::CollectTypeAnnotations(FunctionLiteral* root) {
  if (root == nullptr || zone_ == nullptr) return;

  ZoneList<TSType*>* collected =
      zone_->New<ZoneList<TSType*>>(0, zone_);

  CollectTypeAnnotationsRecursive(root, collected);

  if (collected->length() > 0) {
    TSType* first = collected->at(0);
    if (first != nullptr) {
      function_return_type_ = first;
    }
  }
}

void TSPipeline::CollectTypeAnnotationsRecursive(AstNode* node,
                                                 ZoneList<TSType*>* types) {
  if (node == nullptr || types == nullptr) return;

  switch (node->node_type()) {
    case AstNode::kFunctionLiteral: {
      FunctionLiteral* func = node->AsFunctionLiteral();
      if (func->scope() != nullptr) {
        DeclarationScope* scope = func->scope();
        int param_count = func->parameter_count();
        for (int i = 0; i < param_count; i++) {
          Variable* var = scope->parameter(i);
          if (var != nullptr) {
            TSType* param_type = type_system_->NewAny();
            types->Add(param_type, zone_);
            parameter_types_->Add(std::make_pair(i, param_type), zone_);
          }
        }
      }
      if (func->body() != nullptr) {
        ZonePtrList<Statement>* body = func->body();
        for (int i = 0; i < static_cast<int>(body->size()); i++) {
          CollectTypeAnnotationsRecursive((*body)[i], types);
        }
      }
      break;
    }

    case AstNode::kVariableDeclaration: {
      VariableDeclaration* var_decl = node->AsVariableDeclaration();
      Variable* var = var_decl->var();
      if (var != nullptr) {
        const char* var_name =
            reinterpret_cast<const char*>(var->raw_name()->raw_data());
        TSType* inferred = type_system_->NewAny();
        if (var_decl->init() != nullptr) {
          inferred = InferExpressionType(var_decl->init());
        }
        variable_types_->Add(std::make_pair(var_name, inferred), zone_);
      }
      if (var_decl->init() != nullptr) {
        CollectTypeAnnotationsRecursive(var_decl->init(), types);
      }
      break;
    }

    case AstNode::kAssignment: {
      Assignment* assign = node->AsAssignment();
      if (assign->target() != nullptr) {
        CollectTypeAnnotationsRecursive(assign->target(), types);
      }
      if (assign->value() != nullptr) {
        CollectTypeAnnotationsRecursive(assign->value(), types);
      }
      break;
    }

    case AstNode::kCall: {
      Call* call = node->AsCall();
      if (call->expression() != nullptr) {
        CollectTypeAnnotationsRecursive(call->expression(), types);
      }
      ZonePtrList<Expression>* args = call->arguments();
      if (args != nullptr) {
        for (int i = 0; i < static_cast<int>(args->size()); i++) {
          CollectTypeAnnotationsRecursive((*args)[i], types);
        }
      }
      break;
    }

    case AstNode::kReturnStatement: {
      ReturnStatement* ret = node->AsReturnStatement();
      if (ret->expression() != nullptr) {
        TSType* ret_type = InferExpressionType(ret->expression());
        if (ret_type != nullptr) {
          types->Add(ret_type, zone_);
        }
        CollectTypeAnnotationsRecursive(ret->expression(), types);
      }
      break;
    }

    case AstNode::kIfStatement: {
      IfStatement* if_stmt = node->AsIfStatement();
      if (if_stmt->condition() != nullptr) {
        CollectTypeAnnotationsRecursive(if_stmt->condition(), types);
      }
      if (if_stmt->then_statement() != nullptr) {
        CollectTypeAnnotationsRecursive(if_stmt->then_statement(), types);
      }
      if (if_stmt->else_statement() != nullptr) {
        CollectTypeAnnotationsRecursive(if_stmt->else_statement(), types);
      }
      break;
    }

    case AstNode::kForStatement: {
      ForStatement* for_stmt = node->AsForStatement();
      if (for_stmt->init() != nullptr) {
        CollectTypeAnnotationsRecursive(for_stmt->init(), types);
      }
      if (for_stmt->condition() != nullptr) {
        CollectTypeAnnotationsRecursive(for_stmt->condition(), types);
      }
      if (for_stmt->next() != nullptr) {
        CollectTypeAnnotationsRecursive(for_stmt->next(), types);
      }
      if (for_stmt->body() != nullptr) {
        CollectTypeAnnotationsRecursive(for_stmt->body(), types);
      }
      break;
    }

    case AstNode::kWhileStatement: {
      WhileStatement* while_stmt = node->AsWhileStatement();
      if (while_stmt->condition() != nullptr) {
        CollectTypeAnnotationsRecursive(while_stmt->condition(), types);
      }
      if (while_stmt->body() != nullptr) {
        CollectTypeAnnotationsRecursive(while_stmt->body(), types);
      }
      break;
    }

    case AstNode::kBlock: {
      Block* block = node->AsBlock();
      if (block->statements() != nullptr) {
        ZonePtrList<Statement>* stmts = block->statements();
        for (int i = 0; i < static_cast<int>(stmts->size()); i++) {
          CollectTypeAnnotationsRecursive((*stmts)[i], types);
        }
      }
      break;
    }

    case AstNode::kExpressionStatement: {
      ExpressionStatement* expr_stmt = node->AsExpressionStatement();
      if (expr_stmt->expression() != nullptr) {
        CollectTypeAnnotationsRecursive(expr_stmt->expression(), types);
      }
      break;
    }

    case AstNode::kBinaryOperation: {
      BinaryOperation* binop = node->AsBinaryOperation();
      if (binop->left() != nullptr) {
        CollectTypeAnnotationsRecursive(binop->left(), types);
      }
      if (binop->right() != nullptr) {
        CollectTypeAnnotationsRecursive(binop->right(), types);
      }
      break;
    }

    case AstNode::kProperty: {
      Property* prop = node->AsProperty();
      if (prop->obj() != nullptr) {
        CollectTypeAnnotationsRecursive(prop->obj(), types);
      }
      if (prop->key() != nullptr) {
        CollectTypeAnnotationsRecursive(prop->key(), types);
      }
      break;
    }

    case AstNode::kObjectLiteral: {
      ObjectLiteral* obj = node->AsObjectLiteral();
      if (obj->properties() != nullptr) {
        ZonePtrList<ObjectLiteralProperty>* props = obj->properties();
        for (int i = 0; i < static_cast<int>(props->size()); i++) {
          ObjectLiteralProperty* prop = (*props)[i];
          if (prop->value() != nullptr) {
            CollectTypeAnnotationsRecursive(prop->value(), types);
          }
        }
      }
      break;
    }

    case AstNode::kArrayLiteral: {
      ArrayLiteral* arr = node->AsArrayLiteral();
      if (arr->elements() != nullptr) {
        ZonePtrList<Expression>* elems = arr->elements();
        for (int i = 0; i < static_cast<int>(elems->size()); i++) {
          CollectTypeAnnotationsRecursive((*elems)[i], types);
        }
      }
      break;
    }

    case AstNode::kConditional: {
      Conditional* cond = node->AsConditional();
      if (cond->condition() != nullptr) {
        CollectTypeAnnotationsRecursive(cond->condition(), types);
      }
      if (cond->then_expression() != nullptr) {
        CollectTypeAnnotationsRecursive(cond->then_expression(), types);
      }
      if (cond->else_expression() != nullptr) {
        CollectTypeAnnotationsRecursive(cond->else_expression(), types);
      }
      break;
    }

    case AstNode::kThrow: {
      Throw* throw_expr = node->AsThrow();
      if (throw_expr->expression() != nullptr) {
        CollectTypeAnnotationsRecursive(throw_expr->expression(), types);
      }
      break;
    }

    case AstNode::kTryCatchStatement: {
      TryCatchStatement* try_stmt = node->AsTryCatchStatement();
      if (try_stmt->try_block() != nullptr) {
        CollectTypeAnnotationsRecursive(try_stmt->try_block(), types);
      }
      if (try_stmt->catch_block() != nullptr) {
        CollectTypeAnnotationsRecursive(try_stmt->catch_block(), types);
      }
      if (try_stmt->finally_block() != nullptr) {
        CollectTypeAnnotationsRecursive(try_stmt->finally_block(), types);
      }
      break;
    }

    case AstNode::kFunctionDeclaration: {
      FunctionDeclaration* func_decl = node->AsFunctionDeclaration();
      if (func_decl->fun() != nullptr) {
        CollectTypeAnnotationsRecursive(func_decl->fun(), types);
      }
      break;
    }

    case AstNode::kVariableProxy: {
      VariableProxy* proxy = node->AsVariableProxy();
      if (proxy->is_resolved()) {
        Variable* var = proxy->var();
        if (var != nullptr) {
          const char* var_name =
              reinterpret_cast<const char*>(var->raw_name()->raw_data());
          TSType* var_type = type_system_->NewAny();
          for (int i = 0; i < variable_types_->length(); i++) {
            if (strcmp(variable_types_->at(i).first, var_name) == 0) {
              var_type = variable_types_->at(i).second;
              break;
            }
          }
        }
      }
      break;
    }

    case AstNode::kLiteral:
    case AstNode::kSmi:
    case AstNode::kHeapNumber:
    case AstNode::kString:
    case AstNode::kConsString:
    case AstNode::kBoolean:
    case AstNode::kUndefined:
    case AstNode::kNull:
    case AstNode::kThisExpression:
    case AstNode::kDoWhileStatement:
    case AstNode::kEmptyStatement:
    case AstNode::kDebuggerStatement:
    case AstNode::kBreakStatement:
    case AstNode::kContinueStatement:
    case AstNode::kSwitchStatement:
    case AstNode::kForInStatement:
    case AstNode::kForOfStatement:
    case AstNode::kClassDeclaration:
    case AstNode::kSuperCall:
    case AstNode::kSuperProperty:
    case AstNode::kTemplateLiteral:
    case AstNode::kTaggedTemplate:
    case AstNode::kYield:
    case AstNode::kYieldStar:
    case AstNode::kAwait:
    case AstNode::kSpread:
    case AstNode::kOptionalChain:
    case AstNode::kConditionalChain:
    case AstNode::kCallNew:
    case AstNode::kDelete:
    case AstNode::kTypeOf:
    case AstNode::kVoid:
    case AstNode::kUnaryOperation:
    case AstNode::kCompareOperation:
    case AstNode::kRegExpLiteral:
    case AstNode::kNew:
    case AstNode::kClassLiteral:
    case AstNode::kWhileFor:
      break;
  }
}

void TSPipeline::CollectCheckerInferredTypes(FunctionLiteral* root) {
  if (root == nullptr || zone_ == nullptr) return;
  if (type_checker_ == nullptr) return;

  variable_types_ = zone_->New<ZoneList<std::pair<const char*, TSType*>>>(
      0, zone_);
  parameter_types_ =
      zone_->New<ZoneList<std::pair<int, TSType*>>>(0, zone_);
  function_return_type_ = nullptr;

  CollectCheckerTypesRecursive(root);
}

void TSPipeline::CollectCheckerTypesRecursive(AstNode* node) {
  if (node == nullptr) return;

  switch (node->node_type()) {
    case AstNode::kFunctionLiteral: {
      FunctionLiteral* func = node->AsFunctionLiteral();
      if (func->scope() != nullptr) {
        DeclarationScope* scope = func->scope();
        int param_count = func->parameter_count();
        for (int i = 0; i < param_count; i++) {
          Variable* var = scope->parameter(i);
          if (var != nullptr) {
            TSType* param_type = type_system_->NewAny();
            const char* param_name =
                reinterpret_cast<const char*>(var->raw_name()->raw_data());
            bool found = false;
            for (int j = 0; j < parameter_types_->length(); j++) {
              if (parameter_types_->at(j).first == i) {
                param_type = parameter_types_->at(j).second;
                found = true;
                break;
              }
            }
            if (!found) {
              TSType* checker_type =
                  type_checker_->CheckExpression(func->body());
              if (checker_type != nullptr &&
                  checker_type->kind() != TypeKind::kAny) {
                param_type = checker_type;
              }
              parameter_types_->Add(std::make_pair(i, param_type), zone_);
            }
            variable_types_->Add(std::make_pair(param_name, param_type),
                                 zone_);
          }
        }
      }
      if (func->body() != nullptr) {
        ZonePtrList<Statement>* body = func->body();
        for (int i = 0; i < static_cast<int>(body->size()); i++) {
          CollectCheckerTypesRecursive((*body)[i]);
        }
      }
      break;
    }

    case AstNode::kVariableDeclaration: {
      VariableDeclaration* var_decl = node->AsVariableDeclaration();
      Variable* var = var_decl->var();
      if (var != nullptr) {
        const char* var_name =
            reinterpret_cast<const char*>(var->raw_name()->raw_data());
        TSType* inferred = type_system_->NewAny();
        if (var_decl->init() != nullptr) {
          inferred = type_checker_->CheckExpression(var_decl->init());
          if (inferred == nullptr) {
            inferred = type_system_->NewAny();
          }
        }
        bool found = false;
        for (int i = 0; i < variable_types_->length(); i++) {
          if (strcmp(variable_types_->at(i).first, var_name) == 0) {
            variable_types_->at(i).second = inferred;
            found = true;
            break;
          }
        }
        if (!found) {
          variable_types_->Add(std::make_pair(var_name, inferred), zone_);
        }
      }
      if (var_decl->init() != nullptr) {
        CollectCheckerTypesRecursive(var_decl->init());
      }
      break;
    }

    case AstNode::kAssignment: {
      Assignment* assign = node->AsAssignment();
      if (assign->target() != nullptr) {
        CollectCheckerTypesRecursive(assign->target());
      }
      if (assign->value() != nullptr) {
        TSType* value_type = type_checker_->CheckExpression(assign->value());
        if (value_type != nullptr && assign->target() != nullptr &&
            assign->target()->is_variable()) {
          VariableProxy* proxy = assign->target()->AsVariableProxy();
          if (proxy->is_resolved()) {
            Variable* var = proxy->var();
            const char* var_name =
                reinterpret_cast<const char*>(var->raw_name()->raw_data());
            for (int i = 0; i < variable_types_->length(); i++) {
              if (strcmp(variable_types_->at(i).first, var_name) == 0) {
                variable_types_->at(i).second = value_type;
                break;
              }
            }
          }
        }
        CollectCheckerTypesRecursive(assign->value());
      }
      break;
    }

    case AstNode::kReturnStatement: {
      ReturnStatement* ret = node->AsReturnStatement();
      if (ret->expression() != nullptr) {
        TSType* ret_type = type_checker_->CheckExpression(ret->expression());
        if (ret_type != nullptr) {
          if (function_return_type_ == nullptr ||
              (ret_type->kind() != TypeKind::kAny &&
               function_return_type_->kind() == TypeKind::kAny)) {
            function_return_type_ = ret_type;
          }
        }
        CollectCheckerTypesRecursive(ret->expression());
      }
      break;
    }

    case AstNode::kCall: {
      Call* call = node->AsCall();
      if (call->expression() != nullptr) {
        CollectCheckerTypesRecursive(call->expression());
      }
      ZonePtrList<Expression>* args = call->arguments();
      if (args != nullptr) {
        for (int i = 0; i < static_cast<int>(args->size()); i++) {
          CollectCheckerTypesRecursive((*args)[i]);
        }
      }
      break;
    }

    case AstNode::kIfStatement: {
      IfStatement* if_stmt = node->AsIfStatement();
      if (if_stmt->condition() != nullptr) {
        CollectCheckerTypesRecursive(if_stmt->condition());
      }
      if (if_stmt->then_statement() != nullptr) {
        CollectCheckerTypesRecursive(if_stmt->then_statement());
      }
      if (if_stmt->else_statement() != nullptr) {
        CollectCheckerTypesRecursive(if_stmt->else_statement());
      }
      break;
    }

    case AstNode::kForStatement: {
      ForStatement* for_stmt = node->AsForStatement();
      if (for_stmt->init() != nullptr) {
        CollectCheckerTypesRecursive(for_stmt->init());
      }
      if (for_stmt->condition() != nullptr) {
        CollectCheckerTypesRecursive(for_stmt->condition());
      }
      if (for_stmt->next() != nullptr) {
        CollectCheckerTypesRecursive(for_stmt->next());
      }
      if (for_stmt->body() != nullptr) {
        CollectCheckerTypesRecursive(for_stmt->body());
      }
      break;
    }

    case AstNode::kWhileStatement: {
      WhileStatement* while_stmt = node->AsWhileStatement();
      if (while_stmt->condition() != nullptr) {
        CollectCheckerTypesRecursive(while_stmt->condition());
      }
      if (while_stmt->body() != nullptr) {
        CollectCheckerTypesRecursive(while_stmt->body());
      }
      break;
    }

    case AstNode::kBlock: {
      Block* block = node->AsBlock();
      if (block->statements() != nullptr) {
        ZonePtrList<Statement>* stmts = block->statements();
        for (int i = 0; i < static_cast<int>(stmts->size()); i++) {
          CollectCheckerTypesRecursive((*stmts)[i]);
        }
      }
      break;
    }

    case AstNode::kExpressionStatement: {
      ExpressionStatement* expr_stmt = node->AsExpressionStatement();
      if (expr_stmt->expression() != nullptr) {
        CollectCheckerTypesRecursive(expr_stmt->expression());
      }
      break;
    }

    case AstNode::kBinaryOperation: {
      BinaryOperation* binop = node->AsBinaryOperation();
      if (binop->left() != nullptr) {
        CollectCheckerTypesRecursive(binop->left());
      }
      if (binop->right() != nullptr) {
        CollectCheckerTypesRecursive(binop->right());
      }
      break;
    }

    case AstNode::kProperty: {
      Property* prop = node->AsProperty();
      if (prop->obj() != nullptr) {
        CollectCheckerTypesRecursive(prop->obj());
      }
      if (prop->key() != nullptr) {
        CollectCheckerTypesRecursive(prop->key());
      }
      break;
    }

    case AstNode::kObjectLiteral: {
      ObjectLiteral* obj = node->AsObjectLiteral();
      if (obj->properties() != nullptr) {
        ZonePtrList<ObjectLiteralProperty>* props = obj->properties();
        for (int i = 0; i < static_cast<int>(props->size()); i++) {
          ObjectLiteralProperty* prop = (*props)[i];
          if (prop->value() != nullptr) {
            CollectCheckerTypesRecursive(prop->value());
          }
        }
      }
      break;
    }

    case AstNode::kArrayLiteral: {
      ArrayLiteral* arr = node->AsArrayLiteral();
      if (arr->elements() != nullptr) {
        ZonePtrList<Expression>* elems = arr->elements();
        for (int i = 0; i < static_cast<int>(elems->size()); i++) {
          CollectCheckerTypesRecursive((*elems)[i]);
        }
      }
      break;
    }

    case AstNode::kConditional: {
      Conditional* cond = node->AsConditional();
      if (cond->condition() != nullptr) {
        CollectCheckerTypesRecursive(cond->condition());
      }
      if (cond->then_expression() != nullptr) {
        CollectCheckerTypesRecursive(cond->then_expression());
      }
      if (cond->else_expression() != nullptr) {
        CollectCheckerTypesRecursive(cond->else_expression());
      }
      break;
    }

    case AstNode::kFunctionDeclaration: {
      FunctionDeclaration* func_decl = node->AsFunctionDeclaration();
      if (func_decl->fun() != nullptr) {
        CollectCheckerTypesRecursive(func_decl->fun());
      }
      break;
    }

    case AstNode::kThrow: {
      Throw* throw_expr = node->AsThrow();
      if (throw_expr->expression() != nullptr) {
        CollectCheckerTypesRecursive(throw_expr->expression());
      }
      break;
    }

    case AstNode::kTryCatchStatement: {
      TryCatchStatement* try_stmt = node->AsTryCatchStatement();
      if (try_stmt->try_block() != nullptr) {
        CollectCheckerTypesRecursive(try_stmt->try_block());
      }
      if (try_stmt->catch_block() != nullptr) {
        CollectCheckerTypesRecursive(try_stmt->catch_block());
      }
      if (try_stmt->finally_block() != nullptr) {
        CollectCheckerTypesRecursive(try_stmt->finally_block());
      }
      break;
    }

    case AstNode::kVariableProxy: {
      VariableProxy* proxy = node->AsVariableProxy();
      if (proxy->is_resolved()) {
        Variable* var = proxy->var();
        if (var != nullptr) {
          const char* var_name =
              reinterpret_cast<const char*>(var->raw_name()->raw_data());
          TSType* var_type = type_system_->NewAny();
          for (int i = 0; i < variable_types_->length(); i++) {
            if (strcmp(variable_types_->at(i).first, var_name) == 0) {
              var_type = variable_types_->at(i).second;
              break;
            }
          }
        }
      }
      break;
    }

    case AstNode::kLiteral:
    case AstNode::kSmi:
    case AstNode::kHeapNumber:
    case AstNode::kString:
    case AstNode::kConsString:
    case AstNode::kBoolean:
    case AstNode::kUndefined:
    case AstNode::kNull:
    case AstNode::kThisExpression:
    case AstNode::kDoWhileStatement:
    case AstNode::kEmptyStatement:
    case AstNode::kDebuggerStatement:
    case AstNode::kBreakStatement:
    case AstNode::kContinueStatement:
    case AstNode::kSwitchStatement:
    case AstNode::kForInStatement:
    case AstNode::kForOfStatement:
    case AstNode::kClassDeclaration:
    case AstNode::kSuperCall:
    case AstNode::kSuperProperty:
    case AstNode::kTemplateLiteral:
    case AstNode::kTaggedTemplate:
    case AstNode::kYield:
    case AstNode::kYieldStar:
    case AstNode::kAwait:
    case AstNode::kSpread:
    case AstNode::kOptionalChain:
    case AstNode::kConditionalChain:
    case AstNode::kCallNew:
    case AstNode::kDelete:
    case AstNode::kTypeOf:
    case AstNode::kVoid:
    case AstNode::kUnaryOperation:
    case AstNode::kCompareOperation:
    case AstNode::kRegExpLiteral:
    case AstNode::kNew:
    case AstNode::kClassLiteral:
    case AstNode::kWhileFor:
      break;
  }
}

void TSPipeline::PropagateTypes(FunctionLiteral* root) {
  if (root == nullptr || zone_ == nullptr) return;
  PropagateTypesRecursive(root);
}

void TSPipeline::PropagateTypesRecursive(AstNode* node) {
  if (node == nullptr) return;

  switch (node->node_type()) {
    case AstNode::kFunctionLiteral: {
      FunctionLiteral* func = node->AsFunctionLiteral();
      if (func->body() != nullptr) {
        ZonePtrList<Statement>* body = func->body();
        for (int i = 0; i < static_cast<int>(body->size()); i++) {
          PropagateTypesRecursive((*body)[i]);
        }
      }
      break;
    }

    case AstNode::kVariableDeclaration: {
      VariableDeclaration* var_decl = node->AsVariableDeclaration();
      Variable* var = var_decl->var();
      if (var != nullptr && var_decl->init() != nullptr) {
        TSType* init_type = InferExpressionType(var_decl->init());
        if (init_type != nullptr) {
          const char* var_name =
              reinterpret_cast<const char*>(var->raw_name()->raw_data());
          bool found = false;
          for (int i = 0; i < variable_types_->length(); i++) {
            if (strcmp(variable_types_->at(i).first, var_name) == 0) {
              variable_types_->at(i).second = init_type;
              found = true;
              break;
            }
          }
          if (!found) {
            variable_types_->Add(std::make_pair(var_name, init_type), zone_);
          }
        }
      }
      if (var_decl->init() != nullptr) {
        PropagateTypesRecursive(var_decl->init());
      }
      break;
    }

    case AstNode::kAssignment: {
      Assignment* assign = node->AsAssignment();
      if (assign->value() != nullptr) {
        TSType* value_type = InferExpressionType(assign->value());
        if (assign->target() != nullptr && assign->target()->is_variable()) {
          VariableProxy* proxy = assign->target()->AsVariableProxy();
          if (proxy->is_resolved()) {
            Variable* var = proxy->var();
            const char* var_name =
                reinterpret_cast<const char*>(var->raw_name()->raw_data());
            for (int i = 0; i < variable_types_->length(); i++) {
              if (strcmp(variable_types_->at(i).first, var_name) == 0) {
                variable_types_->at(i).second = value_type;
                break;
              }
            }
          }
        }
      }
      if (assign->target() != nullptr) {
        PropagateTypesRecursive(assign->target());
      }
      if (assign->value() != nullptr) {
        PropagateTypesRecursive(assign->value());
      }
      break;
    }

    case AstNode::kBinaryOperation: {
      BinaryOperation* binop = node->AsBinaryOperation();
      if (binop->left() != nullptr) {
        PropagateTypesRecursive(binop->left());
      }
      if (binop->right() != nullptr) {
        PropagateTypesRecursive(binop->right());
      }
      break;
    }

    case AstNode::kCall: {
      Call* call = node->AsCall();
      if (call->expression() != nullptr) {
        PropagateTypesRecursive(call->expression());
      }
      ZonePtrList<Expression>* args = call->arguments();
      if (args != nullptr) {
        for (int i = 0; i < static_cast<int>(args->size()); i++) {
          PropagateTypesRecursive((*args)[i]);
        }
      }
      break;
    }

    case AstNode::kProperty: {
      Property* prop = node->AsProperty();
      if (prop->obj() != nullptr) {
        PropagateTypesRecursive(prop->obj());
      }
      if (prop->key() != nullptr) {
        PropagateTypesRecursive(prop->key());
      }
      break;
    }

    case AstNode::kBlock: {
      Block* block = node->AsBlock();
      if (block->statements() != nullptr) {
        ZonePtrList<Statement>* stmts = block->statements();
        for (int i = 0; i < static_cast<int>(stmts->size()); i++) {
          PropagateTypesRecursive((*stmts)[i]);
        }
      }
      break;
    }

    case AstNode::kIfStatement: {
      IfStatement* if_stmt = node->AsIfStatement();
      if (if_stmt->condition() != nullptr) {
        PropagateTypesRecursive(if_stmt->condition());
      }
      if (if_stmt->then_statement() != nullptr) {
        PropagateTypesRecursive(if_stmt->then_statement());
      }
      if (if_stmt->else_statement() != nullptr) {
        PropagateTypesRecursive(if_stmt->else_statement());
      }
      break;
    }

    case AstNode::kForStatement: {
      ForStatement* for_stmt = node->AsForStatement();
      if (for_stmt->init() != nullptr) {
        PropagateTypesRecursive(for_stmt->init());
      }
      if (for_stmt->condition() != nullptr) {
        PropagateTypesRecursive(for_stmt->condition());
      }
      if (for_stmt->next() != nullptr) {
        PropagateTypesRecursive(for_stmt->next());
      }
      if (for_stmt->body() != nullptr) {
        PropagateTypesRecursive(for_stmt->body());
      }
      break;
    }

    case AstNode::kWhileStatement: {
      WhileStatement* while_stmt = node->AsWhileStatement();
      if (while_stmt->condition() != nullptr) {
        PropagateTypesRecursive(while_stmt->condition());
      }
      if (while_stmt->body() != nullptr) {
        PropagateTypesRecursive(while_stmt->body());
      }
      break;
    }

    case AstNode::kReturnStatement: {
      ReturnStatement* ret = node->AsReturnStatement();
      if (ret->expression() != nullptr) {
        PropagateTypesRecursive(ret->expression());
      }
      break;
    }

    case AstNode::kExpressionStatement: {
      ExpressionStatement* expr_stmt = node->AsExpressionStatement();
      if (expr_stmt->expression() != nullptr) {
        PropagateTypesRecursive(expr_stmt->expression());
      }
      break;
    }

    case AstNode::kObjectLiteral: {
      ObjectLiteral* obj = node->AsObjectLiteral();
      if (obj->properties() != nullptr) {
        ZonePtrList<ObjectLiteralProperty>* props = obj->properties();
        for (int i = 0; i < static_cast<int>(props->size()); i++) {
          ObjectLiteralProperty* prop = (*props)[i];
          if (prop->value() != nullptr) {
            PropagateTypesRecursive(prop->value());
          }
        }
      }
      break;
    }

    case AstNode::kArrayLiteral: {
      ArrayLiteral* arr = node->AsArrayLiteral();
      if (arr->elements() != nullptr) {
        ZonePtrList<Expression>* elems = arr->elements();
        for (int i = 0; i < static_cast<int>(elems->size()); i++) {
          PropagateTypesRecursive((*elems)[i]);
        }
      }
      break;
    }

    case AstNode::kConditional: {
      Conditional* cond = node->AsConditional();
      if (cond->condition() != nullptr) {
        PropagateTypesRecursive(cond->condition());
      }
      if (cond->then_expression() != nullptr) {
        PropagateTypesRecursive(cond->then_expression());
      }
      if (cond->else_expression() != nullptr) {
        PropagateTypesRecursive(cond->else_expression());
      }
      break;
    }

    case AstNode::kFunctionDeclaration: {
      FunctionDeclaration* func_decl = node->AsFunctionDeclaration();
      if (func_decl->fun() != nullptr) {
        PropagateTypesRecursive(func_decl->fun());
      }
      break;
    }

    default:
      break;
  }
}

TSType* TSPipeline::InferExpressionType(Expression* expr) {
  if (expr == nullptr) return type_system_->NewAny();

  switch (expr->node_type()) {
    case AstNode::kLiteral: {
      Literal* lit = expr->AsLiteral();
      switch (lit->type()) {
        case Literal::kSmi:
        case Literal::kHeapNumber:
          return type_system_->NewNumber();
        case Literal::kString:
        case Literal::kConsString:
          return type_system_->NewString();
        case Literal::kBoolean:
          return type_system_->NewBoolean();
        case Literal::kUndefined:
          return type_system_->NewUndefined();
        case Literal::kNull:
          return config_.check_nulls ? type_system_->NewNull()
                                    : type_system_->NewAny();
        case Literal::kBigInt:
          return type_system_->NewAny();
        case Literal::kTheHole:
          return type_system_->NewUndefined();
      }
      break;
    }

    case AstNode::kBinaryOperation: {
      BinaryOperation* binop = expr->AsBinaryOperation();
      TSType* left_type = InferExpressionType(binop->left());
      TSType* right_type = InferExpressionType(binop->right());
      Token::Value op = binop->op();

      if (op == Token::kAdd) {
        if (left_type->kind() == TypeKind::kString ||
            right_type->kind() == TypeKind::kString) {
          return type_system_->NewString();
        }
        if (left_type->kind() == TypeKind::kNumber &&
            right_type->kind() == TypeKind::kNumber) {
          return type_system_->NewNumber();
        }
        return type_system_->NewNumber();
      }

      if (op == Token::kSub || op == Token::kMul || op == Token::kDiv ||
          op == Token::kMod || op == Token::kExp) {
        return type_system_->NewNumber();
      }

      if (Token::IsCompareOp(op)) {
        return type_system_->NewBoolean();
      }

      if (op == Token::kAnd || op == Token::kOr) {
        return type_system_->PromoteToCommonType(left_type, right_type);
      }

      if (op == Token::kBitOr || op == Token::kBitXor ||
          op == Token::kBitAnd || op == Token::kShl ||
          op == Token::kSar || op == Token::kShr) {
        return type_system_->NewNumber();
      }

      if (op == Token::kNullish) {
        ZoneList<TSType*>* union_types =
            zone_->New<ZoneList<TSType*>>(2, zone_);
        union_types->Add(left_type, zone_);
        union_types->Add(right_type, zone_);
        return type_system_->NewUnion(union_types);
      }

      return left_type;
    }

    case AstNode::kUnaryOperation: {
      UnaryOperation* unary = expr->AsUnaryOperation();
      Token::Value op = unary->op();
      if (op == Token::kNot) {
        return type_system_->NewBoolean();
      }
      if (op == Token::kTypeOf) {
        return type_system_->NewString();
      }
      if (op == Token::kVoid) {
        return type_system_->NewUndefined();
      }
      if (op == Token::kDelete) {
        return type_system_->NewBoolean();
      }
      if (op == Token::kBitNot || op == Token::kNeg || op == Token::kPos) {
        return type_system_->NewNumber();
      }
      return type_system_->NewAny();
    }

    case AstNode::kCompareOperation: {
      return type_system_->NewBoolean();
    }

    case AstNode::kProperty: {
      Property* prop = expr->AsProperty();
      TSType* obj_type = InferExpressionType(prop->obj());
      if (obj_type->kind() == TypeKind::kAny) {
        return type_system_->NewAny();
      }
      if (obj_type->kind() == TypeKind::kString) {
        if (prop->key()->IsVariableProxy()) {
          const AstRawString* name = prop->key()->AsVariableProxy()->raw_name();
          if (name->IsOneByteEqualTo("length")) {
            return type_system_->NewNumber();
          }
        }
        return type_system_->NewAny();
      }
      if (obj_type->kind() == TypeKind::kNumber) {
        return type_system_->NewAny();
      }
      if (obj_type->kind() == TypeKind::kArray) {
        if (prop->key()->IsVariableProxy()) {
          const AstRawString* name = prop->key()->AsVariableProxy()->raw_name();
          if (name->IsOneByteEqualTo("length")) {
            return type_system_->NewNumber();
          }
        }
        if (obj_type->GetElementType() != nullptr) {
          return obj_type->GetElementType();
        }
        return type_system_->NewAny();
      }
      return type_system_->NewAny();
    }

    case AstNode::kCall: {
      Call* call = expr->AsCall();
      TSType* callee_type = InferExpressionType(call->expression());
      if (callee_type->kind() == TypeKind::kFunction) {
        if (callee_type->GetReturnType() != nullptr) {
          return callee_type->GetReturnType();
        }
      }
      return type_system_->NewAny();
    }

    case AstNode::kCallNew: {
      CallNew* new_call = expr->AsCallNew();
      TSType* ctor_type = InferExpressionType(new_call->expression());
      if (ctor_type->kind() == TypeKind::kObject) {
        return ctor_type;
      }
      return type_system_->NewObject();
    }

    case AstNode::kConditional: {
      Conditional* cond = expr->AsConditional();
      TSType* then_type = InferExpressionType(cond->then_expression());
      TSType* else_type = InferExpressionType(cond->else_expression());
      return type_system_->PromoteToCommonType(then_type, else_type);
    }

    case AstNode::kArrayLiteral: {
      ArrayLiteral* arr = expr->AsArrayLiteral();
      ZonePtrList<Expression>* elems = arr->elements();
      if (elems == nullptr || elems->size() == 0) {
        return type_system_->NewArray(type_system_->NewAny());
      }
      TSType* elem_type = InferExpressionType((*elems)[0]);
      return type_system_->NewArray(elem_type);
    }

    case AstNode::kObjectLiteral: {
      return type_system_->NewObject();
    }

    case AstNode::kFunctionLiteral: {
      FunctionLiteral* func = expr->AsFunctionLiteral();
      ZoneList<TSType*>* param_types =
          zone_->New<ZoneList<TSType*>>(func->parameter_count(), zone_);
      for (int i = 0; i < func->parameter_count(); i++) {
        param_types->Add(type_system_->NewAny(), zone_);
      }
      return type_system_->NewFunction(param_types, type_system_->NewAny());
    }

    case AstNode::kVariableProxy: {
      VariableProxy* proxy = expr->AsVariableProxy();
      if (proxy->is_resolved()) {
        Variable* var = proxy->var();
        const char* var_name =
            reinterpret_cast<const char*>(var->raw_name()->raw_data());
        for (int i = 0; i < variable_types_->length(); i++) {
          if (strcmp(variable_types_->at(i).first, var_name) == 0) {
            return variable_types_->at(i).second;
          }
        }
      }
      return type_system_->NewAny();
    }

    case AstNode::kThisExpression: {
      return type_system_->NewThis();
    }

    case AstNode::kTemplateLiteral: {
      return type_system_->NewString();
    }

    case AstNode::kTaggedTemplate: {
      return type_system_->NewAny();
    }

    case AstNode::kSpread: {
      return type_system_->NewAny();
    }

    case AstNode::kAwait: {
      Await* await_expr = expr->AsAwait();
      TSType* awaited = InferExpressionType(await_expr->expression());
      if (awaited->kind() == TypeKind::kPromise &&
          awaited->GetElementType() != nullptr) {
        return awaited->GetElementType();
      }
      return awaited;
    }

    case AstNode::kYield:
    case AstNode::kYieldStar:
      return type_system_->NewAny();

    case AstNode::kThrow:
      return type_system_->NewNever();

    case AstNode::kNew:
      return type_system_->NewObject();

    case AstNode::kDelete:
      return type_system_->NewBoolean();

    case AstNode::kTypeOf:
      return type_system_->NewString();

    case AstNode::kVoid:
      return type_system_->NewUndefined();

    case AstNode::kConditionalChain:
    case AstNode::kOptionalChain:
      return type_system_->NewAny();

    default:
      return type_system_->NewAny();
  }
}

void TSPipeline::GenerateMapHints(FunctionLiteral* root) {
  if (root == nullptr || zone_ == nullptr) return;

  ZoneList<ObjectLiteral**> object_literals(0, zone_);
  CollectObjectLiterals(root, &object_literals);

  for (int i = 0; i < object_literals.length(); i++) {
    ObjectLiteral* obj = object_literals.at(i);
    if (obj == nullptr) continue;

    ZonePtrList<ObjectLiteralProperty>* props = obj->properties();
    if (props == nullptr || props->size() == 0) continue;

    MapCreationHint hint;
    hint.name = "__anonymous__";
    hint.properties =
        zone_->New<ZoneList<PropertyDescriptor>>(props->size(), zone_);
    hint.is_stable = true;
    hint.expected_inobject_properties = static_cast<int>(props->size());

    for (int j = 0; j < static_cast<int>(props->size()); j++) {
      ObjectLiteralProperty* prop = (*props)[j];
      PropertyDescriptor desc;
      desc.name = "__prop__";
      desc.type = type_system_->NewAny();
      desc.is_readonly = false;
      desc.is_optional = false;
      desc.is_public = true;
      desc.is_private = false;
      desc.is_protected = false;
      desc.has_readonly_modifier = false;

      if (prop->value() != nullptr) {
        desc.type = InferExpressionType(prop->value());
      }

      hint.properties->Add(desc, zone_);
    }

    map_hints_->Add(hint, zone_);
  }
}

void TSPipeline::CollectObjectLiterals(AstNode* node,
                                       ZoneList<ObjectLiteral*>* result) {
  if (node == nullptr || result == nullptr) return;

  if (node->IsObjectLiteral()) {
    result->Add(node->AsObjectLiteral(), zone_);
  }

  switch (node->node_type()) {
    case AstNode::kFunctionLiteral: {
      FunctionLiteral* func = node->AsFunctionLiteral();
      if (func->body() != nullptr) {
        ZonePtrList<Statement>* body = func->body();
        for (int i = 0; i < static_cast<int>(body->size()); i++) {
          CollectObjectLiterals((*body)[i], result);
        }
      }
      break;
    }

    case AstNode::kBlock: {
      Block* block = node->AsBlock();
      if (block->statements() != nullptr) {
        ZonePtrList<Statement>* stmts = block->statements();
        for (int i = 0; i < static_cast<int>(stmts->size()); i++) {
          CollectObjectLiterals((*stmts)[i], result);
        }
      }
      break;
    }

    case AstNode::kExpressionStatement: {
      ExpressionStatement* expr_stmt = node->AsExpressionStatement();
      if (expr_stmt->expression() != nullptr) {
        CollectObjectLiterals(expr_stmt->expression(), result);
      }
      break;
    }

    case AstNode::kVariableDeclaration: {
      VariableDeclaration* var_decl = node->AsVariableDeclaration();
      if (var_decl->init() != nullptr) {
        CollectObjectLiterals(var_decl->init(), result);
      }
      break;
    }

    case AstNode::kAssignment: {
      Assignment* assign = node->AsAssignment();
      if (assign->value() != nullptr) {
        CollectObjectLiterals(assign->value(), result);
      }
      break;
    }

    case AstNode::kReturnStatement: {
      ReturnStatement* ret = node->AsReturnStatement();
      if (ret->expression() != nullptr) {
        CollectObjectLiterals(ret->expression(), result);
      }
      break;
    }

    case AstNode::kIfStatement: {
      IfStatement* if_stmt = node->AsIfStatement();
      if (if_stmt->then_statement() != nullptr) {
        CollectObjectLiterals(if_stmt->then_statement(), result);
      }
      if (if_stmt->else_statement() != nullptr) {
        CollectObjectLiterals(if_stmt->else_statement(), result);
      }
      break;
    }

    case AstNode::kBinaryOperation: {
      BinaryOperation* binop = node->AsBinaryOperation();
      CollectObjectLiterals(binop->left(), result);
      CollectObjectLiterals(binop->right(), result);
      break;
    }

    case AstNode::kCall: {
      Call* call = node->AsCall();
      ZonePtrList<Expression>* args = call->arguments();
      if (args != nullptr) {
        for (int i = 0; i < static_cast<int>(args->size()); i++) {
          CollectObjectLiterals((*args)[i], result);
        }
      }
      break;
    }

    case AstNode::kArrayLiteral: {
      ArrayLiteral* arr = node->AsArrayLiteral();
      ZonePtrList<Expression>* elems = arr->elements();
      if (elems != nullptr) {
        for (int i = 0; i < static_cast<int>(elems->size()); i++) {
          CollectObjectLiterals((*elems)[i], result);
        }
      }
      break;
    }

    case AstNode::kConditional: {
      Conditional* cond = node->AsConditional();
      CollectObjectLiterals(cond->then_expression(), result);
      CollectObjectLiterals(cond->else_expression(), result);
      break;
    }

    default:
      break;
  }
}

void TSPipeline::GenerateVariableTypeMap(FunctionLiteral* root) {
  if (root == nullptr || zone_ == nullptr) return;

  if (root->scope() == nullptr) return;

  DeclarationScope* scope = root->scope();
  int param_count = root->parameter_count();

  for (int i = 0; i < param_count; i++) {
    Variable* var = scope->parameter(i);
    if (var != nullptr) {
      const char* param_name =
          reinterpret_cast<const char*>(var->raw_name()->raw_data());
      TSType* param_type = type_system_->NewAny();
      bool found = false;
      for (int j = 0; j < parameter_types_->length(); j++) {
        if (parameter_types_->at(j).first == i) {
          param_type = parameter_types_->at(j).second;
          found = true;
          break;
        }
      }
      if (!found) {
        parameter_types_->Add(std::make_pair(i, param_type), zone_);
      }
      if (config_.skip_type_erasure) {
        variable_types_->Add(std::make_pair(param_name, param_type), zone_);
      }
    }
  }

  if (config_.generate_runtime_checks) {
    for (int i = 0; i < variable_types_->length(); i++) {
      const char* var_name = variable_types_->at(i).first;
      TSType* var_type = variable_types_->at(i).second;
      if (var_type != nullptr && var_type->kind() != TypeKind::kAny &&
          var_type->kind() != TypeKind::kUnknown) {
      }
    }
  }
}

TSPipeline::TypeInfoForJIT* TSPipeline::GetTypeInfoForJIT(Zone* zone) {
  if (zone == nullptr) return nullptr;

  TypeInfoForJIT* info = zone->New<TypeInfoForJIT>();
  info->variable_types = variable_types_;
  info->parameter_types = parameter_types_;
  info->return_type = function_return_type_;
  info->types_are_stable = config_.trust_types;

  return info;
}

ZoneList<TSPipeline::MapCreationHint>* TSPipeline::GetMapHints(
    Zone* zone) {
  if (zone == nullptr) return nullptr;
  return map_hints_;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8