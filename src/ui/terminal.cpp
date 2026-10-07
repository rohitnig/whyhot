#include "ui/terminal.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace whyhot {

namespace {

constexpr int kWidth = 62;

std::string hr() {
  std::string s;
  for (int i = 0; i < kWidth; ++i) s += "\xe2\x94\x80";  // UTF-8 for U+2500
  return s;
}

std::string fmtDuration(int total_seconds) {
  if (total_seconds < 0) total_seconds = 0;
  const int h = total_seconds / 3600;
  const int m = (total_seconds % 3600) / 60;
  const int s = total_seconds % 60;
  char buf[32];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%dh %dm %ds", h, m, s);
  } else if (m > 0) {
    std::snprintf(buf, sizeof(buf), "%dm %ds", m, s);
  } else {
    std::snprintf(buf, sizeof(buf), "%ds", s);
  }
  return buf;
}

std::string fmtDayTime(Clock::time_point tp) { return formatClockTime(tp); }

int secondsBetween(Clock::time_point a, Clock::time_point b) {
  return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(b - a).count());
}

enum class Sev { kNone, kGood, kWarn, kBad };

// Ascending metric (higher = worse), e.g. CPU% or temperature. `value < 0`
// is treated as "unavailable" and left uncolored.
Sev levelAsc(double value, double warn_at, double bad_at) {
  if (value < 0) return Sev::kNone;
  if (value >= bad_at) return Sev::kBad;
  if (value >= warn_at) return Sev::kWarn;
  return Sev::kGood;
}

std::string colorize(const std::string& text, Sev sev) {
  switch (sev) {
    case Sev::kGood: return "\033[32m" + text + "\033[0m";
    case Sev::kWarn: return "\033[33m" + text + "\033[0m";
    case Sev::kBad: return "\033[31m" + text + "\033[0m";
    default: return text;
  }
}

}  // namespace

void TerminalUi::enterAltScreen() {
  std::cout << "\033[?1049h\033[?25l" << std::flush;
}

void TerminalUi::leaveAltScreen() {
  std::cout << "\033[?25h\033[?1049l" << std::flush;
}

