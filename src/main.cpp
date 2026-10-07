#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "collectors/cpu.h"
#include "collectors/fan.h"
#include "collectors/platform.h"
#include "collectors/process.h"
#include "collectors/thermal.h"
#include "correlation/engine.h"
#include "correlation/store.h"
#include "ui/terminal.h"
#include "web/json.h"
#include "web/server.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handleSignal(int) { g_stop = 1; }

// Shared by `whyhot watch` (terminal UI) and `whyhot serve` (web UI, via
// `web_port`); at most one of the two UIs is active per run.
int runSession(bool with_tui, int web_port) {
  using namespace whyhot;

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  CpuCollector cpu;
  ProcessCollector process(5);
  ThermalCollector thermal;
  FanCollector fan;
  PlatformCollector platform;
  CorrelationEngine engine;
  const std::string log_path = IncidentStore::defaultPath();
  const IncidentStore store(log_path);

  // Continue numbering from the log rather than restarting at 1, so a
  // fresh run's ids can't collide with ones from a previous run that are
  // still sitting there marked incomplete.
  engine.setNextId(store.maxKnownId() + 1);

  const std::vector<Incident> history = store.loadAll();

  int incomplete_count = 0;
  for (const auto& inc : history) {
    if (!inc.incomplete) continue;
    ++incomplete_count;
    std::cout << "Found incomplete incident #" << inc.id << " (started "
              << formatClockTime(inc.start_ts)
              << ") - the watcher was interrupted before it settled. Run `whyhot explain "
              << inc.id << "` for details.\n";
  }
  if (incomplete_count > 0) std::cout << "\n";

  // So the live view's "recent incidents" panel isn't misleadingly empty
  // just because nothing has closed yet during this particular run.
  engine.seedRecentClosed(history);

  TerminalUi ui;
  if (with_tui) ui.enterAltScreen();

  std::unique_ptr<WebServer> server;
  if (web_port >= 0) {
    server = std::make_unique<WebServer>(web_port);
    if (!server->start()) {
      std::cerr << "Could not bind 127.0.0.1:" << web_port << " - is something else using it?\n";
      return 1;
    }
    std::cout << "whyhot serving on http://127.0.0.1:" << web_port << " (Ctrl-C to stop)\n";
  }

  while (!g_stop) {
    Sample sample;
    sample.ts = Clock::now();
    sample.cpu = cpu.sample();
    sample.thermal = thermal.sample();
    sample.fan = fan.sample();
    sample.platform = platform.sample();
    auto proc_result = process.sample();
    sample.top_processes = proc_result.top;

    if (auto closed = engine.addSample(sample, proc_result.events)) {
      store.append(*closed);
    }
    if (const Incident* opened = engine.justOpened()) {
      store.appendStart(*opened);
    }

    if (with_tui) ui.renderLive(engine, log_path);
    if (server) server->updateSnapshot(buildStateJson(engine, log_path));

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (with_tui) ui.leaveAltScreen();
  return 0;
}

int runExplain(int argc, char** argv) {
  using namespace whyhot;
  const IncidentStore store(IncidentStore::defaultPath());
  auto incidents = store.loadAll();
  if (incidents.empty()) {
    std::cout << "No incidents recorded yet. Run `whyhot watch` and wait for one.\n";
    return 1;
  }

  const Incident* target = &incidents.back();
  if (argc > 2) {
    const int id = std::atoi(argv[2]);
    bool found = false;
    for (const auto& inc : incidents) {
      if (inc.id == id) {
        target = &inc;
        found = true;
        break;
      }
    }
    if (!found) {
      std::cerr << "No incident with id " << id << " found.\n";
      return 1;
    }
  }

  printExplain(*target);
  return 0;
}

int runList() {
  using namespace whyhot;
  const IncidentStore store(IncidentStore::defaultPath());
  printList(store.loadAll());
  return 0;
}

void printUsage() {
  std::cout << "whyhot v0.1 - why is my fan running?\n\n"
            << "Usage:\n"
            << "  whyhot [watch]        Live terminal view; records incidents as they happen\n"
            << "  whyhot serve [--port N]  Same, served as a web UI at http://127.0.0.1:N (default 7676)\n"
            << "  whyhot list           List recorded incidents\n"
            << "  whyhot explain [id]   Explain the most recent (or given) incident\n";
}

int parsePort(int argc, char** argv, int fallback) {
  for (int i = 2; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--port") return std::atoi(argv[i + 1]);
  }
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "watch";

  if (cmd == "watch") return runSession(/*with_tui=*/true, /*web_port=*/-1);
  if (cmd == "serve") return runSession(/*with_tui=*/false, parsePort(argc, argv, 7676));
  if (cmd == "explain") return runExplain(argc, argv);
  if (cmd == "list") return runList();
  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    printUsage();
    return 0;
  }

  std::cerr << "Unknown command: " << cmd << "\n\n";
  printUsage();
  return 1;
}
