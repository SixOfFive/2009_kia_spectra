// vroom dashboard — polls /api/state every POLL_MS, updates DOM, dispatches button commands.

const POLL_MS = window.POLL_MS || 1000;

const els = {
  voltage:  document.getElementById('v-battery'),
  rpm:      document.getElementById('v-rpm'),
  speed:    document.getElementById('v-speed'),
  coolant:  document.getElementById('v-coolant'),
  throttle: document.getElementById('v-throttle'),
  fuel:     document.getElementById('v-fuel'),

  pillEngine: document.getElementById('pill-engine'),
  pillEsp32:  document.getElementById('pill-esp32'),
  pillWifi:   document.getElementById('pill-wifi'),

  lastUpdate: document.getElementById('last-update'),
  uptime:     document.getElementById('uptime'),

  eventsList: document.getElementById('events-list'),
  tripsBody:  document.getElementById('trips-body'),

  btnStart: document.getElementById('btn-start'),
  btnStop:  document.getElementById('btn-stop'),
  btnPing:  document.getElementById('btn-ping'),
};

// Event display classification — these get extra color in the events list
const EVENT_CLASS = {
  engine_started:       'event-good',
  engine_stopped:       'event-good',
  compustar_tx:         'event-good',
  pi_boot:              'event-good',
  low_voltage_trigger:  'event-error',
  compustar_tx_fail:    'event-error',
};

// ----- Rendering helpers -----

function fmt(n, decimals) {
  if (n === null || n === undefined || isNaN(n)) return '--';
  return Number(n).toFixed(decimals || 0);
}

function fmtUptime(seconds) {
  if (!seconds || seconds < 0) return '--';
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return `${h}h ${m}m`;
}

function render(state) {
  els.voltage.textContent  = fmt(state.v_battery, 1);
  els.rpm.textContent      = fmt(state.last_obd?.rpm, 0);
  els.speed.textContent    = fmt(state.last_obd?.speed, 0);
  els.coolant.textContent  = fmt(state.last_obd?.coolant_temp, 0);
  els.throttle.textContent = fmt(state.last_obd?.throttle_position, 0);
  els.fuel.textContent     = fmt(state.last_obd?.fuel_level, 0);

  // Engine pill
  if (state.engine_running) {
    els.pillEngine.textContent = 'Engine: running';
    els.pillEngine.className = 'pill pill-on';
  } else {
    els.pillEngine.textContent = 'Engine: off';
    els.pillEngine.className = 'pill pill-off';
  }

  // ESP32 pill
  els.pillEsp32.textContent = `ESP32: ${state.esp32_state || 'unknown'}`;

  // WiFi pill
  if (state.wifi_rssi !== null && state.wifi_rssi !== undefined) {
    els.pillWifi.textContent = `WiFi: ${state.wifi_rssi} dBm`;
  } else {
    els.pillWifi.textContent = 'WiFi: -';
  }

  els.lastUpdate.textContent = `Updated: ${new Date().toLocaleTimeString()}`;
  els.uptime.textContent     = `Uptime: ${fmtUptime(state.uptime_s)}`;

  renderEvents(state.events || []);
}

function renderEvents(events) {
  if (!events.length) {
    els.eventsList.innerHTML = '<li class="events-empty">No events yet</li>';
    return;
  }
  // Newest first, cap at 8 visible
  const recent = events.slice(-8).reverse();
  els.eventsList.innerHTML = recent.map(e => {
    const cls = EVENT_CLASS[e.event] || '';
    const t = e.ts ? new Date(e.ts).toLocaleTimeString() : '';
    const meta = e.detail && Object.keys(e.detail).length
        ? ` <span class="event-meta">${formatDetail(e.detail)}</span>` : '';
    return `<li class="${cls}"><span class="event-name">${e.event || '?'}</span>${meta}<span class="event-meta">${t}</span></li>`;
  }).join('');
}

function formatDetail(detail) {
  // Compact one-line render: key=value pairs, truncated
  return Object.entries(detail)
    .map(([k, v]) => `${k}=${typeof v === 'number' ? v : String(v).slice(0, 12)}`)
    .join(' ');
}

// ----- Polling -----