void TerminalUi::renderLive(const CorrelationEngine& engine, const std::string& log_path) {
  std::ostringstream out;
  const Sample* s = engine.latest();
  const Incident* active_inc = engine.active();

  if (nproc_ <= 0) nproc_ = sysconf(_SC_NPROCESSORS_ONLN);

  out << "\033[H";  // cursor home; we redraw the whole frame each tick
  out << "WHYHOT v0.1" << std::string(kWidth - 11 - 8, ' ')
      << (s ? fmtDayTime(s->ts) : "--:--:--") << "\n";
  out << hr() << "\n";

  if (s) {
    char field[32];

    std::snprintf(field, sizeof(field), "%5.1f%%", s->cpu.total_util_pct);
    const std::string cpu_f = colorize(field, levelAsc(s->cpu.total_util_pct, 20, 50));

    std::snprintf(field, sizeof(field), "%5.1fC", s->thermal.cpu_temp_c);
    const Sev temp_sev = s->thermal.throttled ? Sev::kBad : levelAsc(s->thermal.cpu_temp_c, 70, 90);
    const std::string temp_f = colorize(field, temp_sev);

    Sev fan_sev = Sev::kNone;
    if (s->fan.available) {
      fan_min_seen_ = fan_min_seen_ < 0 ? s->fan.rpm : std::min(fan_min_seen_, static_cast<long>(s->fan.rpm));
      fan_sev = levelAsc(s->fan.rpm - fan_min_seen_, 500, 1500);
    }
    std::snprintf(field, sizeof(field), "%5d RPM", s->fan.rpm);
    const std::string fan_f = colorize(field, fan_sev);

    out << "CPU  " << cpu_f << "    Temp  " << temp_f << "    Fan  " << fan_f << "\n";

    const double load_ratio = nproc_ > 0 ? s->cpu.load_avg1 / static_cast<double>(nproc_) : -1;
    char line[160];
    std::snprintf(line, sizeof(line), "%.2f %.2f %.2f", s->cpu.load_avg1, s->cpu.load_avg5,
                  s->cpu.load_avg15);
    out << "Load " << colorize(line, levelAsc(load_ratio, 0.7, 1.5)) << "    Profile "
        << (s->platform.available ? s->platform.profile : "n/a") << "\n\n";

    out << "TOP PROCESSES\n";
    for (const auto& p : s->top_processes) {
      std::snprintf(line, sizeof(line), "  %-20.20s %7d  %5.1f%%", p.comm.c_str(), p.pid,
                    p.cpu_pct);
      Sev row_sev = Sev::kNone;
      if (active_inc && p.pid == active_inc->trigger_pid) row_sev = Sev::kBad;
      else if (p.cpu_pct >= 30.0) row_sev = Sev::kWarn;
      out << colorize(line, row_sev) << "\n";
    }
  } else {
    out << "(collecting first sample...)\n";
  }

  out << "\n" << hr() << "\n";

  if (const Incident* inc = active_inc) {
    const int elapsed = s ? secondsBetween(inc->start_ts, s->ts) : 0;
    out << "\xf0\x9f\x94\xa5 INCIDENT #" << inc->id << "  (started " << fmtDayTime(inc->start_ts)
        << ", " << fmtDuration(elapsed) << " elapsed)\n\n";
    const size_t start = inc->timeline.size() > 8 ? inc->timeline.size() - 8 : 0;
    for (size_t i = start; i < inc->timeline.size(); ++i) {
      out << fmtDayTime(inc->timeline[i].ts) << "  " << inc->timeline[i].text << "\n";
    }
    out << "\nWatching for return to baseline...\n";
  } else {
    out << "No active incident. Monitoring.\n";
  }

  out << "\n" << hr() << "\n";
  out << "RECENT INCIDENTS\n";
  if (engine.recentClosed().empty()) {
    out << "  (none yet this session)\n";
  } else {
    int shown = 0;
    for (const auto& inc : engine.recentClosed()) {
      if (shown++ >= 5) break;
      char line[160];
      std::snprintf(line, sizeof(line), "  #%-3d %s-%s  %-14.14s conf %-6s fan %s", inc.id,
                    fmtDayTime(inc.start_ts).c_str(), fmtDayTime(inc.end_ts).c_str(),
                    inc.primary_contributor.c_str(), toString(inc.confidence).c_str(),
                    toString(inc.fan_response).c_str());
      out << line << "\n";
    }
  }

  out << "\n" << hr() << "\n";
  out << "Log: " << log_path << "\n";
  out << "Colors: CPU 20/50%  Temp 70C/throttle  Load 0.7x/1.5x cores  "
         "Fan vs session-min  Proc red=trigger amber>=30%\n";
  out << "\033[J";  // clear anything left over from a previous, longer frame

  std::cout << out.str() << std::flush;
}

namespace {

std::string describeTriggerForDisplay(const Incident& incident) {
  if (incident.trigger == "process_cpu") {
    return incident.trigger_process + " (pid " + std::to_string(incident.trigger_pid) +
           ") crossing 30% CPU";
  }
  if (incident.trigger == "temp_rise") return "CPU temperature rising sharply";
  if (incident.trigger == "fan_rise") return "fan speed rising sharply";
  return "unknown";
}

}  // namespace

