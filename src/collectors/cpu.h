#pragma once

#include <cstdint>
#include <vector>

namespace whyhot {

struct CpuStats {
  double total_util_pct = 0.0;
  std::vector<double> per_core_pct;
  double load_avg1 = 0.0;
  double load_avg5 = 0.0;
  double load_avg15 = 0.0;
};

// Reads /proc/stat and /proc/loadavg. Utilization is computed as a delta
// against the previous sample, so the first sample always reads 0%.
class CpuCollector {
 public:
  struct Times {
    uint64_t idle_all = 0;   // idle + iowait
    uint64_t non_idle = 0;   // user + nice + system + irq + softirq + steal
  };

  CpuStats sample();

 private:
  Times prev_total_;
  std::vector<Times> prev_per_core_;
  bool have_prev_ = false;

  static double utilFromDelta(const Times& prev, const Times& cur);
};

}  // namespace whyhot
