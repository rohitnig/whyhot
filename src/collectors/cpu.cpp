#include "collectors/cpu.h"

#include <fstream>
#include <sstream>
#include <string>

namespace whyhot {

namespace {

bool parseCpuLine(const std::string& line, CpuCollector::Times* out) {
  // Format: cpu<N?> user nice system idle iowait irq softirq steal ...
  std::istringstream iss(line);
  std::string label;
  iss >> label;
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
           softirq = 0, steal = 0;
  if (!(iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
        steal)) {
    return false;
  }
  out->idle_all = idle + iowait;
  out->non_idle = user + nice + system + irq + softirq + steal;
  return true;
}

}  // namespace

double CpuCollector::utilFromDelta(const Times& prev, const Times& cur) {
  const uint64_t d_idle =
      cur.idle_all >= prev.idle_all ? cur.idle_all - prev.idle_all : 0;
  const uint64_t d_busy =
      cur.non_idle >= prev.non_idle ? cur.non_idle - prev.non_idle : 0;
  const uint64_t d_total = d_idle + d_busy;
  if (d_total == 0) return 0.0;
  return 100.0 * static_cast<double>(d_busy) / static_cast<double>(d_total);
}

CpuStats CpuCollector::sample() {
  CpuStats stats;

  std::ifstream stat("/proc/stat");
  std::string line;
  Times total;
  std::vector<Times> per_core;

  while (std::getline(stat, line)) {
    if (line.rfind("cpu", 0) != 0) break;
    // Distinguish aggregate "cpu " line from per-core "cpu0", "cpu1", ...
    Times t;
    if (!parseCpuLine(line, &t)) continue;
    if (line.size() > 3 && line[3] == ' ') {
      total = t;
    } else {
      per_core.push_back(t);
    }
  }

  if (have_prev_) {
    stats.total_util_pct = utilFromDelta(prev_total_, total);
    stats.per_core_pct.reserve(per_core.size());
    for (size_t i = 0; i < per_core.size(); ++i) {
      if (i < prev_per_core_.size()) {
        stats.per_core_pct.push_back(
            utilFromDelta(prev_per_core_[i], per_core[i]));
      } else {
        stats.per_core_pct.push_back(0.0);
      }
    }
  } else {
    stats.per_core_pct.assign(per_core.size(), 0.0);
  }

  prev_total_ = total;
  prev_per_core_ = per_core;
  have_prev_ = true;

  std::ifstream loadavg("/proc/loadavg");
  if (loadavg) {
    loadavg >> stats.load_avg1 >> stats.load_avg5 >> stats.load_avg15;
  }

  return stats;
}

}  // namespace whyhot
