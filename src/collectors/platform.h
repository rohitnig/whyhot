#pragma once

#include <string>

namespace whyhot {

struct PlatformStats {
  std::string profile;  // "low-power" | "balanced" | "performance"; empty if unavailable
  bool available = false;
};

// Reads /sys/firmware/acpi/platform_profile - the ACPI knob behind GNOME's
// power mode / TLP / powerprofilesctl. Switching it changes the fan curve
// and power limits, which is exactly the kind of thing that can explain a
// thermal incident (or its absence) independent of CPU load.
class PlatformCollector {
 public:
  PlatformStats sample();
};

}  // namespace whyhot
