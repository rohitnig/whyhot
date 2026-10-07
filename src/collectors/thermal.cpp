#include "collectors/thermal.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <string>

namespace whyhot {

namespace {

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

std::string readFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::string line;
  std::getline(f, line);
  return trim(line);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

void ThermalCollector::discover() {
  discovered_ = true;
  std::string coretemp_pkg_path;

  DIR* hwmon_dir = opendir("/sys/class/hwmon");
  if (hwmon_dir) {
    struct dirent* entry;
    while ((entry = readdir(hwmon_dir)) != nullptr) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      const std::string base = "/sys/class/hwmon/" + name;
      const std::string driver_name = lower(readFile(base + "/name"));

      for (int i = 1; i <= 10; ++i) {
        const std::string label_path =
            base + "/temp" + std::to_string(i) + "_label";
        const std::string input_path =
            base + "/temp" + std::to_string(i) + "_input";
        std::ifstream probe(label_path);
        if (!probe) continue;
        const std::string label = lower(readFile(label_path));
        if (label == "cpu" && cpu_path_.empty()) {
          cpu_path_ = input_path;
        } else if (label == "gpu" && gpu_path_.empty()) {
          gpu_path_ = input_path;
        } else if (driver_name == "coretemp" && label == "package id 0") {
          coretemp_pkg_path = input_path;
        }
      }
    }
    closedir(hwmon_dir);
  }

  if (cpu_path_.empty() && !coretemp_pkg_path.empty()) {
    cpu_path_ = coretemp_pkg_path;
  }
}

ThermalStats ThermalCollector::sample() {
  if (!discovered_) discover();

  ThermalStats stats;
  if (!cpu_path_.empty()) {
    const std::string raw = readFile(cpu_path_);
    if (!raw.empty()) stats.cpu_temp_c = std::stod(raw) / 1000.0;
  }
  if (!gpu_path_.empty()) {
    const std::string raw = readFile(gpu_path_);
    if (!raw.empty()) stats.gpu_temp_c = std::stod(raw) / 1000.0;
  }

  // Meteor Lake and most recent mobile Intel parts start clamping clocks
  // in the ~95-100C band; there's no portable "am I throttled right now"
  // sysfs flag, so this is a practical proxy rather than an exact signal.
  if (stats.cpu_temp_c >= 95.0) stats.throttled = true;

  return stats;
}

}  // namespace whyhot
