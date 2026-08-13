#ifndef V8_TS_TS_BUILD_INTEGRATION_H_
#define V8_TS_TS_BUILD_INTEGRATION_H_

#include "src/ts/ts-pipeline.h"
#include "src/parsing/parsing.h"
#include "src/parsing/parse-info.h"

namespace v8 {
namespace internal {

class Isolate;
class Script;

namespace ts {

class TSBuildIntegration {
 public:
  static void Initialize(Isolate* isolate);

  static bool ShouldUseTSV8(ParseInfo* info);

  static bool ParseProgram(ParseInfo* info,
                            DirectHandle<Script> script,
                            MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info,
                            Isolate* isolate,
                            ReportStatisticsMode mode);

  static bool ParseFunction(ParseInfo* info,
                              DirectHandle<SharedFunctionInfo> shared_info,
                              Isolate* isolate,
                              ReportStatisticsMode mode);

  static void Enable(bool enabled);
  static bool IsEnabled();

  static void SetConfig(const TSCompilationConfig& config);
  static TSCompilationConfig GetConfig();

  static void ForceTSMode(bool force);

 private:
  static bool ts_v8_enabled_;
  static bool force_ts_mode_;
  static TSCompilationConfig config_;

  static TSPipeline* CreatePipeline(ParseInfo* info, Isolate* isolate);
};

namespace parsing {

bool ParseAny(ParseInfo* info,
              Isolate* isolate,
              ReportStatisticsMode mode);

bool ParseProgram(ParseInfo* info,
                  DirectHandle<Script> script,
                  MaybeDirectHandle<ScopeInfo> maybe_outer_scope_info,
                  Isolate* isolate,
                  ReportStatisticsMode mode);

bool ParseFunction(ParseInfo* info,
                   DirectHandle<SharedFunctionInfo> shared_info,
                   Isolate* isolate,
                   ReportStatisticsMode mode);

}  // namespace parsing

}  // namespace ts
}  // namespace internal
}  // namespace v8

#endif  // V8_TS_TS_BUILD_INTEGRATION_H_