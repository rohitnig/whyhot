#pragma once

#include <string>
#include <vector>

#include "correlation/engine.h"

namespace whyhot {

class TerminalUi {
 public:
  void enterAltScreen();
  void leaveAltScreen();

  // Full-screen live view used by `whyhot watch`.
  void renderLive(const CorrelationEngine& engine, const std::string& log_path);

 private:
  // "Normal" fan RPM varies by machine, so color it relative to the lowest
  // this session has actually observed rather than a guessed constant.
  long fan_min_seen_ = -1;
  long nproc_ = -1;  // lazily resolved via sysconf on first render
};

// Human-readable post-mortem for one incident. Shared by `whyhot explain`
// and the web UI's per-incident detail view, so both stay in sync.
std::string buildExplainText(const Incident& incident);

// `whyhot explain [id]` — prints buildExplainText() to stdout.
void printExplain(const Incident& incident);

// `whyhot list` — one line per recorded incident, most recent last.
void printList(const std::vector<Incident>& incidents);

}  // namespace whyhot
