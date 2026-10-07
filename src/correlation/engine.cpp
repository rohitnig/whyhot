#include "correlation/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace whyhot {

namespace {

// Trigger thresholds: any one of these opens a new incident.
constexpr double kTempRiseTriggerC = 8.0;     // over the trailing window below
constexpr int kFanRiseTriggerRpm = 800;
constexpr double kProcCpuTriggerPct = 30.0;
constexpr int kTriggerWindowSeconds = 10;

// An incident closes once things stay this close to baseline for this long.
constexpr double kTempReturnToleranceC = 3.0;
constexpr int kFanReturnToleranceRpm = 300;
constexpr int kCooldownSeconds = 20;
constexpr int kMaxIncidentSeconds = 30 * 60;

// A process must peak above this to be named "primary contributor".
constexpr double kContributorMinPeakPct = 15.0;

// How many closed incidents recentClosed() keeps around for the live view.
constexpr size_t kMaxRecentClosed = 20;

int secondsBetween(Clock::time_point a, Clock::time_point b) {
  return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(b - a).count());
}

}  // namespace

std::string toString(Level level) {
  switch (level) {
    case Level::kNone: return "NONE";
    case Level::kLow: return "LOW";
    case Level::kMed: return "MEDIUM";
    case Level::kHigh: return "HIGH";
  }
  return "NONE";
}

Level levelFromString(const std::string& s) {
  if (s == "LOW") return Level::kLow;
  if (s == "MEDIUM") return Level::kMed;
  if (s == "HIGH") return Level::kHigh;
  return Level::kNone;
}

std::string toString(FanResponse response) {
  switch (response) {
    case FanResponse::kNone: return "NONE";
    case FanResponse::kMuted: return "MUTED";
    case FanResponse::kNormal: return "NORMAL";
    case FanResponse::kDelayed: return "DELAYED";
    case FanResponse::kStuck: return "STUCK";
  }
  return "NONE";
}

FanResponse fanResponseFromString(const std::string& s) {
  if (s == "MUTED") return FanResponse::kMuted;
  if (s == "NORMAL") return FanResponse::kNormal;
  if (s == "DELAYED") return FanResponse::kDelayed;
  if (s == "STUCK") return FanResponse::kStuck;
  return FanResponse::kNone;
}