std::string buildExplainText(const Incident& incident) {
  std::ostringstream out;

  if (incident.incomplete) {
    out << "WHY DID MY FAN RUN?\n\n";
    out << "Incident #" << incident.id << " started " << formatClockTime(incident.start_ts)
        << " - INCOMPLETE\n";
    out << std::string(kWidth, '-') << "\n\n";
    out << "The watcher was killed, crashed, or the system went down before\n"
           "this incident settled, so no verdict was computed. It was\n"
           "triggered by:\n\n";
    out << "  " << describeTriggerForDisplay(incident) << "\n\n";
    out << "Baseline at the time it opened:\n";
    char line[128];
    std::snprintf(line, sizeof(line), "  CPU temp  ~%.1fC", incident.baseline_cpu_temp);
    out << line << "\n";
    std::snprintf(line, sizeof(line), "  Fan       ~%.0f RPM", incident.baseline_fan_rpm);
    out << line << "\n";
    return out.str();
  }

  out << "WHY DID MY FAN RUN?\n\n";
  out << "Incident #" << incident.id << ": " << formatClockTime(incident.start_ts) << "-"
      << formatClockTime(incident.end_ts) << "\n";
  out << std::string(kWidth, '-') << "\n\n";

  // A cooldown-closed incident where neither temperature nor fan moved by a
  // meaningful amount has no real consequence to explain, even though a
  // process did cross the trigger threshold - usually a brief CPU blip
  // (e.g. a short-lived command-line tool). Naming a confident "Cause" in
  // that case is actively misleading, so say plainly that nothing happened
  // instead of dressing up noise as a finding.
  const bool trivial = !incident.closed_via_timeout &&
                        incident.thermal_correlation == Level::kNone &&
                        incident.fan_response == FanResponse::kMuted;
  if (trivial) {
    char line[128];
    out << "No real thermal consequence found.\n\n";
    out << "This was triggered by " << describeTriggerForDisplay(incident)
        << ", but CPU temperature and\nfan speed both stayed within noise of their pre-incident "
           "baseline for its\nentire "
        << fmtDuration(secondsBetween(incident.start_ts, incident.end_ts))
        << ". Likely a brief CPU blip rather than anything that\nactually made the machine run "
           "hotter or louder.\n\n";
    out << "Measured change:\n";
    std::snprintf(line, sizeof(line), "  CPU temperature  %+.1fC",
                  incident.peak_cpu_temp - incident.start_cpu_temp);
    out << line << "\n";
    std::snprintf(line, sizeof(line), "  Fan speed        %+.0f RPM",
                  incident.peak_fan_rpm - incident.start_fan_rpm);
    out << line << "\n";
    return out.str();
  }

  const bool has_contributor = incident.primary_contributor != "system" &&
                                incident.primary_contributor != "unknown";

  out << "Cause:\n";
  if (has_contributor) {
    out << "  " << incident.primary_contributor << " CPU activity\n\n";
  } else {
    out << "  Unclear - no single process crossed the contributor threshold\n\n";
  }

  out << "Evidence:\n";
  char line[128];
  if (has_contributor) {
    std::snprintf(line, sizeof(line), "  %-16s+%.0f%%", incident.primary_contributor.c_str(),
                  incident.contributor_peak_cpu_pct - incident.baseline_cpu_util);
    out << line << " CPU\n";
  }
  std::snprintf(line, sizeof(line), "  %-16s%+.1fC", "CPU temperature",
                incident.peak_cpu_temp - incident.start_cpu_temp);
  out << line << "\n";
  std::snprintf(line, sizeof(line), "  %-16s%+.0f RPM", "Fan speed",
                incident.peak_fan_rpm - incident.start_fan_rpm);
  out << line << "\n\n";

  out << "Sequence:\n";
  if (has_contributor) {
    out << "  " << incident.primary_contributor << " load up\n       v\n";
  }
  out << "  CPU temperature up\n       v\n  fan speed up\n";
  if (incident.closed_via_timeout) {
    out << "       v\n  (fan had not returned to baseline after 30 minutes)\n\n";
  } else {
    out << "       v\n  load down\n       v\n  temperature down\n\n";
  }

  out << "Assessment:\n";
  std::string assessment;
  switch (incident.confidence) {
    case Level::kHigh: assessment = "Strong temporal correlation."; break;
    case Level::kMed: assessment = "Partial correlation - some signals line up, others don't."; break;
    default: assessment = "Weak correlation - fan/temperature moves don't clearly track load."; break;
  }
  if (incident.fan_response == FanResponse::kStuck) {
    assessment += " Fan stayed elevated well past the load/thermal event - possibly a"
                  " firmware or EC fan-curve issue rather than load.";
  }
  out << "  " << assessment << "\n\n";

  out << "Not evaluated in this build (v0.1 does not monitor these):\n";
  out << "  GPU load, disk I/O, kernel/interrupt CPU time\n";
  return out.str();
}

void printExplain(const Incident& incident) { std::cout << buildExplainText(incident); }

void printList(const std::vector<Incident>& incidents) {
  if (incidents.empty()) {
    std::cout << "No incidents recorded yet.\n";
    return;
  }
  for (const auto& inc : incidents) {
    char line[160];
    if (inc.incomplete) {
      std::snprintf(line, sizeof(line), "#%-3d %s-?         INCOMPLETE     trigger %s",
                    inc.id, formatClockTime(inc.start_ts).c_str(),
                    describeTriggerForDisplay(inc).c_str());
    } else {
      std::snprintf(line, sizeof(line), "#%-3d %s-%s  %-14.14s conf %-6s fan %-6s temp %+.0fC",
                    inc.id, formatClockTime(inc.start_ts).c_str(),
                    formatClockTime(inc.end_ts).c_str(), inc.primary_contributor.c_str(),
                    toString(inc.confidence).c_str(), toString(inc.fan_response).c_str(),
                    inc.peak_cpu_temp - inc.start_cpu_temp);
    }
    std::cout << line << "\n";
  }
}

}  // namespace whyhot
