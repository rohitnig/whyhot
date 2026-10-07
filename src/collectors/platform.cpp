#include "collectors/platform.h"

#include <cctype>
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

PlatformStats PlatformCollector::sample() {
  PlatformStats stats;
  std::ifstream f("/sys/firmware/acpi/platform_profile");
  if (!f) return stats;

  std::string line;
  std::getline(f, line);
  line = trim(line);
  if (line.empty()) return stats;

  stats.profile = line;
  stats.available = true;
  return stats;
}

}  // namespace whyhot
