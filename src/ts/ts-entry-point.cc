#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "include/libplatform/libplatform.h"
#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-function.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-script.h"
#include "include/v8-string.h"
#include "include/v8-value.h"

#include "src/base/platform/time.h"
#include "src/execution/isolate.h"
#include "src/handles/handles.h"
#include "src/init/v8.h"
#include "src/objects/objects.h"
#include "src/objects/script.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parsing.h"
#include "src/ts/ts-benchmark-suite.h"
#include "src/ts/ts-build-integration.h"
#include "src/ts/ts-pipeline.h"
#include "src/ts/ts-type-checker.h"
#include "src/ts/ts-type-system.h"
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {
namespace ts {

struct CommandLineOptions {
  std::string filename;
  bool run_benchmark = false;
  bool check_only = false;
  bool compare_mode = false;
  bool show_help = false;
  bool show_version = false;
  bool use_strict = true;
  bool use_type_check = true;
  bool force_ts = false;
  int warmup_iterations = 3;
  int measurement_iterations = 10;
  std::string benchmark_output_dir = "/workspace/ts-benchmark-results";
  bool json_output = false;
};

void PrintUsage(const char* program_name) {
  fprintf(stderr,
      "TS-V8: TypeScript V8 Integration\n"
      "=================================\n\n"
      "Usage: %s [options] [file.ts]\n\n"
      "Options:\n"
      "  --benchmark              Run benchmark suite\n"
      "  --check-only             Type-check without execution\n"
      "  --compare                Compare TS-V8 vs traditional compilation\n"
      "  --force-ts               Force TypeScript mode (even for .js files)\n"
      "  --no-type-check          Disable type checking\n"
      "  --no-strict              Disable strict mode\n"
      "  --warmup N               Warmup iterations for benchmarks (default: 3)\n"
      "  --measure N              Measurement iterations (default: 10)\n"
      "  --output-dir DIR         Benchmark output directory\n"
      "  --json                   Output results in JSON format\n"
      "  --help                   Show this help message\n"
      "  --version                Show version information\n\n"
      "Examples:\n"
      "  %s app.ts                 Compile and run a TypeScript file\n"
      "  %s --check-only app.ts    Type-check without running\n"
      "  %s --benchmark            Run all built-in benchmarks\n"
      "  %s --compare app.ts       Compare TS-V8 vs traditional compilation\n"
      "  %s --force-ts app.js      Treat JS file as TypeScript\n",
      program_name, program_name, program_name, program_name, program_name,
      program_name, program_name);
}

void PrintVersion() {
  fprintf(stdout,
      "TS-V8 version 1.0.0\n"
      "V8 version: %s\n"
      "TypeScript compilation pipeline: enabled\n"
      "Features:\n"
      "  - TypeScript-aware parsing\n"
      "  - Static type checking\n"
      "  - Type-guided JIT optimization (TurboFan/Maglev)\n"
      "  - Map creation hints from type annotations\n"
      "  - Bytecode specialization from type info\n",
      V8::GetVersion());
}

CommandLineOptions ParseCommandLine(int argc, char* argv[]) {
  CommandLineOptions opts;

  for (int i = 1; i < argc; i++) {
    std::string arg(argv[i]);

    if (arg == "--help" || arg == "-h") {
      opts.show_help = true;
    } else if (arg == "--version" || arg == "-v") {
      opts.show_version = true;
    } else if (arg == "--benchmark") {
      opts.run_benchmark = true;
    } else if (arg == "--check-only") {
      opts.check_only = true;
    } else if (arg == "--compare") {
      opts.compare_mode = true;
    } else if (arg == "--force-ts") {
      opts.force_ts = true;
    } else if (arg == "--no-type-check") {
      opts.use_type_check = false;
    } else if (arg == "--no-strict") {
      opts.use_strict = false;
    } else if (arg == "--json") {
      opts.json_output = true;
    } else if (arg == "--warmup" && i + 1 < argc) {
      opts.warmup_iterations = atoi(argv[++i]);
    } else if (arg == "--measure" && i + 1 < argc) {
      opts.measurement_iterations = atoi(argv[++i]);
    } else if (arg == "--output-dir" && i + 1 < argc) {
      opts.benchmark_output_dir = argv[++i];
    } else if (arg[0] != '-') {
      opts.filename = arg;
    }
  }

  return opts;
}

std::string ReadFileContents(const std::string& filename) {
  std::ifstream file(filename, std::ios::in | std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return "";
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::string contents(size, '\0');
  file.read(&contents[0], size);

  return contents;
}

bool WriteFileContents(const std::string& filename,
                        const std::string& contents) {
  std::ofstream file(filename, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return false;
  }
  file.write(contents.c_str(), contents.size());
  return true;
}

std::string EscapeJSON(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result += c; break;
    }
  }
  return result;
}

class TSV8Runner {
 public:
  TSV8Runner() : isolate_(nullptr), platform_(nullptr), started_(false) {}

  ~TSV8Runner() {
    if (isolate_ != nullptr) {
      isolate_->Dispose();
      isolate_ = nullptr;
    }
    if (platform_ != nullptr) {
      V8::ShutdownPlatform();
      delete platform_;
      platform_ = nullptr;
    }
    V8::Dispose();
  }

  bool Start() {
    if (started_) return true;

    platform_ = platform::CreateDefaultPlatform();
    if (platform_ == nullptr) {
      fprintf(stderr, "TS-V8: Failed to create platform\n");
      return false;
    }
    V8::InitializePlatform(platform_);
    V8::Initialize();

    Isolate::CreateParams params;
    params.array_buffer_allocator =
        ArrayBufferAllocator::GetDefault();

    isolate_ = Isolate::New(params);
    if (isolate_ == nullptr) {
      fprintf(stderr, "TS-V8: Failed to create isolate\n");
      return false;
    }

    TSBuildIntegration::Initialize(isolate_);

    started_ = true;
    return true;
  }

  bool ExecuteFile(const std::string& filename, bool force_ts_mode = false) {
    if (!started_ || isolate_ == nullptr) return false;

    std::string source = ReadFileContents(filename);
    if (source.empty()) {
      fprintf(stderr, "TS-V8: Cannot read file: %s\n", filename.c_str());
      return false;
    }

    if (force_ts_mode) {
      TSBuildIntegration::ForceTSMode(true);
    }

    TSCompilationConfig config;
    config.is_typescript = TSBuildIntegration::IsEnabled() && force_ts_mode;
    config.type_check = true;
    config.strict_mode = true;
    TSBuildIntegration::SetConfig(config);

    auto total_start = base::Time::Now();

    HandleScope handle_scope(isolate_);
    Local<Context> context = Context::New(isolate_);
    Context::Scope context_scope(context);

    Local<String> source_str = String::NewFromUtf8(
        isolate_, source.c_str(), NewStringType::kNormal).ToLocalChecked();

    Local<Script> script;
    MaybeLocal<Script> maybe_script = Script::Compile(
        context, source_str,
        ScriptOrigin(String::NewFromUtf8(isolate_, filename.c_str(),
                                          NewStringType::kNormal).ToLocalChecked()));

    if (!maybe_script.ToLocal(&script)) {
      MaybeLocal<Value> exception = isolate_->TryCatchException();
      if (!exception.IsEmpty()) {
        Local<Value> ex = exception.ToLocalChecked();
        String::Utf8Value utf8(isolate_, ex);
        fprintf(stderr, "TS-V8 Compilation Error: %s\n", *utf8);
      }
      return false;
    }

    auto compile_end = base::Time::Now();

    Local<Value> result;
    MaybeLocal<Value> maybe_result = script->Run(context);

    if (!maybe_result.ToLocal(&result)) {
      MaybeLocal<Value> exception = isolate_->TryCatchException();
      if (!exception.IsEmpty()) {
        Local<Value> ex = exception.ToLocalChecked();
        String::Utf8Value utf8(isolate_, ex);
        fprintf(stderr, "TS-V8 Runtime Error: %s\n", *utf8);
      }
      return false;
    }

    auto total_end = base::Time::Now();

    double compile_ms = (compile_end - total_start).InMillisecondsF();
    double exec_ms = (total_end - compile_end).InMillisecondsF();
    double total_ms = (total_end - total_start).InMillisecondsF();

    fprintf(stdout, "Execution result: %s\n",
            *String::Utf8Value(isolate_, result->ToString(context).ToLocalChecked()));
    fprintf(stdout, "\n--- Performance ---\n");
    fprintf(stdout, "  Compilation:  %.3f ms\n", compile_ms);
    fprintf(stdout, "  Execution:    %.3f ms\n", exec_ms);
    fprintf(stdout, "  Total:        %.3f ms\n", total_ms);

    return true;
  }

  bool CheckFile(const std::string& filename) {
    if (!started_ || isolate_ == nullptr) return false;

    std::string source = ReadFileContents(filename);
    if (source.empty()) {
      fprintf(stderr, "TS-V8: Cannot read file: %s\n", filename.c_str());
      return false;
    }

    TSCompilationConfig config;
    config.is_typescript = true;
    config.type_check = true;
    config.strict_mode = true;
    TSBuildIntegration::SetConfig(config);
    TSBuildIntegration::ForceTSMode(true);

    HandleScope handle_scope(isolate_);
    Local<Context> context = Context::New(isolate_);
    Context::Scope context_scope(context);

    Local<String> source_str = String::NewFromUtf8(
        isolate_, source.c_str(), NewStringType::kNormal).ToLocalChecked();

    Local<Script> script;
    MaybeLocal<Script> maybe_script = Script::Compile(
        context, source_str,
        ScriptOrigin(String::NewFromUtf8(isolate_, filename.c_str(),
                                          NewStringType::kNormal).ToLocalChecked()));

    bool success = true;
    if (!maybe_script.ToLocal(&script)) {
      MaybeLocal<Value> exception = isolate_->TryCatchException();
      if (!exception.IsEmpty()) {
        Local<Value> ex = exception.ToLocalChecked();
        String::Utf8Value utf8(isolate_, ex);
        fprintf(stderr, "TS-V8 Type Check Error: %s\n", *utf8);
        success = false;
      }
    } else {
      fprintf(stdout, "TS-V8 Type Check: PASSED\n");
      fprintf(stdout, "File: %s\n", filename.c_str());
      fprintf(stdout, "Status: No type errors found\n");
    }

    return success;
  }

  bool CompareFile(const std::string& filename, int warmup, int measure) {
    if (!started_ || isolate_ == nullptr) return false;

    std::string source = ReadFileContents(filename);
    if (source.empty()) {
      fprintf(stderr, "TS-V8: Cannot read file: %s\n", filename.c_str());
      return false;
    }

    fprintf(stdout, "=== TS-V8 Performance Comparison ===\n\n");
    fprintf(stdout, "File: %s\n", filename.c_str());
    fprintf(stdout, "Warmup iterations: %d\n", warmup);
    fprintf(stdout, "Measurement iterations: %d\n\n", measure);

    std::vector<double> ts_times;
    std::vector<double> traditional_times;

    fprintf(stdout, "--- TS-V8 Pipeline ---\n");
    for (int i = 0; i < warmup + measure; i++) {
      HandleScope handle_scope(isolate_);
      Local<Context> context = Context::New(isolate_);
      Context::Scope context_scope(context);

      TSCompilationConfig config;
      config.is_typescript = true;
      config.type_check = true;
      config.strict_mode = true;
      TSBuildIntegration::SetConfig(config);
      TSBuildIntegration::ForceTSMode(true);

      auto start = base::Time::Now();

      Local<String> source_str = String::NewFromUtf8(
          isolate_, source.c_str(), NewStringType::kNormal).ToLocalChecked();
      MaybeLocal<Script> maybe_script = Script::Compile(context, source_str);
      Local<Script> script;
      if (maybe_script.ToLocal(&script)) {
        MaybeLocal<Value> result = script->Run(context);
      }

      auto end = base::Time::Now();
      double elapsed = (end - start).InMillisecondsF();

      if (i >= warmup) {
        ts_times.push_back(elapsed);
      }
    }

    double ts_avg = 0.0;
    for (double t : ts_times) ts_avg += t;
    if (!ts_times.empty()) ts_avg /= ts_times.size();

    fprintf(stdout, "  TS-V8 avg time: %.3f ms\n", ts_avg);

    fprintf(stdout, "\n--- Traditional V8 Pipeline ---\n");
    for (int i = 0; i < warmup + measure; i++) {
      HandleScope handle_scope(isolate_);
      Local<Context> context = Context::New(isolate_);
      Context::Scope context_scope(context);

      TSBuildIntegration::ForceTSMode(false);
      TSBuildIntegration::Enable(false);

      auto start = base::Time::Now();

      Local<String> source_str = String::NewFromUtf8(
          isolate_, source.c_str(), NewStringType::kNormal).ToLocalChecked();
      MaybeLocal<Script> maybe_script = Script::Compile(context, source_str);
      Local<Script> script;
      if (maybe_script.ToLocal(&script)) {
        MaybeLocal<Value> result = script->Run(context);
      }

      auto end = base::Time::Now();
      double elapsed = (end - start).InMillisecondsF();

      if (i >= warmup) {
        traditional_times.push_back(elapsed);
      }
    }

    double trad_avg = 0.0;
    for (double t : traditional_times) trad_avg += t;
    if (!traditional_times.empty()) trad_avg /= traditional_times.size();

    fprintf(stdout, "  Traditional avg time: %.3f ms\n", trad_avg);

    if (trad_avg > 0.001) {
      double speedup = trad_avg / ts_avg;
      fprintf(stdout, "\n--- Results ---\n");
      fprintf(stdout, "  TS-V8:        %.3f ms\n", ts_avg);
      fprintf(stdout, "  Traditional:  %.3f ms\n", trad_avg);
      fprintf(stdout, "  Speedup:      %.2fx\n", speedup);
      fprintf(stdout, "  Difference:   %+.2f%%\n",
              (ts_avg - trad_avg) / trad_avg * 100.0);
    }

    TSBuildIntegration::Enable(true);

    return true;
  }

  bool RunBenchmark(const CommandLineOptions& opts) {
    if (!started_ || isolate_ == nullptr) return false;

    BenchmarkConfig config;
    config.run_correctness_tests = true;
    config.run_performance_tests = true;
    config.compare_with_legacy = true;
    config.warmup_iterations = opts.warmup_iterations;
    config.measurement_iterations = opts.measurement_iterations;
    config.output_dir = opts.benchmark_output_dir;
    config.generate_report = true;

    TSBenchmarkSuite suite(config);
    suite.AddAllBuiltInCases();

    fprintf(stdout, "=== TS-V8 Benchmark Suite ===\n\n");
    fprintf(stdout, "Running %zu benchmark cases...\n",
            suite.test_cases().size());

    auto results = suite.RunAll();

    fprintf(stdout, "\n--- Results Summary ---\n");
    fprintf(stdout, "  Total:  %zu\n", results.size());
    fprintf(stdout, "  Passed: %d\n", suite.passed_count());
    fprintf(stdout, "  Failed: %d\n", suite.failed_count());

    if (suite.failed_count() > 0) {
      fprintf(stdout, "\n--- Failed Cases ---\n");
      for (const auto& r : results) {
        if (!r.passed) {
          fprintf(stdout, "  FAIL: %s\n", r.name.c_str());
          if (!r.compiled_successfully) {
            fprintf(stdout, "    Compilation failed\n");
          }
          if (!r.type_checked_successfully) {
            fprintf(stdout, "    Type check failed\n");
          }
          if (!r.executed_successfully) {
            fprintf(stdout, "    Execution failed\n");
          }
          if (!r.output_matched) {
            fprintf(stdout, "    Output mismatch\n");
            fprintf(stdout, "    Expected: %s\n", r.expected_output.c_str());
            fprintf(stdout, "    Actual:   %s\n", r.actual_output.c_str());
          }
        }
      }
    }

    fprintf(stdout, "\n--- Performance ---\n");
    double total_compile = 0, total_exec = 0;
    int count = 0;
    for (const auto& r : results) {
      total_compile += r.compilation_time_ms;
      total_exec += r.execution_time_ms;
      count++;
    }
    if (count > 0) {
      fprintf(stdout, "  Avg compilation: %.3f ms\n", total_compile / count);
      fprintf(stdout, "  Avg execution:   %.3f ms\n", total_exec / count);
    }

    if (opts.json_output) {
      std::string json = GenerateJSONResults(results);
      std::string json_file = opts.benchmark_output_dir + "/results.json";
      WriteFileContents(json_file, json);
      fprintf(stdout, "\nJSON results written to: %s\n", json_file.c_str());
    }

    return suite.failed_count() == 0;
  }

 private:
  Isolate* isolate_;
  platform::Platform* platform_;
  bool started_;

  std::string GenerateJSONResults(
      const std::vector<BenchmarkResult>& results) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"total\": " << results.size() << ",\n";
    int passed = 0, failed = 0;
    for (const auto& r : results) {
      if (r.passed) passed++; else failed++;
    }
    oss << "  \"passed\": " << passed << ",\n";
    oss << "  \"failed\": " << failed << ",\n";
    oss << "  \"results\": [\n";
    for (size_t i = 0; i < results.size(); i++) {
      const auto& r = results[i];
      oss << "    {\n";
      oss << "      \"name\": \"" << EscapeJSON(r.name) << "\",\n";
      oss << "      \"compiled\": " << (r.compiled_successfully ? "true" : "false") << ",\n";
      oss << "      \"type_checked\": " << (r.type_checked_successfully ? "true" : "false") << ",\n";
      oss << "      \"executed\": " << (r.executed_successfully ? "true" : "false") << ",\n";
      oss << "      \"output_matched\": " << (r.output_matched ? "true" : "false") << ",\n";
      oss << "      \"passed\": " << (r.passed ? "true" : "false") << ",\n";
      oss << "      \"compilation_time_ms\": " << std::fixed << std::setprecision(3)
          << r.compilation_time_ms << ",\n";
      oss << "      \"type_check_time_ms\": " << std::fixed << std::setprecision(3)
          << r.type_check_time_ms << ",\n";
      oss << "      \"execution_time_ms\": " << std::fixed << std::setprecision(3)
          << r.execution_time_ms << ",\n";
      oss << "      \"error_count\": " << r.error_count << "\n";
      oss << "    }";
      if (i + 1 < results.size()) oss << ",";
      oss << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
  }
};

int TSV8Main(int argc, char* argv[]) {
  CommandLineOptions opts = ParseCommandLine(argc, argv);

  if (opts.show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  if (opts.show_version) {
    PrintVersion();
    return 0;
  }

  TSV8Runner runner;

  if (!runner.Start()) {
    fprintf(stderr, "TS-V8: Failed to initialize\n");
    return 1;
  }

  if (opts.run_benchmark) {
    return runner.RunBenchmark(opts) ? 0 : 1;
  }

  if (opts.filename.empty()) {
    fprintf(stderr,
        "TS-V8: No input file specified.\n"
        "Usage: %s [options] [file.ts]\n"
        "Try '%s --help' for more information.\n",
        argv[0], argv[0]);
    return 1;
  }

  if (opts.check_only) {
    return runner.CheckFile(opts.filename) ? 0 : 1;
  }

  if (opts.compare_mode) {
    return runner.CompareFile(opts.filename,
                               opts.warmup_iterations,
                               opts.measurement_iterations) ? 0 : 1;
  }

  return runner.ExecuteFile(opts.filename, opts.force_ts) ? 0 : 1;
}

}  // namespace ts
}  // namespace internal
}  // namespace v8

int main(int argc, char* argv[]) {
  return v8::internal::ts::TSV8Main(argc, argv);
}