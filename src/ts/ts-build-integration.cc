#include "src/ts/ts-build-integration.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/ast/ast.h"
#include "src/ast/ast-value-factory.h"
#include "src/ast/scopes.h"
#include "src/base/logging.h"
#include "src/base/platform/time.h"
#include "src/common/globals.h"
#include "src/execution/vm-state-inl.h"
#include "src/flags/flags.h"
#include "src/handles/handles.h"
#include "src/handles/maybe-handles.h"
#include "src/objects/scope-info.h"
#include "src/objects/script.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parser.h"
#include "src/parsing/scanner.h"
#include "src/parsing/scanner-character-streams.h"
#include "src/parsing/token.h"
#include "src/ts/ts-benchmark-suite.h"
#include "src/ts/ts-bytecode-extensions.h"
#include "src/ts/ts-jit-integration.h"
#include "src/ts/ts-map-extensions.h"
#include "src/ts/ts-type-checker.h"
#include "src/ts/ts-type-system.h"
#include "src/zone/zone-list-inl.h"

namespace v8 {
namespace internal {
namespace ts {

bool TSBuildIntegration::ts_v8_enabled_ = true;
bool TSBuildIntegration::force_ts_mode_ = false;
TSCompilationConfig TSBuildIntegration::config_;

namespace {

struct CompilationStats {
  double parse_time_ms = 0.0;
  double type_check_time_ms = 0.0;
  double annotate_time_ms = 0.0;
  double bytecode_gen_time_ms = 0.0;
  double total_time_ms = 0.0;
  int source_length = 0;
  int error_count = 0;
  int warning_count = 0;
  bool used_ts_pipeline = false;
  bool type_script = false;
  std::string filename;
  std::string ts_version = "1.0.0";
};

CompilationStats g_last_stats;
bool g_stats_enabled = false;

struct TSCompilationEvent {
  enum Type { kCompileStart, kCompileEnd, kTypeCheckStart,
              kTypeCheckEnd, kJITOptimize, kDeoptimize };
  Type type;
  double timestamp_ms;
  int source_position;
  const char* function_name;
};

std::vector<TSCompilationEvent> g_event_log;

struct SourcePositionInfo {
  int line = 0;
  int column = 0;
  const char* filename = nullptr;
};

SourcePositionInfo CalculateSourcePosition(const char* source,
                                            size_t source_length,
                                            int offset) {
  SourcePositionInfo info;
  if (source == nullptr || offset < 0) return info;

  int pos = offset;
  if (pos > static_cast<int>(source_length)) {
    pos = static_cast<int>(source_length);
  }

  info.line = 1;
  info.column = 1;

  for (int i = 0; i < pos; i++) {
    if (source[i] == '\n') {
      info.line++;
      info.column = 1;
    } else {
      info.column++;
    }
  }

  return info;
}

bool DetectTypeScriptFromFilename(const char* filename) {
  if (filename == nullptr) return false;
  size_t len = strlen(filename);
  if (len < 3) return false;

  const char* ext = filename + len - 3;
  if (strcmp(ext, ".ts") == 0) return true;

  if (len >= 5) {
    const char* ext5 = filename + len - 5;
    if (strcmp(ext5, ".tsx") == 0) return true;
    if (strcmp(ext5, ".mts") == 0) return true;
    if (strcmp(ext5, ".cts") == 0) return true;
  }

  return false;
}

bool DetectTypeScriptFromContent(const char* source, size_t length) {
  if (source == nullptr || length == 0) return false;

  static const char* ts_patterns[] = {
      "interface ", "type ", "enum ", "namespace ", "implements ",
      "abstract ", "readonly ", "satisfies ", "keyof ", "infer ",
      "declare ", "module ", "unique ", " as ", ": string", ": number",
      ": boolean", ": void", ": any", ": never", ": unknown",
      ": bigint", ": symbol", ": object",
  };

  static const int num_patterns =
      sizeof(ts_patterns) / sizeof(ts_patterns[0]);

  for (int i = 0; i < num_patterns; i++) {
    size_t pat_len = strlen(ts_patterns[i]);
    if (pat_len > length) continue;
    for (size_t j = 0; j <= length - pat_len; j++) {
      bool match = true;
      for (size_t k = 0; k < pat_len; k++) {
        if (source[j + k] != ts_patterns[i][k]) {
          match = false;
          break;
        }
      }
      if (match) {
        bool boundary_before =
            (j == 0) || !isalnum(static_cast<unsigned char>(source[j - 1]));
        bool boundary_after =
            (j + pat_len >= length) ||
            !isalnum(static_cast<unsigned char>(source[j + pat_len]));
        if (boundary_before && boundary_after) {
          return true;
        }
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
    if (source[i] == '<' && source[i + 1] != ' ') {
      for (size_t j = i + 1; j < length && j < i + 20; j++) {
        if (source[j] == '>') {
          return true;
        }
        if (!isalpha(static_cast<unsigned char>(source[j])) &&
            source[j] != ' ' && source[j] != '_' && source[j] != ',') {
          break;
        }
      }
    }
  }

  return false;
}

void LogCompilationStats(const CompilationStats& stats) {
  if (!g_stats_enabled) return;
  if (!stats.used_ts_pipeline) return;

  fprintf(stderr, "[TS-V8] === Compilation Results ===\n");
  fprintf(stderr, "[TS-V8] File: %s\n", stats.filename.c_str());
  fprintf(stderr, "[TS-V8] TypeScript mode: %s\n",
          stats.type_script ? "YES" : "NO (standard JS fallback)");
  fprintf(stderr, "[TS-V8] Source length: %d chars\n", stats.source_length);
  fprintf(stderr, "[TS-V8] -- Timing --\n");
  fprintf(stderr, "[TS-V8] Parse time:        %.3f ms\n", stats.parse_time_ms);
  fprintf(stderr, "[TS-V8] Type check time:   %.3f ms\n",
          stats.type_check_time_ms);
  fprintf(stderr, "[TS-V8] Annotate time:     %.3f ms\n",
          stats.annotate_time_ms);
  fprintf(stderr, "[TS-V8] Bytecode gen time: %.3f ms\n",
          stats.bytecode_gen_time_ms);
  fprintf(stderr, "[TS-V8] Total time:        %.3f ms\n", stats.total_time_ms);
  fprintf(stderr, "[TS-V8] -- Diagnostics --\n");
  fprintf(stderr, "[TS-V8] Errors:   %d\n", stats.error_count);
  fprintf(stderr, "[TS-V8] Warnings: %d\n", stats.warning_count);
  fprintf(stderr, "[TS-V8] ===============================\n");
}

void LogEvent(TSCompilationEvent::Type type, int position,
              const char* function_name) {
  if (!g_stats_enabled) return;

  TSCompilationEvent event;
  event.type = type;
  event.timestamp_ms = 0.0;
  event.source_position = position;
  event.function_name = function_name;
  g_event_log.push_back(event);
}

void PrintTypeErrors(ParseInfo* info, TSTypeChecker* checker,
                     const char* source, size_t source_len) {
  if (info == nullptr || checker == nullptr) return;
  if (checker->error_count() == 0 && checker->warning_count() == 0) return;

  fprintf(stderr, "\n=== TypeScript Type Diagnostics ===\n");
  fprintf(stderr, "Errors:   %d\n", checker->error_count());
  fprintf(stderr, "Warnings: %d\n\n", checker->warning_count());

  for (int i = 0; i < checker->error_count(); i++) {
    fprintf(stderr, "  Error[%d]: (position info unavailable)\n", i);
  }

  for (int i = 0; i < checker->warning_count(); i++) {
    fprintf(stderr, "  Warning[%d]: (position info unavailable)\n", i);
  }
}

void RegisterTSV8Flags() {
  g_stats_enabled = true;
  g_event_log.clear();
  memset(&g_last_stats, 0, sizeof(g_last_stats));
}

void ClearCompilationEvents() {
  g_event_log.clear();
}

}  // namespace

void TSBuildIntegration::Initialize(Isolate* isolate) {
  if (isolate == nullptr) return;

  ts_v8_enabled_ = true;
  force_ts_mode_ = false;

  config_.is_typescript = false;
  config_.type_check = true;
  config_.strict_mode = true;
  config_.check_nulls = true;
  config_.check_implicit_any = false;
  config_.skip_type_erasure = false;
  config_.generate_runtime_checks = false;
  config_.trust_types = true;

  RegisterTSV8Flags();
}

bool TSBuildIntegration::ShouldUseTSV8(ParseInfo* info) {
  if (!ts_v8_enabled_) return false;
  if (info == nullptr) return false;

  if (force_ts_mode_) return true;

  if (!info->flags().is_toplevel()) {
    return false;
  }

  return config_.is_typescript;
}

TSPipeline* TSBuildIntegration::CreatePipeline(ParseInfo* info,
                                                 Isolate* isolate) {
  if (info == nullptr) return nullptr;

  TSCompilationConfig config = config_;

  if (force_ts_mode_) {
    config.is_typescript = true;
  }

  TSPipeline* pipeline = new TSPipeline(config);
  pipeline->Initialize(info);

  return pipeline;
}

bool TSBuildIntegration::ParseProgram(
    ParseInfo* info, DirectHandle<Script> script,
    MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info, Isolate* isolate,
    ReportStatisticsMode mode) {
  if (info == nullptr || isolate == nullptr) return false;

  memset(&g_last_stats, 0, sizeof(g_last_stats));

  if (!ts_v8_enabled_ || !ShouldUseTSV8(info)) {
    g_last_stats.used_ts_pipeline = false;
    g_last_stats.type_script = false;

    return v8::internal::parsing::ParseProgram(info, script,
                                                maybe_outer_scope_info,
                                                isolate, mode);
  }

  VMState<PARSER> state(isolate);

  g_last_stats.used_ts_pipeline = true;
  g_last_stats.type_script = config_.is_typescript;

  Handle<String> source(Cast<String>(script->source()), isolate);
  g_last_stats.source_length = source->length();

  if (script->name()->IsString()) {
    g_last_stats.filename = std::string(
        script->name()->ToString(isolate).c_str());
  }

  std::unique_ptr<Utf16CharacterStream> stream(
      ScannerStream::For(isolate, source));
  info->set_character_stream(std::move(stream));

  auto total_start = base::Time::Now();

  TSPipeline pipeline(config_);

  auto init_start = base::Time::Now();
  pipeline.Initialize(info);
  auto init_end = base::Time::Now();

  auto parse_start = base::Time::Now();
  LogEvent(TSCompilationEvent::kCompileStart, 0, "ParseProgram");

  bool success = pipeline.Compile(isolate, info, script,
                                   maybe_outer_scope_info);

  LogEvent(TSCompilationEvent::kCompileEnd, 0, "ParseProgram");
  auto parse_end = base::Time::Now();

  g_last_stats.parse_time_ms =
      (init_end - init_start).InMillisecondsF() +
      (parse_end - parse_start).InMillisecondsF();

  if (info->literal() != nullptr && config_.type_check &&
      pipeline.is_typescript()) {
    LogEvent(TSCompilationEvent::kTypeCheckStart, 0, "TypeCheck");
    auto tc_start = base::Time::Now();

    TSTypeChecker* checker = pipeline.type_checker();
    if (checker != nullptr) {
      checker->CheckProgram(info->literal());
      g_last_stats.error_count = checker->error_count();
      g_last_stats.warning_count = checker->warning_count();

      if (checker->error_count() > 0) {
        Handle<String> source_str(Cast<String>(script->source()), isolate);
        int source_len = source_str->length();
        const char* raw_source = source_str->ToCString();
        PrintTypeErrors(info, checker, raw_source, source_len);
      }

      if (config_.strict_mode && checker->error_count() > 0) {
        success = false;
      }
    }

    auto tc_end = base::Time::Now();
    g_last_stats.type_check_time_ms = (tc_end - tc_start).InMillisecondsF();
    LogEvent(TSCompilationEvent::kTypeCheckEnd, 0, "TypeCheck");
  }

  auto total_end = base::Time::Now();
  g_last_stats.total_time_ms = (total_end - total_start).InMillisecondsF();

  if (info->literal() != nullptr) {
    info->literal()->scope()->GetScriptScope();
  }

  LogCompilationStats(g_last_stats);

  return success;
}

bool TSBuildIntegration::ParseFunction(
    ParseInfo* info, DirectHandle<SharedFunctionInfo> shared_info,
    Isolate* isolate, ReportStatisticsMode mode) {
  if (info == nullptr || isolate == nullptr) return false;

  if (!ts_v8_enabled_) {
    return v8::internal::parsing::ParseFunction(info, shared_info, isolate,
                                                 mode);
  }

  VMState<PARSER> state(isolate);

  DirectHandle<Script> script(Cast<Script>(shared_info->script()), isolate);
  Handle<String> source(Cast<String>(script->source()), isolate);
  uint32_t start_pos = shared_info->StartPosition();
  uint32_t end_pos = shared_info->EndPosition();
  if (end_pos > source->length()) {
    isolate->PushStackTraceAndDie(
        "shared function end position beyond source length",
        reinterpret_cast<void*>(script->ptr()),
        reinterpret_cast<void*>(source->ptr()));
    return false;
  }
  std::unique_ptr<Utf16CharacterStream> stream(
      ScannerStream::For(isolate, source, start_pos, end_pos));
  info->set_character_stream(std::move(stream));

  if (config_.is_typescript || force_ts_mode_) {
    LocalIsolate* local_isolate = isolate->main_thread_local_isolate();
    TSParser ts_parser(local_isolate, info);
    ts_parser.ParseFunction(isolate, info, shared_info);

    if (mode) {
      ts_parser.UpdateStatistics(isolate, script);
    }

    return info->literal() != nullptr;
  }

  Parser parser(isolate->main_thread_local_isolate(), info);
  DCHECK(parser.parsing_on_main_thread_);
  parser.ParseFunction(isolate, info, shared_info);

  if (mode) {
    parser.UpdateStatistics(isolate, script);
  }

  return info->literal() != nullptr;
}

void TSBuildIntegration::Enable(bool enabled) {
  ts_v8_enabled_ = enabled;
}

bool TSBuildIntegration::IsEnabled() {
  return ts_v8_enabled_;
}

void TSBuildIntegration::SetConfig(const TSCompilationConfig& config) {
  config_ = config;
}

TSCompilationConfig TSBuildIntegration::GetConfig() {
  return config_;
}

void TSBuildIntegration::ForceTSMode(bool force) {
  force_ts_mode_ = force;
  if (force) {
    config_.is_typescript = true;
  }
}

namespace parsing {

bool ParseAny(ParseInfo* info, Isolate* isolate,
              ReportStatisticsMode mode) {
  if (info == nullptr || isolate == nullptr) return false;

  if (info->flags().is_toplevel()) {
    return false;
  }

  return TSBuildIntegration::ParseProgram(
      info, DirectHandle<Script>(), kNullMaybeHandle, isolate, mode);
}

bool ParseProgram(ParseInfo* info, DirectHandle<Script> script,
                  MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info,
                  Isolate* isolate, ReportStatisticsMode mode) {
  return TSBuildIntegration::ParseProgram(info, script,
                                          maybe_outer_scope_info, isolate,
                                          mode);
}

bool ParseFunction(ParseInfo* info,
                   DirectHandle<SharedFunctionInfo> shared_info,
                   Isolate* isolate, ReportStatisticsMode mode) {
  return TSBuildIntegration::ParseFunction(info, shared_info, isolate, mode);
}

}  // namespace parsing

}  // namespace ts
}  // namespace internal
}  // namespace v8