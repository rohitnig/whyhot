#include "correlation/store.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace whyhot {

namespace {

long long toEpochSeconds(Clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

Clock::time_point fromEpochSeconds(long long secs) {
  return Clock::time_point(std::chrono::seconds(secs));
}

// Escapes '\n' and '|' so a timeline line survives round-tripping through
// the pipe-delimited "text" field.
std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\n') out += "\\n";
    else if (c == '|') out += "\\|";
    else out += c;
  }
  return out;
}

std::string unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == '|')) {
      out += s[i + 1] == 'n' ? '\n' : '|';
      ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::ofstream openForAppend(const std::string& path) {
  std::filesystem::path p(path);
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  return std::ofstream(path, std::ios::app);
}

}  // namespace

IncidentStore::IncidentStore(std::string path) : path_(std::move(path)) {}

std::string IncidentStore::defaultPath() {
  if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
    return std::string(xdg) + "/whyhot/incidents.log";
  }
  if (const char* home = std::getenv("HOME")) {
    return std::string(home) + "/.local/state/whyhot/incidents.log";
  }
  return "./whyhot-incidents.log";
}

void IncidentStore::appendStart(const Incident& incident) const {
  std::ofstream f = openForAppend(path_);
  if (!f) return;

  f << "[incident_start]\n";
  f << "id=" << incident.id << "\n";
  f << "start=" << toEpochSeconds(incident.start_ts) << "\n";
  f << "trigger=" << incident.trigger << "\n";
  f << "trigger_process=" << incident.trigger_process << "\n";
  f << "trigger_pid=" << incident.trigger_pid << "\n";
  f << "baseline_cpu_temp=" << incident.baseline_cpu_temp << "\n";
  f << "baseline_fan_rpm=" << incident.baseline_fan_rpm << "\n";
  f << "baseline_cpu_util=" << incident.baseline_cpu_util << "\n";
  f << "[end]\n";
}

void IncidentStore::append(const Incident& incident) const {
  std::ofstream f = openForAppend(path_);
  if (!f) return;

  f << "[incident]\n";
  f << "id=" << incident.id << "\n";
  f << "start=" << toEpochSeconds(incident.start_ts) << "\n";
  f << "end=" << toEpochSeconds(incident.end_ts) << "\n";
  f << "closed_via_timeout=" << (incident.closed_via_timeout ? 1 : 0) << "\n";
  f << "trigger=" << incident.trigger << "\n";
  f << "trigger_process=" << incident.trigger_process << "\n";
  f << "trigger_pid=" << incident.trigger_pid << "\n";
  f << "baseline_cpu_temp=" << incident.baseline_cpu_temp << "\n";
  f << "baseline_fan_rpm=" << incident.baseline_fan_rpm << "\n";
  f << "baseline_cpu_util=" << incident.baseline_cpu_util << "\n";
  f << "start_cpu_temp=" << incident.start_cpu_temp << "\n";
  f << "peak_cpu_temp=" << incident.peak_cpu_temp << "\n";
  f << "start_fan_rpm=" << incident.start_fan_rpm << "\n";
  f << "peak_fan_rpm=" << incident.peak_fan_rpm << "\n";
  f << "primary_contributor=" << incident.primary_contributor << "\n";
  f << "contributor_peak_cpu_pct=" << incident.contributor_peak_cpu_pct << "\n";
  f << "cpu_contribution=" << toString(incident.cpu_contribution) << "\n";
  f << "thermal_correlation=" << toString(incident.thermal_correlation) << "\n";
  f << "fan_response=" << toString(incident.fan_response) << "\n";
  f << "confidence=" << toString(incident.confidence) << "\n";
  f << "[timeline]\n";
  for (const auto& e : incident.timeline) {
    f << toEpochSeconds(e.ts) << "|" << escape(e.text) << "\n";
  }
  f << "[end]\n";
}

