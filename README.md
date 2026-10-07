# whyhot

A Linux diagnostic tool that answers "why is my fan running?" — not another
resource dashboard. It watches CPU, thermal, and process activity at 1Hz,
and instead of showing you a wall of graphs, it opens an **incident** when
something abnormal happens (a temperature spike, a fan surge, a process
CPU spike) and records what led up to it, what followed, and whether the
fan's behavior is actually explained by load.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires a C++20 compiler and CMake >= 3.16. No other dependencies.

## Usage

```
whyhot watch          # live view; runs until Ctrl-C, records incidents as they close
whyhot list           # one line per recorded incident
whyhot explain [id]   # post-mortem for the most recent (or a specific) incident
```

Incidents are appended to a plain-text, greppable log at
`$XDG_STATE_HOME/whyhot/incidents.log` (or `~/.local/state/whyhot/incidents.log`),
so `list`/`explain` work as ordinary short-lived commands against whatever
`watch` has recorded, without needing a running daemon or IPC.

## How an incident is detected

An incident opens when, within a ~10s window:

- CPU package temperature rises >= 8°C, or
- fan RPM rises >= 800, or
- any single process crosses 30% CPU.

It stays open, recording a timeline of temperature/fan band crossings and
process CPU crossings, until things settle back near their pre-incident
baseline for 20 consecutive seconds (or 30 minutes elapse, in which case
it's flagged as timed out rather than resolved — this is how a fan stuck at
high RPM with no corresponding load gets caught).

At close, it computes a verdict:

- **primary contributor** — the process with the highest average CPU% during
  the incident (must have peaked above 15%)
- **CPU contribution** — how much of the incident's CPU usage that implies
- **thermal correlation** — whether the temperature rise tracked the load,
  or moved independently of it
- **fan response** — NORMAL (rose and fell with load), STUCK (still elevated
  when the 30-minute cap hit), DELAYED, or MUTED
- **confidence** — how clean the correlation is overall

## Scope

v0.1 deliberately covers only `/proc/stat`, `/proc/loadavg`, `/proc/<pid>/stat`,
`/proc/<pid>/cmdline`, `/sys/class/hwmon`, and `/sys/class/thermal`. No GPU,
no power, no eBPF — those are candidates for later versions, once there's a
concrete question these interfaces can't answer.
