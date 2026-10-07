#include "collectors/fan.h"

#include <cctype>
#include <dirent.h>
#include <fstream>

namespace whyhot {

namespace {

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

}  // namespace

void FanCollector::discover() {
  discovered_ = true;

  DIR* hwmon_dir = opendir("/sys/class/hwmon");
  if (!hwmon_dir) return;

  struct dirent* entry;
  while ((entry = readdir(hwmon_dir)) != nullptr) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    const std::string candidate =
        "/sys/class/hwmon/" + name + "/fan1_input";
    std::ifstream probe(candidate);
    if (probe) {
      fan_path_ = candidate;
      break;
    }
  }
  closedir(hwmon_dir);
}

FanStats FanCollector::sample() {
  if (!discovered_) discover();

  FanStats stats;
  if (fan_path_.empty()) return stats;

  std::ifstream f(fan_path_);
  if (!f) return stats;
  std::string line;
  std::getline(f, line);
  line = trim(line);
  if (line.empty()) return stats;

  try {
    stats.rpm = std::stoi(line);
    stats.available = true;
  } catch (...) {
  }
  return stats;
}

}  // namespace whyhot