async function poll() {
  try {
    const res = await fetch('/api/state');
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const state = await res.json();
    render(state);
  } catch (err) {
    els.lastUpdate.textContent = `Updated: error (${err.message})`;
  }
}

setInterval(poll, POLL_MS);
poll();  // initial fire

// ----- Trips panel (polled less often — append-only file) -----

function fmtDuration(s) {
  if (s === null || s === undefined) return '--';
  const m = Math.floor(s / 60);
  const r = s % 60;
  return `${m}m${String(r).padStart(2, '0')}`;
}

function fmtIso(iso) {
  if (!iso) return '--';
  try {
    const d = new Date(iso);
    return d.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
  } catch (e) {
    return iso;
  }
}

function renderTrips(payload) {
  const trips = payload.trips || [];
  if (!els.tripsBody) return;
  if (!trips.length) {
    els.tripsBody.innerHTML = '<tr><td class="trips-empty" colspan="7">No trips yet</td></tr>';
    return;
  }
  els.tripsBody.innerHTML = trips.slice(0, 10).map(t => {
    const peak = t.peak_obd || {};
    return `<tr>
      <td>${fmtIso(t.ts_start)}</td>
      <td>${fmtDuration(t.duration_s)}</td>
      <td>${t.trigger_source || '--'}</td>
      <td>${fmt(t.v_start, 1)}</td>
      <td>${fmt(t.v_end, 1)}</td>
      <td>${fmt(peak.rpm, 0)}</td>
      <td>${fmt(peak.coolant_temp, 0)}</td>
    </tr>`;
  }).join('');
}

async function pollTrips() {
  try {
    const res = await fetch('/api/trips?limit=10');
    if (!res.ok) return;
    const body = await res.json();
    renderTrips(body);
  } catch (err) {
    // Silent — the panel just stays stale.
  }
}

// Trips don't update every second; once a minute is plenty since rows
// only land at engine_stopped.
setInterval(pollTrips, 30_000);
pollTrips();

// ----- Buttons -----

function showToast(msg, kind) {
  // kind = 'good' | 'bad' | 'info'
  const t = document.createElement('div');
  t.className = 'toast toast-' + (kind || 'info');
  t.textContent = msg;
  document.body.appendChild(t);
  // Trigger CSS fade-in
  requestAnimationFrame(() => t.classList.add('toast-show'));
  setTimeout(() => {
    t.classList.remove('toast-show');
    setTimeout(() => t.remove(), 300);
  }, 2500);
}

async function sendCommand(cmd) {
  try {
    const res = await fetch('/api/command', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({cmd}),
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok) {
      const reason = body.error || ('HTTP ' + res.status);
      showToast(`${cmd}: ${reason}`, 'bad');
      return;
    }
    showToast(`${cmd} sent`, 'good');
    poll();
  } catch (err) {
    showToast(`${cmd} failed: ${err.message}`, 'bad');
  }
}

els.btnStart.addEventListener('click', () => {
  if (confirm('Start the engine?')) sendCommand('start_engine');
});
els.btnStop.addEventListener('click', () => {
  if (confirm('Stop the engine?')) sendCommand('stop_engine');
});
els.btnPing.addEventListener('click', () => sendCommand('ping'));

// ----- Manual transmit buttons (start/lock/unlock/trunk) -----

async function sendTransmit(button) {
  try {
    const res = await fetch(`/api/transmit/${button}`, {method: 'POST'});
    const body = await res.json().catch(() => ({}));
    if (res.status === 501) {
      showToast(`${button}: not yet wired on ESP32`, 'info');
      return;
    }
    if (!res.ok) {
      const reason = body.error || body.detail || ('HTTP ' + res.status);
      showToast(`${button}: ${reason}`, 'bad');
      return;
    }
    showToast(`${button} sent`, 'good');
    poll();
  } catch (err) {
    showToast(`${button} failed: ${err.message}`, 'bad');
  }
}

document.querySelectorAll('button[data-tx]').forEach(btn => {
  btn.addEventListener('click', () => {
    const which = btn.dataset.tx;
    // Start has destructive potential; the other buttons are quick taps.
    if (which === 'start' && !confirm('Transmit start?')) return;
    sendTransmit(which);
  });
});