namespace {

// Parses the whole log into per-id start records and per-id end records.
// Both record kinds share the same key=value vocabulary (a start record
// just doesn't have most of the keys, or a [timeline] section), so one
// parse loop handles both.
void parseLog(const std::string& path, std::map<int, Incident>* starts,
              std::map<int, Incident>* ends) {
  std::ifstream f(path);
  if (!f) return;

  std::string line;
  Incident cur;
  bool in_timeline = false;
  bool in_record = false;
  bool cur_is_start = false;

  while (std::getline(f, line)) {
    if (line == "[incident_start]" || line == "[incident]") {
      cur = Incident{};
      in_record = true;
      in_timeline = false;
      cur_is_start = line == "[incident_start]";
      continue;
    }
    if (line == "[timeline]") {
      in_timeline = true;
      continue;
    }
    if (line == "[end]") {
      if (in_record) {
        if (cur_is_start) {
          cur.incomplete = true;
          (*starts)[cur.id] = cur;
        } else {
          cur.closed = true;
          (*ends)[cur.id] = cur;
        }
      }
      in_record = false;
      in_timeline = false;
      continue;
    }
    if (!in_record) continue;

    if (in_timeline) {
      const auto sep = line.find('|');
      if (sep == std::string::npos) continue;
      TimelineEntry e;
      e.ts = fromEpochSeconds(std::stoll(line.substr(0, sep)));
      e.text = unescape(line.substr(sep + 1));
      cur.timeline.push_back(e);
      continue;
    }

    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string val = line.substr(eq + 1);

    if (key == "id") cur.id = std::stoi(val);
    else if (key == "start") cur.start_ts = fromEpochSeconds(std::stoll(val));
    else if (key == "end") cur.end_ts = fromEpochSeconds(std::stoll(val));
    else if (key == "closed_via_timeout") cur.closed_via_timeout = val == "1";
    else if (key == "trigger") cur.trigger = val;
    else if (key == "trigger_process") cur.trigger_process = val;
    else if (key == "trigger_pid") cur.trigger_pid = std::stoi(val);
    else if (key == "baseline_cpu_temp") cur.baseline_cpu_temp = std::stod(val);
    else if (key == "baseline_fan_rpm") cur.baseline_fan_rpm = std::stod(val);
    else if (key == "baseline_cpu_util") cur.baseline_cpu_util = std::stod(val);
    else if (key == "start_cpu_temp") cur.start_cpu_temp = std::stod(val);
    else if (key == "peak_cpu_temp") cur.peak_cpu_temp = std::stod(val);
    else if (key == "start_fan_rpm") cur.start_fan_rpm = std::stod(val);
    else if (key == "peak_fan_rpm") cur.peak_fan_rpm = std::stod(val);
    else if (key == "primary_contributor") cur.primary_contributor = val;
    else if (key == "contributor_peak_cpu_pct") cur.contributor_peak_cpu_pct = std::stod(val);
    else if (key == "cpu_contribution") cur.cpu_contribution = levelFromString(val);
    else if (key == "thermal_correlation") cur.thermal_correlation = levelFromString(val);
    else if (key == "fan_response") cur.fan_response = fanResponseFromString(val);
    else if (key == "confidence") cur.confidence = levelFromString(val);
  }
}

}  // namespace

std::vector<Incident> IncidentStore::loadAll() const {
  std::map<int, Incident> starts, ends;
  parseLog(path_, &starts, &ends);

  std::vector<Incident> result;
  result.reserve(starts.size() + ends.size());
  for (auto& [id, inc] : ends) result.push_back(inc);
  for (auto& [id, inc] : starts) {
    if (ends.find(id) == ends.end()) result.push_back(inc);
  }
  std::sort(result.begin(), result.end(),
            [](const Incident& a, const Incident& b) { return a.id < b.id; });
  return result;
}

int IncidentStore::maxKnownId() const {
  std::map<int, Incident> starts, ends;
  parseLog(path_, &starts, &ends);
  int max_id = 0;
  for (auto& [id, inc] : starts) max_id = std::max(max_id, id);
  for (auto& [id, inc] : ends) max_id = std::max(max_id, id);
  return max_id;
}

}  // namespace whyhot
