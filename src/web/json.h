#pragma once

#include <string>

#include "correlation/engine.h"

namespace whyhot {

// Serializes the current engine state (latest sample, active incident,
// recent closed incidents) as a single JSON object for the web UI to poll.
std::string buildStateJson(const CorrelationEngine& engine, const std::string& log_path);

}  // namespace whyhot
