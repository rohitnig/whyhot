#include "web/json.h"

#include <unistd.h>

#include <cstdio>
#include <sstream>

#include "ui/terminal.h"

namespace whyhot {

namespace {

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string jstr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

int secondsBetween(Clock::time_point a, Clock::time_point b) {
  return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(b - a).count());
}

// The web UI never shows more than the last 10 timeline lines (see
// page.h), and a long-running incident's timeline can run into the
// hundreds of entries - sending the whole thing every ~2s poll bloated the
// response past what a single send() call could deliver in one shot (see
// sendAll() in server.cpp), which silently truncated the JSON. Cap what
// goes over the wire to what's actually ever displayed.
constexpr size_t kMaxTimelineEntriesSent = 20;

void writeTimelineTail(std::ostringstream& out, const std::vector<TimelineEntry>& timeline) {
  const size_t start = timeline.size() > kMaxTimelineEntriesSent
                            ? timeline.size() - kMaxTimelineEntriesSent
                            : 0;
  out << "[";
  for (size_t i = start; i < timeline.size(); ++i) {
    if (i != start) out << ",";
    out << "{\"time\":" << jstr(formatClockTime(timeline[i].ts))
        << ",\"text\":" << jstr(timeline[i].text) << "}";
  }
  out << "]";
}

// Recent-incidents entries are rendered from explain_text (see page.h); the
// raw timeline isn't used there and can be large, so it's left out.
void writeIncident(std::ostringstream& out, const Incident& inc) {
  out << "{";
  out << "\"id\":" << inc.id << ",";
  out << "\"start\":" << jstr(formatClockTime(inc.start_ts)) << ",";
  out << "\"end\":" << jstr(formatClockTime(inc.end_ts)) << ",";
  out << "\"closed_via_timeout\":" << (inc.closed_via_timeout ? "true" : "false") << ",";
  out << "\"primary_contributor\":" << jstr(inc.primary_contributor) << ",";
  out << "\"cpu_contribution\":" << jstr(toString(inc.cpu_contribution)) << ",";
  out << "\"thermal_correlation\":" << jstr(toString(inc.thermal_correlation)) << ",";
  out << "\"fan_response\":" << jstr(toString(inc.fan_response)) << ",";
  out << "\"confidence\":" << jstr(toString(inc.confidence)) << ",";
  out << "\"start_cpu_temp\":" << inc.start_cpu_temp << ",";
  out << "\"peak_cpu_temp\":" << inc.peak_cpu_temp << ",";
  out << "\"start_fan_rpm\":" << inc.start_fan_rpm << ",";
  out << "\"peak_fan_rpm\":" << inc.peak_fan_rpm << ",";
  out << "\"explain_text\":" << jstr(buildExplainText(inc));
  out << "}";
}

}  // namespace

std::string buildStateJson(const CorrelationEngine& engine, const std::string& log_path) {
  std::ostringstream out;
  out << "{";

  const Sample* s = engine.latest();
  if (s) {
    out << "\"time\":" << jstr(formatClockTime(s->ts)) << ",";
    out << "\"cpu_pct\":" << s->cpu.total_util_pct << ",";
    out << "\"temp_c\":" << s->thermal.cpu_temp_c << ",";
    out << "\"fan_rpm\":" << s->fan.rpm << ",";
    out << "\"load\":[" << s->cpu.load_avg1 << "," << s->cpu.load_avg5 << ","
        << s->cpu.load_avg15 << "],";
    out << "\"platform_profile\":"
        << (s->platform.available ? jstr(s->platform.profile) : "null") << ",";
    out << "\"throttled\":" << (s->thermal.throttled ? "true" : "false") << ",";
    out << "\"top_processes\":[";
    for (size_t i = 0; i < s->top_processes.size(); ++i) {
      if (i) out << ",";
      const auto& p = s->top_processes[i];
      out << "{\"pid\":" << p.pid << ",\"comm\":" << jstr(p.comm)
          << ",\"cpu_pct\":" << p.cpu_pct << "}";
    }
    out << "],";
  } else {
    out << "\"time\":null,\"cpu_pct\":null,\"temp_c\":null,\"fan_rpm\":null,"
           "\"load\":[0,0,0],\"platform_profile\":null,\"throttled\":false,\"top_processes\":[],";
  }

  if (const Incident* inc = engine.active()) {
    out << "\"active_incident\":{";
    out << "\"id\":" << inc->id << ",";
    out << "\"start\":" << jstr(formatClockTime(inc->start_ts)) << ",";
    out << "\"elapsed_s\":" << (s ? secondsBetween(inc->start_ts, s->ts) : 0) << ",";
    out << "\"trigger\":" << jstr(inc->trigger) << ",";
    out << "\"trigger_process\":" << jstr(inc->trigger_process) << ",";
    out << "\"trigger_pid\":" << inc->trigger_pid << ",";
    out << "\"timeline\":";
    writeTimelineTail(out, inc->timeline);
    out << "},";
  } else {
    out << "\"active_incident\":null,";
  }

  out << "\"nproc\":" << sysconf(_SC_NPROCESSORS_ONLN) << ",";

  out << "\"recent_incidents\":[";
  bool first = true;
  for (const auto& inc : engine.recentClosed()) {
    if (!first) out << ",";
    first = false;
    writeIncident(out, inc);
  }
  out << "],";

  out << "\"log_path\":" << jstr(log_path);
  out << "}";
  return out.str();
}

}  // namespace whyhot
