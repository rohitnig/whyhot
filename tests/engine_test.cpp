// Deterministic test for CorrelationEngine + IncidentStore, driven by
// synthetic samples instead of real /proc//sys data, so it doesn't depend
// on what happens to be running on the machine at test time.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "correlation/engine.h"
#include "correlation/store.h"
#include "ui/terminal.h"

using namespace whyhot;

namespace {

bool g_ok = true;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL: " << #cond << " (" << __FILE__ << ":" << __LINE__  \
                << ")\n";                                                    \
      g_ok = false;                                                          \
    }                                                                        \
  } while (0)

Sample mkSample(Clock::time_point ts, double cpu_util, double temp, int fan_rpm,
                 const std::string& comm, double comm_pct) {
  Sample s;
  s.ts = ts;
  s.cpu.total_util_pct = cpu_util;
  s.thermal.cpu_temp_c = temp;
  s.thermal.gpu_temp_c = -1;
  s.fan.rpm = fan_rpm;
  s.fan.available = true;
  if (!comm.empty()) {
    ProcessInfo p;
    p.pid = 500;
    p.comm = comm;
    p.cmdline = comm;
    p.cpu_pct = comm_pct;
    s.top_processes.push_back(p);
  }
  return s;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Simulates a watcher that gets killed mid-incident: an [incident_start]
// record is written, but nothing ever writes the matching [incident] close
// record. A fresh process (engine2) must then (a) see it as incomplete via
// loadAll(), (b) not reuse its id, and (c) if the first incident's close
// record does eventually show up (e.g. hand-appended here to stand in for
// a run that resolves normally), the close record must win over the
// leftover start record rather than the id staying stuck as incomplete.
void testDurability() {
  const std::string path = "/tmp/whyhot_engine_test_durability.log";
  std::remove(path.c_str());
  IncidentStore store(path);
  const auto base = Clock::now();

  CorrelationEngine engine;
  for (int i = 0; i < 15; ++i) {
    auto s = mkSample(base + std::chrono::seconds(i), 4, 55.0, 1700, "idle", 2);
    engine.addSample(s, {});
  }
  auto spike = mkSample(base + std::chrono::seconds(15), 35, 55.0, 1700, "python", 35);
  auto closed_immediately = engine.addSample(spike, {});
  CHECK(!closed_immediately.has_value());

  const Incident* opened = engine.justOpened();
  CHECK(opened != nullptr);
  if (!opened) return;
  CHECK(opened->trigger == "process_cpu");
  CHECK(opened->trigger_process == "python");
  const int first_id = opened->id;
  store.appendStart(*opened);
  // "Crash" here - engine and process go away without ever finalizing.

  auto loaded = store.loadAll();
  CHECK(loaded.size() == 1);
  if (loaded.size() == 1) {
    CHECK(loaded[0].incomplete);
    CHECK(loaded[0].id == first_id);
    CHECK(loaded[0].trigger_process == "python");
  }
  CHECK(store.maxKnownId() == first_id);

  // Restart: a fresh engine must continue numbering from the log, not
  // restart at 1 (which would collide with the still-open incident above).
  CorrelationEngine engine2;
  engine2.setNextId(store.maxKnownId() + 1);
  for (int i = 0; i < 15; ++i) {
    auto s = mkSample(base + std::chrono::seconds(100 + i), 4, 55.0, 1700, "idle", 2);
    engine2.addSample(s, {});
  }
  auto spike2 = mkSample(base + std::chrono::seconds(115), 40, 55.0, 1700, "ffmpeg", 40);
  engine2.addSample(spike2, {});
  const Incident* opened2 = engine2.justOpened();
  CHECK(opened2 != nullptr);
  if (!opened2) return;
  CHECK(opened2->id != first_id);
  const int second_id = opened2->id;
  store.appendStart(*opened2);

  // Now let the first incident resolve normally.
  Incident manual_close = *opened;
  manual_close.closed = true;
  manual_close.end_ts = base + std::chrono::seconds(50);
  manual_close.primary_contributor = "python";
  manual_close.confidence = Level::kHigh;
  store.append(manual_close);

  auto loaded2 = store.loadAll();
  CHECK(loaded2.size() == 2);
  for (const auto& inc : loaded2) {
    if (inc.id == first_id) {
      CHECK(!inc.incomplete);
      CHECK(inc.primary_contributor == "python");
    } else if (inc.id == second_id) {
      CHECK(inc.incomplete);
      CHECK(inc.trigger_process == "ffmpeg");
    } else {
      CHECK(false);  // unexpected id
    }
  }

  std::remove(path.c_str());
}

// A one-second CLI blip (a process briefly crosses 30% CPU, e.g. `nvidia-smi`
// being invoked) that triggers an incident but leaves temperature and fan
// unmoved must not be explained with a confident "Cause:" - there's nothing
// to blame it on.
void testTrivialIncidentExplain() {
  CorrelationEngine engine;
  const auto base = Clock::now();
  std::vector<Incident> closed;

  for (int i = 0; i < 15; ++i) {
    auto s = mkSample(base + std::chrono::seconds(i), 3, 55.0, 1700, "idle", 2);
    if (auto c = engine.addSample(s, {})) closed.push_back(*c);
  }
  auto blip = mkSample(base + std::chrono::seconds(15), 3, 55.0, 1700, "nvidia-smi", 90);
  if (auto c = engine.addSample(blip, {})) closed.push_back(*c);
  for (int i = 0; i < 25; ++i) {
    auto s = mkSample(base + std::chrono::seconds(16 + i), 3, 55.0, 1700, "idle", 2);
    if (auto c = engine.addSample(s, {})) closed.push_back(*c);
  }

  CHECK(closed.size() == 1);
  if (!closed.empty()) {
    const Incident& inc = closed.front();
    CHECK(!inc.closed_via_timeout);
    CHECK(inc.thermal_correlation == Level::kNone);
    CHECK(inc.fan_response == FanResponse::kMuted);
    const std::string text = buildExplainText(inc);
    CHECK(text.find("No real thermal consequence found") != std::string::npos);
    CHECK(text.find("Cause:") == std::string::npos);
  }
}

// recentClosed() is consumed front-to-back as "most recent first" by both
// UIs, so seeding it from history (ascending by id, as IncidentStore
// returns it) must end up in the same order as incidents closed live would.
void testSeedRecentClosedOrdering() {
  CorrelationEngine engine;
  std::vector<Incident> history;
  for (int id : {5, 6, 7}) {
    Incident inc;
    inc.id = id;
    inc.closed = true;
    inc.primary_contributor = "proc" + std::to_string(id);
    history.push_back(inc);
  }
  Incident incomplete;
  incomplete.id = 8;
  incomplete.closed = false;
  incomplete.incomplete = true;
  history.push_back(incomplete);

  engine.seedRecentClosed(history);

  const auto& closed = engine.recentClosed();
  CHECK(closed.size() == 3);  // the incomplete one must be skipped
  if (closed.size() == 3) {
    CHECK(closed[0].id == 7);  // most recent first
    CHECK(closed[1].id == 6);
    CHECK(closed[2].id == 5);
  }
}

}  // namespace

int main() {
  CorrelationEngine engine;
  const auto base = Clock::now();
  std::vector<Incident> closed;

  // 0..19: quiet baseline.
  for (int i = 0; i < 20; ++i) {
    auto s = mkSample(base + std::chrono::seconds(i), 5, 60.0, 1800, "idle", 2);
    if (auto c = engine.addSample(s, {})) closed.push_back(*c);
  }
  // 20..39: chrome ramps up; temp/fan follow with a short lag.
  for (int i = 0; i < 20; ++i) {
    const double t = i / 19.0;
    const double chrome_pct = lerp(5, 60, t);
    const double temp = lerp(60, 90, std::min(1.0, t + 0.1));
    const int fan = static_cast<int>(lerp(1800, 3800, std::min(1.0, t + 0.15)));
    auto s = mkSample(base + std::chrono::seconds(20 + i), chrome_pct, temp, fan,
                       "chrome", chrome_pct);
    if (auto c = engine.addSample(s, {})) closed.push_back(*c);
  }
  // 40..59: chrome ramps down; temp/fan follow back down.
  for (int i = 0; i < 20; ++i) {
    const double t = i / 19.0;
    const double chrome_pct = lerp(60, 2, t);
    const double temp = lerp(90, 62, std::min(1.0, t + 0.1));
    const int fan = static_cast<int>(lerp(3800, 1900, std::min(1.0, t + 0.15)));
    auto s = mkSample(base + std::chrono::seconds(40 + i), chrome_pct, temp, fan,
                       "chrome", chrome_pct);
    if (auto c = engine.addSample(s, {})) closed.push_back(*c);
  }
  // 60..90: back to quiet, long enough to clear the cooldown window.
  for (int i = 0; i < 31; ++i) {
    auto s = mkSample(base + std::chrono::seconds(60 + i), 4, 61.0, 1850, "idle", 2);
    if (auto c = engine.addSample(s, {})) closed.push_back(*c);
  }

  CHECK(closed.size() == 1);
  if (!closed.empty()) {
    const Incident& inc = closed.front();
    CHECK(inc.closed);
    CHECK(!inc.closed_via_timeout);
    CHECK(inc.primary_contributor == "chrome");
    CHECK(inc.thermal_correlation == Level::kHigh);
    CHECK(inc.fan_response == FanResponse::kNormal);
    CHECK(inc.confidence == Level::kHigh);
    CHECK(!inc.timeline.empty());

    const std::string path = "/tmp/whyhot_engine_test.log";
    std::remove(path.c_str());
    IncidentStore store(path);
    store.append(inc);
    auto loaded = store.loadAll();
    CHECK(loaded.size() == 1);
    if (!loaded.empty()) {
      CHECK(loaded[0].primary_contributor == inc.primary_contributor);
      CHECK(loaded[0].confidence == inc.confidence);
      CHECK(loaded[0].fan_response == inc.fan_response);
      CHECK(loaded[0].timeline.size() == inc.timeline.size());
      std::cout << "--- round-tripped explain output ---\n";
      printExplain(loaded[0]);
    }
    std::remove(path.c_str());
  }

  testDurability();
  testTrivialIncidentExplain();
  testSeedRecentClosedOrdering();

  std::cout << (g_ok ? "PASS\n" : "FAIL\n");
  return g_ok ? 0 : 1;
}
