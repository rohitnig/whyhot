#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace whyhot {

struct ProcessInfo {
  int pid = 0;
  std::string comm;     // short name from /proc/<pid>/stat, truncated to 15 chars
  std::string cmdline;  // full command from /proc/<pid>/cmdline, falls back to comm
  double cpu_pct = 0.0;  // % of one core, top-style, can exceed 100
  long rss_kb = 0;
};

struct ProcessEvent {
  enum class Type { kStart, kExit };
  int pid = 0;
  std::string comm;
  Type type = Type::kStart;
};

// Scans /proc for running processes each sample, computing per-process CPU%
// from utime+stime deltas, and reports processes that appeared/disappeared
// since the previous sample.
class ProcessCollector {
 public:
  struct Result {
    std::vector<ProcessInfo> top;  // sorted by cpu_pct descending
    std::vector<ProcessEvent> events;
  };

  explicit ProcessCollector(int top_n = 5);
  Result sample();

 private:
  struct PrevInfo {
    uint64_t ticks = 0;
    std::string comm;
  };

  int top_n_;
  std::unordered_map<int, PrevInfo> prev_;
  double clk_tck_ = 100.0;
  bool first_sample_ = true;
  std::chrono::steady_clock::time_point prev_wall_;
};

}  // namespace whyhot
