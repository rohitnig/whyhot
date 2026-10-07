#pragma once

#include <string>

namespace whyhot {

struct FanStats {
  int rpm = -1;  // -1 = unavailable
  bool available = false;
};

// Locates the first hwmon device exposing fan1_input (e.g. the ThinkPad EC,
// dell-smm, applesmc, ...). Discovery runs once; the resolved path is cached.
class FanCollector {
 public:
  FanStats sample();

 private:
  void discover();

  bool discovered_ = false;
  std::string fan_path_;
};

}  // namespace whyhot
