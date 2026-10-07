#pragma once

#include <string>

namespace whyhot {

struct ThermalStats {
  double cpu_temp_c = -1.0;  // -1 = unavailable
  double gpu_temp_c = -1.0;
  bool throttled = false;
};

// Locates CPU/GPU temperature sensors under /sys/class/hwmon by label
// (e.g. the ThinkPad EC exposes hwmonN/tempX_label == "CPU"/"GPU") and
// falls back to coretemp's package sensor / ACPI thermal zones. Discovery
// runs once and the resolved sysfs paths are cached.
class ThermalCollector {
 public:
  ThermalStats sample();

 private:
  void discover();

  bool discovered_ = false;
  std::string cpu_path_;
  std::string gpu_path_;
  std::string throttle_path_;  // thermal_zone in_use (>= critical trip)
};

}  // namespace whyhot
