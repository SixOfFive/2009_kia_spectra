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

  btnStart: document.getElementById('btn-start'),
  btnStop:  document.getElementById('btn-stop'),
  btnPing:  document.getElementById('btn-ping'),
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

// ----- Buttons -----

async function sendCommand(cmd) {
  try {
    const res = await fetch('/api/command', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({cmd}),
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    // Force an immediate refresh
    poll();
  } catch (err) {
    alert(`Command ${cmd} failed: ${err.message}`);
  }
}

els.btnStart.addEventListener('click', () => {
  if (confirm('Start the engine?')) sendCommand('start_engine');
});
els.btnStop.addEventListener('click', () => {
  if (confirm('Stop the engine?')) sendCommand('stop_engine');
});
els.btnPing.addEventListener('click', () => sendCommand('ping'));
