#ifndef V8_TS_TS_BENCHMARK_SUITE_H_
#define V8_TS_TS_BENCHMARK_SUITE_H_

#include "src/ts/ts-type-system.h"
#include "src/ts/ts-type-checker.h"
#include "src/ts/ts-pipeline.h"
#include <string>
#include <vector>

namespace v8 {
namespace internal {
namespace ts {

struct BenchmarkCase {
  std::string name;
  std::string description;
  std::string source_code;
  std::string expected_output;
  std::string expected_stdout;
  bool should_pass = true;
  int expected_error_count = 0;

  size_t expected_min_ops = 0;
  size_t expected_max_ops = 0;

  std::string extension = ".ts";
};

struct BenchmarkResult {
  std::string name;
  bool compiled_successfully;
  bool type_checked_successfully;
  bool executed_successfully;
  bool output_matched;
  double compilation_time_ms;
  double type_check_time_ms;
  double execution_time_ms;
  int error_count;
  std::string actual_output;
  std::string expected_output;
  bool passed;
};

struct BenchmarkConfig {
  bool run_correctness_tests = true;
  bool run_performance_tests = true;
  bool compare_with_legacy = true;
  int warmup_iterations = 3;
  int measurement_iterations = 10;
  std::string output_dir = "/workspace/ts-benchmark-results";
  bool generate_report = true;
};

class TSBenchmarkSuite {
 public:
  explicit TSBenchmarkSuite(const BenchmarkConfig& config);

  void AddCase(const BenchmarkCase& test_case);
  void AddAllBuiltInCases();

  std::vector<BenchmarkResult> RunAll();
  BenchmarkResult RunCase(const BenchmarkCase& test_case);

  struct ComparisonResult {
    std::string name;
    double ts_v8_time_ms;
    double traditional_time_ms;
    double speedup;
    bool same_output;
    std::string notes;
  };

  std::vector<ComparisonResult> CompareWithTraditional();

  void GenerateJSONReport(const std::string& filename);
  void GenerateMarkdownReport(const std::string& filename);

  const std::vector<BenchmarkResult>& results() const { return results_; }
  const std::vector<BenchmarkCase>& test_cases() const { return test_cases_; }
  int passed_count() const { return passed_count_; }
  int failed_count() const { return failed_count_; }

 private:
  void AddTypeSystemTests();
  void AddClassTests();
  void AddInterfaceTests();
  void AddGenericTests();
  void AddEnumTests();
  void AddAdvancedTypeTests();
  void AddPerformanceTests();

  BenchmarkResult ExecuteCase(const BenchmarkCase& test_case);
  BenchmarkResult ExecuteWithTSV8(const BenchmarkCase& test_case);
  BenchmarkResult ExecuteWithTraditional(const BenchmarkCase& test_case);

  bool CompareOutputs(const std::string& expected, const std::string& actual);
  std::string NormalizeOutput(const std::string& output);

  BenchmarkConfig config_;
  std::vector<BenchmarkCase> test_cases_;
  std::vector<BenchmarkResult> results_;
  int passed_count_ = 0;
  int failed_count_ = 0;
};

std::vector<BenchmarkCase> GetBuiltInBenchmarkCases();

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_BENCHMARK_SUITE_H_