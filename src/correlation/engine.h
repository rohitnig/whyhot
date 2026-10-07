#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "collectors/cpu.h"
#include "collectors/fan.h"
#include "collectors/platform.h"
#include "collectors/process.h"
#include "collectors/thermal.h"

namespace whyhot {

using Clock = std::chrono::system_clock;

struct Sample {
  Clock::time_point ts;
  CpuStats cpu;
  ThermalStats thermal;
  FanStats fan;
  PlatformStats platform;
  std::vector<ProcessInfo> top_processes;
};

struct TimelineEntry {
  Clock::time_point ts;
  std::string text;
};

enum class Level { kNone, kLow, kMed, kHigh };
enum class FanResponse { kNone, kMuted, kNormal, kDelayed, kStuck };

std::string toString(Level level);
std::string toString(FanResponse response);
Level levelFromString(const std::string& s);
FanResponse fanResponseFromString(const std::string& s);
std::string formatClockTime(Clock::time_point tp);  // "HH:MM:SS", local time

// A bounded window of abnormal activity: something (temperature, fan, a
// process) moved sharply, and we record what happened until things settle
// back near their pre-incident baseline (or we give up after 30 minutes).
struct Incident {
  int id = 0;
  Clock::time_point start_ts;
  Clock::time_point end_ts;  // only meaningful once closed
  bool closed = false;
  bool closed_via_timeout = false;  // hit the 30-min cap instead of cooling down

  // True only when this Incident was reconstructed from an on-disk
  // [incident_start] record that has no matching close record - i.e. the
  // watcher was killed, crashed, or the machine went down while it was
  // open. Every other field below is whatever was known at open time;
  // there is no verdict.
  bool incomplete = false;

  std::string trigger;          // "temp_rise" | "fan_rise" | "process_cpu"
  std::string trigger_process;  // set when trigger == "process_cpu"
  int trigger_pid = -1;

  double baseline_cpu_temp = -1.0;
  double baseline_fan_rpm = -1.0;
  double baseline_cpu_util = -1.0;

  double start_cpu_temp = -1.0;
  double peak_cpu_temp = -1.0;
  double start_fan_rpm = -1.0;
  double peak_fan_rpm = -1.0;

  std::vector<TimelineEntry> timeline;

  // Correlation verdict, filled in when the incident closes.
  std::string primary_contributor;  // process comm, or "system"/"unknown"
  double contributor_peak_cpu_pct = 0.0;
  double contributor_start_cpu_pct = 0.0;
  Level cpu_contribution = Level::kNone;
  Level thermal_correlation = Level::kNone;
  FanResponse fan_response = FanResponse::kNone;
  Level confidence = Level::kNone;
};

// What tripped the trigger, so the incident record (and its on-disk
// [incident_start] entry, written immediately in case the process dies
// before the incident closes) can say why it opened.
struct TriggerHit {
  std::string kind;  // "temp_rise" | "fan_rise" | "process_cpu"
  std::string process;
  int pid = -1;
};

// Watches the 1Hz sample stream for sharp moves in temperature, fan speed,
// or per-process CPU, opens an Incident while things stay abnormal, and
// closes it with a correlation verdict once the system settles back down
// (or after a 30-minute cap). No ML, just thresholds and timing - see
// engine.cpp for the exact rules.
class CorrelationEngine {
 public:
  explicit CorrelationEngine(std::chrono::minutes history_window = std::chrono::minutes(30));

  // Returns a completed Incident if one just closed on this sample.
  std::optional<Incident> addSample(Sample sample, std::vector<ProcessEvent> events);

  // So incident ids don't collide with ones already in the on-disk log from
  // a previous run (which would confuse the incomplete-incident reconciler
  // in IncidentStore::loadAll). Call once at startup, before the first
  // addSample.
  void setNextId(int n) { next_id_ = n; }

  // Pre-populates recentClosed() from the on-disk log at startup, so a
  // freshly (re)started process's live view isn't misleadingly empty just
  // because nothing has closed yet during its own runtime. `incidents`
  // should be closed (non-incomplete) records, oldest first; only the tail
  // up to the usual recentClosed() cap is kept.
  void seedRecentClosed(const std::vector<Incident>& incidents);

  // Non-null on the one addSample() call that just opened an incident, so
  // the caller can persist its [incident_start] record immediately.
  const Incident* justOpened() const { return opened_this_tick_ ? &*active_ : nullptr; }

  bool hasActive() const { return active_.has_value(); }
  const Incident* active() const { return active_ ? &*active_ : nullptr; }
  const std::deque<Incident>& recentClosed() const { return closed_; }
  const std::deque<Sample>& history() const { return history_; }
  const Sample* latest() const { return history_.empty() ? nullptr : &history_.back(); }

 private:
  std::chrono::minutes history_window_;
  std::deque<Sample> history_;
  std::optional<Sample> prev_sample_;
  std::optional<Incident> active_;
  std::deque<Incident> closed_;
  int next_id_ = 1;
  int quiet_streak_ = 0;
  bool opened_this_tick_ = false;

  void trimHistory(Clock::time_point now);
  const Sample* sampleSecondsAgo(int seconds) const;
  std::optional<TriggerHit> checkTrigger(const Sample& s) const;
  Incident startIncident(const Sample& s, const TriggerHit& trigger);
  bool isQuiet(const Sample& s) const;
  void appendTimelineDiffs(Incident& incident, const std::optional<Sample>& prev,
                            const Sample& cur, const std::vector<ProcessEvent>& events);
  void finalizeIncident(Incident& incident, const Sample& last, bool via_cooldown);
};

}  // namespace whyhot
