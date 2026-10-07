#pragma once

namespace whyhot {

// The entire web UI: one self-contained HTML file, no external requests
// (no CDN fonts/scripts/libraries), so it works offline and stays light.
// Polls /api/state on an interval; never redraws continuously or animates.
inline constexpr const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>whyhot</title>
<style>
  :root {
    --bg: #0e1116;
    --panel: #161b22;
    --border: #2a313c;
    --text: #d7dee6;
    --muted: #7d8998;
    --accent: #ff9d4d;
    --ok: #d0d7de;
    --good: #5fd97a;
    --warn: #ff9d4d;
    --bad: #ff5d5d;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font: 14px/1.5 ui-monospace, "SF Mono", Menlo, Consolas, "Liberation Mono", monospace;
    padding: 20px;
  }
  .wrap { max-width: 900px; margin: 0 auto; }
  header {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    border-bottom: 1px solid var(--border);
    padding-bottom: 10px;
    margin-bottom: 16px;
  }
  header h1 { font-size: 16px; letter-spacing: 0.06em; margin: 0; color: var(--text); }
  header h1 span { color: var(--accent); }
  #clock { color: var(--muted); }
  .panel {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 14px 16px;
    margin-bottom: 14px;
  }
  .stats-row { display: flex; gap: 28px; flex-wrap: wrap; }
  .stat .label { color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.05em; }
  .stat .value { font-size: 22px; margin-top: 2px; color: var(--text); }
  .stat .value.good { color: var(--good); }
  .stat .value.warn { color: var(--warn); }
  .stat .value.bad { color: var(--bad); }
  .thresholds { color: var(--muted); font-size: 11px; margin-top: 10px; }
  table { width: 100%; border-collapse: collapse; margin-top: 6px; }
  th, td { text-align: left; padding: 3px 6px; font-size: 13px; }
  th { color: var(--muted); font-weight: normal; font-size: 11px; text-transform: uppercase; }
  td.num, th.num { text-align: right; }
  tr.proc-flagged td { color: var(--bad); }
  tr.proc-elevated td { color: var(--warn); }
  .panel h2 {
    font-size: 12px; text-transform: uppercase; letter-spacing: 0.05em;
    color: var(--muted); margin: 0 0 10px 0;
  }
  .incident-active { border-color: var(--accent); }
  .incident-active h2 { color: var(--accent); }
  .timeline-line { padding: 2px 0; font-size: 13px; }
  .timeline-line .t { color: var(--muted); margin-right: 8px; }
  .muted { color: var(--muted); }
  .inc-row {
    display: flex; gap: 12px; align-items: baseline; padding: 6px 0;
    border-top: 1px solid var(--border); cursor: pointer;
  }
  .inc-row:first-child { border-top: none; }
  .inc-row .id { color: var(--muted); width: 34px; }
  .inc-row .who { flex: 1; }
  .badge {
    font-size: 11px; padding: 1px 6px; border-radius: 4px; border: 1px solid var(--border);
  }
  .badge.HIGH { color: var(--accent); border-color: var(--accent); }
  .badge.STUCK { color: var(--bad); border-color: var(--bad); }
  .badge.NORMAL { color: var(--ok); }
  pre.explain {
    white-space: pre-wrap; background: #0b0e13; border: 1px solid var(--border);
    border-radius: 4px; padding: 10px 12px; margin-top: 8px; font-size: 12.5px;
  }
  footer { color: var(--muted); font-size: 11px; margin-top: 10px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>WHY<span>HOT</span></h1>
    <div id="clock">--:--:--</div>
  </header>

  <div class="panel">
    <div class="stats-row">
      <div class="stat"><div class="label">CPU</div><div class="value" id="s-cpu">--%</div></div>
      <div class="stat"><div class="label">Temp</div><div class="value" id="s-temp">--&deg;C</div></div>
      <div class="stat"><div class="label">Fan</div><div class="value" id="s-fan">-- RPM</div></div>
      <div class="stat"><div class="label">Load</div><div class="value" id="s-load">-- -- --</div></div>
      <div class="stat"><div class="label">Profile</div><div class="value" id="s-profile">--</div></div>
    </div>
    <table>
      <thead><tr><th>Process</th><th class="num">PID</th><th class="num">CPU</th></tr></thead>
      <tbody id="proc-body"></tbody>
    </table>
    <div class="thresholds">
      Colors: CPU/Temp/Load - green/amber/red are fixed thresholds (CPU 20/50%, temp
      70&deg;C/throttle, load 0.7&times;/1.5&times; core count). Fan compares to the lowest RPM seen
      this session, since "normal" varies by machine. Process rows: amber = above the 30% CPU
      incident trigger, red = the process that actually triggered the active incident.
    </div>
  </div>

  <div class="panel" id="incident-panel" style="display:none">
    <h2 id="incident-title"></h2>
    <div id="incident-timeline"></div>
    <div class="muted" style="margin-top:8px">Watching for return to baseline...</div>
  </div>
  <div class="panel" id="no-incident-panel">
    <h2>Status</h2>
    <div class="muted">No active incident. Monitoring.</div>
  </div>

  <div class="panel">
    <h2>Recent incidents</h2>
    <div id="recent-list" class="muted">(none yet this session)</div>
  </div>

  <footer id="log-path"></footer>
</div>

<script>
const expanded = new Set();
let fanMinSeen = null;

function badge(text) {
  return '<span class="badge ' + text + '">' + text + '</span>';
}

// Ascending metric (higher = worse), e.g. CPU% or temperature.
function levelAsc(value, warnAt, badAt) {
  if (value == null) return '';
  if (value >= badAt) return 'bad';
  if (value >= warnAt) return 'warn';
  return 'good';
}

function setStat(id, text, level) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = 'value' + (level ? ' ' + level : '');
}

function render(state) {
  document.getElementById('clock').textContent = state.time || '--:--:--';

  setStat('s-cpu', (state.cpu_pct == null ? '--' : state.cpu_pct.toFixed(1)) + '%',
          levelAsc(state.cpu_pct, 20, 50));

  const tempLevel = state.throttled ? 'bad' : levelAsc(state.temp_c, 70, 90);
  setStat('s-temp', (state.temp_c == null ? '--' : state.temp_c.toFixed(1)) + '°C', tempLevel);

  // "Normal" fan RPM varies a lot by machine, so compare against the
  // lowest this session has actually seen instead of a guessed constant.
  let fanLevel = '';
  if (state.fan_rpm != null) {
    fanMinSeen = fanMinSeen == null ? state.fan_rpm : Math.min(fanMinSeen, state.fan_rpm);
    fanLevel = levelAsc(state.fan_rpm - fanMinSeen, 500, 1500);
  }
  setStat('s-fan', (state.fan_rpm == null ? '--' : state.fan_rpm) + ' RPM', fanLevel);

  const loadRatio = state.nproc ? state.load[0] / state.nproc : null;
  setStat('s-load', state.load.map(x => x.toFixed(2)).join(' '), levelAsc(loadRatio, 0.7, 1.5));

  document.getElementById('s-profile').textContent = state.platform_profile || 'n/a';

  const triggerPid = state.active_incident ? state.active_incident.trigger_pid : null;
  const procBody = document.getElementById('proc-body');
  procBody.innerHTML = state.top_processes.map(p => {
    const rowClass = p.pid === triggerPid ? 'proc-flagged' : p.cpu_pct >= 30 ? 'proc-elevated' : '';
    return '<tr class="' + rowClass + '"><td>' + p.comm + '</td><td class="num">' + p.pid +
           '</td><td class="num">' + p.cpu_pct.toFixed(1) + '%</td></tr>';
  }).join('');

  const incPanel = document.getElementById('incident-panel');
  const noIncPanel = document.getElementById('no-incident-panel');
  if (state.active_incident) {
    incPanel.style.display = '';
    noIncPanel.style.display = 'none';
    incPanel.classList.add('incident-active');
    const inc = state.active_incident;
    document.getElementById('incident-title').textContent =
      'INCIDENT #' + inc.id + '  (started ' + inc.start + ', ' + inc.elapsed_s + 's elapsed)';
    document.getElementById('incident-timeline').innerHTML = inc.timeline.slice(-10).map(e =>
      '<div class="timeline-line"><span class="t">' + e.time + '</span>' + e.text + '</div>'
    ).join('');
  } else {
    incPanel.style.display = 'none';
    noIncPanel.style.display = '';
  }

  const list = document.getElementById('recent-list');
  if (state.recent_incidents.length === 0) {
    list.innerHTML = '(none yet this session)';
  } else {
    list.innerHTML = state.recent_incidents.map(inc => {
      const open = expanded.has(inc.id);
      return '<div>' +
        '<div class="inc-row" data-id="' + inc.id + '">' +
          '<span class="id">#' + inc.id + '</span>' +
          '<span class="who">' + inc.start + '&ndash;' + inc.end + '  ' + inc.primary_contributor + '</span>' +
          badge(inc.confidence) + badge(inc.fan_response) +
        '</div>' +
        (open ? '<pre class="explain">' + inc.explain_text + '</pre>' : '') +
      '</div>';
    }).join('');
    list.querySelectorAll('.inc-row').forEach(row => {
      row.addEventListener('click', () => {
        const id = parseInt(row.dataset.id, 10);
        if (expanded.has(id)) expanded.delete(id); else expanded.add(id);
        render(state);
      });
    });
  }

  document.getElementById('log-path').textContent = 'Log: ' + state.log_path;
}

async function poll() {
  try {
    const res = await fetch('/api/state', { cache: 'no-store' });
    const state = await res.json();
    render(state);
  } catch (e) {
    // Server likely restarting; just try again next tick.
  }
}

poll();
setInterval(poll, 2000);
</script>
</body>
</html>
)HTML";

}  // namespace whyhot
