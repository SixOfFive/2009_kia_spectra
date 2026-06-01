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
  renderSparklines(state.obd_history || {});
}

// ----- Sparklines (pure SVG, no library) -----

// Cache the SVG elements once; the dashboard layout is static.
const _sparkEls = Array.from(document.querySelectorAll('svg[data-spark]'));

function renderSparklines(history) {
  for (const svg of _sparkEls) {
    const pid = svg.dataset.spark;
    const samples = history[pid] || [];
    svg.innerHTML = sparkSvgInner(samples, svg);
  }
}

function sparkSvgInner(samples, svg) {
  // viewBox is "0 0 W H"; pull width/height from the attribute so each
  // tile can size independently (gauge-large has a taller sparkline).
  const vb = (svg.getAttribute('viewBox') || '0 0 60 14').split(/\s+/).map(Number);
  const W = vb[2] || 60;
  const H = vb[3] || 14;

  if (!samples.length) {
    // Dotted baseline placeholder so the tile doesn't visually collapse.
    return `<line class="spark-empty" x1="0" y1="${H/2}" x2="${W}" y2="${H/2}"/>`;
  }

  const values = samples.map(s => s[1]);
  let lo = Math.min(...values);
  let hi = Math.max(...values);
  // Flat data: pad the range so the line draws mid-tile rather than
  // jumping to an edge after the first divide-by-zero would otherwise.
  if (hi - lo < 1e-9) {
    const pad = Math.abs(hi) > 1 ? Math.abs(hi) * 0.05 : 1;
    lo -= pad;
    hi += pad;
  }
  const n = samples.length;
  const xStep = n > 1 ? W / (n - 1) : 0;
  const pts = samples.map((s, i) => {
    const x = i * xStep;
    // Invert Y because SVG (0,0) is top-left.
    const y = H - ((s[1] - lo) / (hi - lo)) * H;
    return `${x.toFixed(1)},${y.toFixed(2)}`;
  });
  const lastX = (n - 1) * xStep;
  const lastY = H - ((values[n - 1] - lo) / (hi - lo)) * H;

  return (
    `<polyline class="spark-line" points="${pts.join(' ')}"/>` +
    `<circle class="spark-dot" cx="${lastX.toFixed(1)}" cy="${lastY.toFixed(2)}" r="1.4"/>`
  );
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


// ----- Map view + toggle -----
//
// The dashboard ships with two views — gauges (default) and an
// embedded Leaflet map. A floating button in the top-right corner
// toggles between them. Leaflet itself is lazy-init'd so a kiosk
// that never opens the map never pays the tile-fetch cost.

let _mapInstance = null;
const TOGGLE_STORAGE_KEY = 'vroom.view';

function initMapIfNeeded() {
  if (_mapInstance !== null) return;
  if (typeof L === 'undefined') {
    // Leaflet hasn't finished loading yet — retry on next animation
    // frame. Cheap and avoids holding up the toggle UI.
    requestAnimationFrame(initMapIfNeeded);
    return;
  }
  _mapInstance = L.map('map', {
    center: window.MAP_CENTER || [53.5461, -113.4938],
    zoom: window.MAP_ZOOM || 12,
    zoomControl: true,
  });
  L.tileLayer(
    window.MAP_TILE_URL || 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    {
      attribution: window.MAP_TILE_ATTRIBUTION || '&copy; OpenStreetMap',
      maxZoom: 19,
    }
  ).addTo(_mapInstance);
}

function setView(view) {
  const mapEl = document.getElementById('view-map');
  const btn = document.getElementById('view-toggle');
  if (view === 'map') {
    initMapIfNeeded();
    mapEl.classList.remove('view-hidden');
    mapEl.setAttribute('aria-hidden', 'false');
    document.body.classList.add('show-map');
    btn.textContent = '📊 Gauges';
    // Leaflet needs a hint to recompute size if the container was
    // hidden when it was created.
    if (_mapInstance) setTimeout(() => _mapInstance.invalidateSize(), 50);
  } else {
    mapEl.classList.add('view-hidden');
    mapEl.setAttribute('aria-hidden', 'true');
    document.body.classList.remove('show-map');
    btn.textContent = '🗺 Map';
  }
  try { localStorage.setItem(TOGGLE_STORAGE_KEY, view); }
  catch (e) { /* private mode — ignore */ }
}

document.getElementById('view-toggle').addEventListener('click', () => {
  const current = document.body.classList.contains('show-map') ? 'map' : 'gauges';
  setView(current === 'map' ? 'gauges' : 'map');
});

// Restore the last-shown view across page reloads — handy for the
// kiosk: a vroom.service restart that reloads the page shouldn't
// bounce the operator back to gauges if they were watching the map.
(function restoreView() {
  let saved = 'gauges';
  try { saved = localStorage.getItem(TOGGLE_STORAGE_KEY) || 'gauges'; }
  catch (e) { /* private mode — default to gauges */ }
  if (saved === 'map') setView('map');
})();
