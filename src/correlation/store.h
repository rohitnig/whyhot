#pragma once

#include <string>
#include <vector>

#include "correlation/engine.h"

namespace whyhot {

// Appends incidents to a plain-text, greppable log
// ($XDG_STATE_HOME/whyhot/incidents.log, falling back to
// ~/.local/state/whyhot/incidents.log) and reads them back for `whyhot
// explain` / `whyhot list`, which run as separate short-lived processes
// from the `whyhot watch`/`serve` loop that recorded them.
//
// Two record types are written per incident: an [incident_start] record
// the moment it opens (so it survives a crash/kill before it closes), and
// an [incident] record with the full verdict when it closes. loadAll()
// reconciles the two: an id with a start but no matching close comes back
// with Incident::incomplete set, so a watcher that died mid-incident is
// reported rather than silently dropped.
class IncidentStore {
 public:
  explicit IncidentStore(std::string path);

  void appendStart(const Incident& incident) const;
  void append(const Incident& incident) const;
  std::vector<Incident> loadAll() const;

  // Highest incident id seen in the log (0 if empty), so a fresh run can
  // continue numbering instead of restarting at 1 and colliding with ids
  // from a previous run still sitting in the log as incomplete.
  int maxKnownId() const;

  static std::string defaultPath();

 private:
  std::string path_;
};

}  // namespace whyhot
