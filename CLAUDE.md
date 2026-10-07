# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`whyhot` is a Linux C++20 diagnostic tool answering "why is my fan running?". It samples CPU, thermal, fan and process data at 1Hz and, instead of showing graphs, opens an **incident** on abnormal activity and records a verdict explaining whether the fan/temperature behavior is justified by load. No dependencies beyond a C++20 compiler, CMake >= 3.16 and pthreads. v0.1 reads only `/proc` and `/sys` (hwmon, thermal, `platform_profile`) — no GPU, power or eBPF.

## Commands

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # or run ./build/engine_test directly

./build/whyhot watch            # live terminal UI (default command)
./build/whyhot serve --port N   # web UI on 127.0.0.1:N (default 7676); not in README
./build/whyhot-desktop         # native always-on-top companion window (Qt6)
./scripts/install-desktop.sh    # installs launcher + icons to ~/.local/share so it can be pinned to the dock
./build/whyhot list
./build/whyhot explain [id]
```

There is one test binary (`engine_test`, a hand-rolled `CHECK` macro, no framework), so there is no per-test filter; add cases as functions called from `main()` in `tests/engine_test.cpp`. No linter is configured; the build uses `-Wall -Wextra -Wpedantic`.

Source files are listed explicitly in `CMakeLists.txt` (no globbing) — add new `.cpp` files there, for both the `whyhot` and (if the test needs them) `engine_test` targets. `engine_test` deliberately does not link the collectors or web code.

## Architecture

Data flow is a single loop in `runSession()` (`src/main.cpp`), shared by `watch` (TUI) and `serve` (web UI); only one UI is active per run:

1. **Collectors** (`src/collectors/`: cpu, process, thermal, fan, platform) each have a `sample()` that reads `/proc`/`/sys`. Their results are bundled into a `Sample`. `ProcessCollector::sample()` also returns `ProcessEvent`s alongside the top-N processes.
2. **`CorrelationEngine`** (`src/correlation/engine.*`) takes each `Sample`, keeps a 30-minute history window, and runs the incident state machine: trigger check → active incident (timeline of band crossings) → quiet-streak cooldown → `finalizeIncident` computes the verdict (primary contributor, CPU contribution, thermal correlation, fan response, confidence). Closes after cooldown, or after a 30-minute cap flagged `closed_via_timeout` (this is how a "stuck fan" is detected). Thresholds are plain constants in `engine.cpp`; the README documents the intended rules.
3. **`IncidentStore`** (`src/correlation/store.*`) appends to a plain-text log at `$XDG_STATE_HOME/whyhot/incidents.log` (fallback `~/.local/state/whyhot/`). `list`/`explain` are separate short-lived processes that read this log — there is no daemon or IPC.
4. **Desktop app** (`src/desktop/main.cpp`): a single-file Qt6 Widgets window, custom-painted with QPainter (frameless, translucent, `WindowStaysOnTopHint` for pin; `main()` forces `QT_QPA_PLATFORM=xcb` because GNOME/Wayland ignores always-on-top for native windows). Hidden dev flag `--snapshot FILE [--collapsed|--expanded] [--temp N]` renders one frame to a PNG without touching the log or prefs. It runs its own collector → engine → store loop in-process and writes the same incident log, so don't run it alongside `whyhot watch`. Target is skipped by CMake if Qt6 Widgets isn't found. Prefs are stored via `QSettings("whyhot","desktop")`.
5. **UIs**: `src/ui/terminal.*` (live view plus the `printList`/`printExplain` output used by CLI commands) and `src/web/` (`server` serves a snapshot; `json.cpp` builds it via `buildStateJson`; `page.h` is the embedded HTML). The main loop pushes a fresh snapshot to the server each tick.

### Things that span files

- Each incident writes **two** records: `[incident_start]` immediately on open (`justOpened()` → `appendStart`) and a full `[incident]` on close. `loadAll()` reconciles them; an id with a start but no close is returned with `Incident::incomplete = true` (watcher crashed/killed). Keep both writers and the parser in `store.cpp` in sync when changing the `Incident` fields or log format.
- On startup `main.cpp` calls `engine.setNextId(store.maxKnownId() + 1)` so ids never collide with incomplete ones from earlier runs, and `engine.seedRecentClosed(history)` so the live view isn't empty.
- Enums (`Level`, `FanResponse`) round-trip through `toString`/`...FromString` for the log format.
- The engine tests feed synthetic `Sample`s with fabricated timestamps, so engine logic must stay driven by `Sample::ts`, not wall-clock time.