std::string formatClockTime(Clock::time_point tp) {
  const std::time_t t = Clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

CorrelationEngine::CorrelationEngine(std::chrono::minutes history_window)
    : history_window_(history_window) {}

void CorrelationEngine::seedRecentClosed(const std::vector<Incident>& incidents) {
  for (const auto& inc : incidents) {
    if (!inc.closed) continue;
    closed_.push_front(inc);
    if (closed_.size() > kMaxRecentClosed) closed_.pop_back();
  }
}

void CorrelationEngine::trimHistory(Clock::time_point now) {
  while (!history_.empty() && now - history_.front().ts > history_window_) {
    history_.pop_front();
  }
}

const Sample* CorrelationEngine::sampleSecondsAgo(int seconds) const {
  if (history_.empty()) return nullptr;
  const Clock::time_point target = history_.back().ts - std::chrono::seconds(seconds);
  const Sample* best = nullptr;
  for (const auto& s : history_) {
    if (s.ts <= target) {
      best = &s;
    } else {
      break;
    }
  }
  return best;
}

std::optional<TriggerHit> CorrelationEngine::checkTrigger(const Sample& s) const {
  if (const Sample* past = sampleSecondsAgo(kTriggerWindowSeconds)) {
    if (s.thermal.cpu_temp_c > 0 && past->thermal.cpu_temp_c > 0 &&
        s.thermal.cpu_temp_c - past->thermal.cpu_temp_c >= kTempRiseTriggerC) {
      return TriggerHit{"temp_rise", "", -1};
    }
    if (s.fan.available && past->fan.available &&
        s.fan.rpm - past->fan.rpm >= kFanRiseTriggerRpm) {
      return TriggerHit{"fan_rise", "", -1};
    }
  }
  for (const auto& p : s.top_processes) {
    if (p.cpu_pct >= kProcCpuTriggerPct) return TriggerHit{"process_cpu", p.comm, p.pid};
  }
  return std::nullopt;
}

namespace {

std::string describeTrigger(const TriggerHit& t) {
  if (t.kind == "temp_rise") return "CPU temperature rising";
  if (t.kind == "fan_rise") return "fan speed rising";
  if (t.kind == "process_cpu") return t.process + " crossed 30% CPU";
  return "unknown trigger";
}

}  // namespace

Incident CorrelationEngine::startIncident(const Sample& s, const TriggerHit& trigger) {
  Incident incident;
  incident.id = next_id_++;
  incident.start_ts = s.ts;
  incident.trigger = trigger.kind;
  incident.trigger_process = trigger.process;
  incident.trigger_pid = trigger.pid;
  incident.start_cpu_temp = s.thermal.cpu_temp_c;
  incident.peak_cpu_temp = s.thermal.cpu_temp_c;
  incident.start_fan_rpm = s.fan.rpm;
  incident.peak_fan_rpm = s.fan.rpm;

  // Baseline = average of the ~15s before the trigger, so "back to normal"
  // means something even if the whole session has been running warm.
  constexpr int kBaselineWindowSeconds = 15;
  double sum_temp = 0, sum_fan = 0, sum_util = 0;
  int n_temp = 0, n_fan = 0, n_util = 0;
  const Clock::time_point cutoff = s.ts - std::chrono::seconds(kBaselineWindowSeconds);
  for (const auto& h : history_) {
    if (h.ts < cutoff) continue;
    if (h.thermal.cpu_temp_c > 0) { sum_temp += h.thermal.cpu_temp_c; ++n_temp; }
    if (h.fan.available) { sum_fan += h.fan.rpm; ++n_fan; }
    sum_util += h.cpu.total_util_pct; ++n_util;
  }
  incident.baseline_cpu_temp = n_temp ? sum_temp / n_temp : s.thermal.cpu_temp_c;
  incident.baseline_fan_rpm = n_fan ? sum_fan / n_fan : s.fan.rpm;
  incident.baseline_cpu_util = n_util ? sum_util / n_util : s.cpu.total_util_pct;

  incident.timeline.push_back(
      {s.ts, "Incident #" + std::to_string(incident.id) + " opened (" + describeTrigger(trigger) + ")"});
  return incident;
}

bool CorrelationEngine::isQuiet(const Sample& s) const {
  if (!active_) return true;
  if (active_->baseline_cpu_temp > 0 && s.thermal.cpu_temp_c > 0 &&
      s.thermal.cpu_temp_c - active_->baseline_cpu_temp > kTempReturnToleranceC) {
    return false;
  }
  if (active_->baseline_fan_rpm > 0 && s.fan.available &&
      s.fan.rpm - active_->baseline_fan_rpm > kFanReturnToleranceRpm) {
    return false;
  }
  for (const auto& p : s.top_processes) {
    if (p.cpu_pct >= kProcCpuTriggerPct) return false;
  }
  return true;
}

void CorrelationEngine::appendTimelineDiffs(Incident& incident, const std::optional<Sample>& prev,
                                             const Sample& cur,
                                             const std::vector<ProcessEvent>& events) {
  auto findPrevProc = [&](int pid) -> const ProcessInfo* {
    if (!prev) return nullptr;
    for (const auto& p : prev->top_processes) if (p.pid == pid) return &p;
    return nullptr;
  };

  for (const auto& p : cur.top_processes) {
    const ProcessInfo* before = findPrevProc(p.pid);
    const double prev_pct = before ? before->cpu_pct : 0.0;
    if (p.cpu_pct >= kProcCpuTriggerPct && prev_pct < kProcCpuTriggerPct) {
      incident.timeline.push_back(
          {cur.ts, p.comm + " PID " + std::to_string(p.pid) + " -> " +
                       std::to_string(static_cast<int>(p.cpu_pct)) + "% CPU"});
    } else if (prev_pct >= kProcCpuTriggerPct && p.cpu_pct < kProcCpuTriggerPct) {
      incident.timeline.push_back({cur.ts, p.comm + " CPU fell"});
    }
  }

  if (prev) {
    if (prev->thermal.cpu_temp_c > 0 && cur.thermal.cpu_temp_c > 0) {
      const int prev_band = static_cast<int>(prev->thermal.cpu_temp_c / 5);
      const int cur_band = static_cast<int>(cur.thermal.cpu_temp_c / 5);
      if (prev_band != cur_band) {
        incident.timeline.push_back(
            {cur.ts, "CPU package " + std::to_string(static_cast<int>(prev->thermal.cpu_temp_c)) +
                         " -> " + std::to_string(static_cast<int>(cur.thermal.cpu_temp_c)) + "C"});
      }
    }
    if (prev->fan.available && cur.fan.available) {
      const int prev_band = prev->fan.rpm / 500;
      const int cur_band = cur.fan.rpm / 500;
      if (prev_band != cur_band) {
        incident.timeline.push_back({cur.ts, "FAN " + std::to_string(prev->fan.rpm) + " -> " +
                                                  std::to_string(cur.fan.rpm) + " RPM"});
      }
    }
    if (prev->platform.available && cur.platform.available &&
        prev->platform.profile != cur.platform.profile) {
      incident.timeline.push_back({cur.ts, "platform profile: " + prev->platform.profile +
                                                " -> " + cur.platform.profile});
    }
  }

  // Only note lifecycle events for processes relevant enough to have shown
  // up in the top-N list; otherwise routine short-lived processes flood
  // the timeline.
  for (const auto& ev : events) {
    const bool relevant = std::any_of(cur.top_processes.begin(), cur.top_processes.end(),
                                       [&](const ProcessInfo& p) { return p.pid == ev.pid; }) ||
                           (prev && std::any_of(prev->top_processes.begin(), prev->top_processes.end(),
                                                 [&](const ProcessInfo& p) { return p.pid == ev.pid; }));
    if (!relevant) continue;
    const char* verb = ev.type == ProcessEvent::Type::kStart ? "started" : "exited";
    incident.timeline.push_back(
        {cur.ts, ev.comm + " PID " + std::to_string(ev.pid) + " " + verb});
  }
}

void CorrelationEngine::finalizeIncident(Incident& incident, const Sample& last, bool via_cooldown) {
  incident.closed = true;
  incident.closed_via_timeout = !via_cooldown;
  incident.end_ts = via_cooldown ? last.ts - std::chrono::seconds(quiet_streak_) : last.ts;

  std::map<std::string, std::pair<double, int>> sum_count;  // comm -> (sum_pct, n)
  std::map<std::string, double> peak;
  double sum_util = 0, peak_util = 0;
  int n_util = 0;

  for (const auto& h : history_) {
    if (h.ts < incident.start_ts || h.ts > last.ts) continue;
    for (const auto& p : h.top_processes) {
      sum_count[p.comm].first += p.cpu_pct;
      sum_count[p.comm].second += 1;
      peak[p.comm] = std::max(peak[p.comm], p.cpu_pct);
    }
    sum_util += h.cpu.total_util_pct;
    peak_util = std::max(peak_util, h.cpu.total_util_pct);
    ++n_util;
  }

  std::string best_comm;
  double best_avg = -1;
  for (const auto& [comm, sc] : sum_count) {
    if (peak[comm] < kContributorMinPeakPct) continue;
    const double avg = sc.first / std::max(1, sc.second);
    if (avg > best_avg) {
      best_avg = avg;
      best_comm = comm;
    }
  }
  incident.primary_contributor = best_comm.empty() ? "system" : best_comm;
  incident.contributor_peak_cpu_pct = best_comm.empty() ? 0.0 : peak[best_comm];

  const double avg_util = n_util ? sum_util / n_util : 0.0;
  incident.cpu_contribution = avg_util >= 50 ? Level::kHigh : avg_util >= 20 ? Level::kMed : Level::kLow;

  // "Did load actually get elevated during this incident" - compared against
  // the pre-incident baseline rather than an incident-internal split, so it
  // still catches a load spike that has already fallen back by the time the
  // incident closes (the common rise-then-fall shape).
  const bool load_rose =
      (peak_util - incident.baseline_cpu_util) >= 15.0 || avg_util >= 40.0;
  const double temp_delta = incident.peak_cpu_temp - incident.start_cpu_temp;
  if (temp_delta < 3.0) {
    incident.thermal_correlation = Level::kNone;
  } else if (temp_delta >= 3.0 && !load_rose) {
    incident.thermal_correlation = Level::kLow;  // temperature moved without matching load
  } else if (temp_delta >= kTempRiseTriggerC && load_rose) {
    incident.thermal_correlation = Level::kHigh;
  } else {
    incident.thermal_correlation = Level::kMed;
  }

  const double fan_delta = incident.peak_fan_rpm - incident.start_fan_rpm;
  if (!incident.closed_via_timeout) {
    incident.fan_response = fan_delta >= kFanReturnToleranceRpm ? FanResponse::kNormal
                                                                  : FanResponse::kMuted;
  } else {
    incident.fan_response = fan_delta >= kFanReturnToleranceRpm ? FanResponse::kStuck
                                                                  : FanResponse::kDelayed;
  }

  const bool clear_contributor = !best_comm.empty();
  if (incident.thermal_correlation == Level::kHigh && clear_contributor &&
      (incident.fan_response == FanResponse::kNormal || incident.fan_response == FanResponse::kStuck)) {
    incident.confidence = Level::kHigh;
  } else if (incident.thermal_correlation == Level::kNone) {
    incident.confidence = Level::kLow;
  } else {
    incident.confidence = Level::kMed;
  }
}

std::optional<Incident> CorrelationEngine::addSample(Sample s, std::vector<ProcessEvent> events) {
  history_.push_back(s);
  trimHistory(s.ts);

  std::optional<Incident> closed_result;
  opened_this_tick_ = false;

  if (active_) {
    appendTimelineDiffs(*active_, prev_sample_, s, events);
    active_->peak_cpu_temp = std::max(active_->peak_cpu_temp, s.thermal.cpu_temp_c);
    active_->peak_fan_rpm = std::max(active_->peak_fan_rpm, static_cast<double>(s.fan.rpm));

    const bool quiet = isQuiet(s);
    quiet_streak_ = quiet ? quiet_streak_ + 1 : 0;
    const int elapsed = secondsBetween(active_->start_ts, s.ts);

    if (quiet_streak_ >= kCooldownSeconds || elapsed >= kMaxIncidentSeconds) {
      finalizeIncident(*active_, s, quiet_streak_ >= kCooldownSeconds);
      closed_result = active_;
      closed_.push_front(*active_);
      if (closed_.size() > kMaxRecentClosed) closed_.pop_back();
      active_.reset();
      quiet_streak_ = 0;
    }
  } else if (auto trigger = checkTrigger(s)) {
    active_ = startIncident(s, *trigger);
    appendTimelineDiffs(*active_, prev_sample_, s, events);
    opened_this_tick_ = true;
  }

  prev_sample_ = s;
  return closed_result;
}

}  // namespace whyhot
