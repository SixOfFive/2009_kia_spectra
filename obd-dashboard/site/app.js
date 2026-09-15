/* app.js -- the vroom OBD-II log page.
 *
 * Reads what the projects VM keeps in data/ and redraws when a file changes:
 *   obdlog.csv     every row the board has served: charts, latest values, table, text states
 *   vehicle.json   the last non-empty single values read while the engine ran (VIN, codes ...)
 *   status.json    the puller's last check: board reachable, engine, downloads
 * The page never talks to the car; only the VM's puller does.
 */
(function () {
  "use strict";

  const { Timeline, TimeChart, fmtDateTime, fmtDay, fmtHM, fmtNum, GAP_MS } = window.VroomCharts;
  const REFRESH_MS = 30e3;
  const $ = (id) => document.getElementById(id);

  // From the firmware's OBD_PIDS table (obd_elm.cpp), plus the four columns the log adds.
  const META = {
    rpm: { name: "Engine speed", unit: "rpm", dec: 0, group: "engine", tip: "Crankshaft revolutions per minute" },
    speed_kmh: { name: "Vehicle speed", unit: "km/h", dec: 0, group: "engine", tip: "Road speed as the ECU sees it" },
    coolant_C: { name: "Coolant temperature", unit: "°C", dec: 0, group: "engine", tip: "Engine coolant temperature" },
    load_pct: { name: "Engine load", unit: "%", dec: 0, group: "engine", tip: "Calculated load: airflow as a share of peak airflow" },
    throttle_pct: { name: "Throttle position", unit: "%", dec: 0, group: "engine", tip: "Absolute throttle position" },
    iat_C: { name: "Intake air temperature", unit: "°C", dec: 0, group: "engine", tip: "Air temperature at the intake" },
    timing_deg: { name: "Timing advance", unit: "°", dec: 1, group: "engine", tip: "Ignition timing before top dead centre, cylinder 1" },
    maf_gps: { name: "Mass air flow", unit: "g/s", dec: 2, group: "engine", tip: "Mass of air entering the engine" },
    map_kPa: { name: "Intake manifold pressure", unit: "kPa", dec: 0, group: "engine", tip: "Absolute pressure in the intake manifold" },
    oil_C: { name: "Oil temperature", unit: "°C", dec: 0, group: "engine", tip: "Engine oil temperature" },
    fuelsys: { name: "Fuel system", unit: "", group: "fuel", tip: "Open loop ignores the oxygen sensors; closed loop trims fuel by them" },
    stft_pct: { name: "Short-term fuel trim", unit: "%", dec: 1, group: "fuel", tip: "Fast fuel correction, bank 1. Positive means adding fuel" },
    ltft_pct: { name: "Long-term fuel trim", unit: "%", dec: 1, group: "fuel", tip: "Learned fuel correction, bank 1. Positive means adding fuel" },
    o2s1_V: { name: "O2 sensor 1 (upstream)", unit: "V", dec: 3, group: "fuel", tip: "Bank 1 sensor 1. Swings around 0.45 V in closed loop" },
    o2s2_V: { name: "O2 sensor 2 (downstream)", unit: "V", dec: 3, group: "fuel", tip: "Bank 1 sensor 2, after the catalyst. Steadier than sensor 1 on a healthy catalyst" },
    lambda: { name: "Commanded air-fuel ratio", unit: "λ", dec: 3, group: "fuel", tip: "Lambda; 1.000 is stoichiometric" },
    fuelpres_kPa: { name: "Fuel pressure", unit: "kPa", dec: 0, group: "fuel", tip: "Fuel pressure (gauge)" },
    fuellvl_pct: { name: "Fuel level", unit: "%", dec: 0, group: "fuel", tip: "Fuel tank level input" },
    baro_kPa: { name: "Barometric pressure", unit: "kPa", dec: 0, group: "fuel", tip: "Ambient absolute pressure" },
    ecuv_V: { name: "ECU supply voltage", unit: "V", dec: 2, group: "electrical", tip: "Control module voltage: what the engine computer is being fed" },
    atrv_V: { name: "Voltage at the OBD port", unit: "V", dec: 1, group: "electrical", tip: "What the reader measures on its own supply pin (ATRV)" },
    battery_V: { name: "Board's battery reading", unit: "V", dec: 2, group: "electrical", tip: "The vroom board's own battery voltage reading" },
    runtime_s: { name: "Run time since start", unit: "s", dec: 0, group: "trip", tip: "Seconds since this engine start" },
    clrkm_km: { name: "Distance since codes cleared", unit: "km", dec: 0, group: "trip", tip: "Kilometres since the fault codes were last cleared" },
    warmups: { name: "Warm-ups since codes cleared", unit: "", dec: 0, group: "trip", integer: true, tip: "Warm-up cycles since the fault codes were last cleared" },
    milkm_km: { name: "Distance with check-engine light on", unit: "km", dec: 0, group: "trip", tip: "Kilometres driven with the check-engine light lit" },
    ambient_C: { name: "Ambient air temperature", unit: "°C", dec: 0, group: "trip", tip: "Outside air temperature" },
    mil: { name: "Check-engine light", unit: "", dec: 0, group: "codes", bool: true, tip: "The malfunction indicator lamp, as the ECU reports it" },
    dtc_count: { name: "Stored fault codes", unit: "", dec: 0, group: "codes", integer: true, tip: "How many fault codes the ECU has stored" },
  };
  const SUFFIX_UNITS = { C: "°C", pct: "%", kmh: "km/h", gps: "g/s", deg: "°", V: "V", kPa: "kPa", s: "s", km: "km" };

  const GROUPS = [
    ["engine", "Engine"], ["fuel", "Fuel & air"], ["electrical", "Electrical"],
    ["trip", "Distance & time"], ["codes", "Fault codes"], ["other", "Other columns"],
  ];
  // Values that share a unit and a question share one axis. A key's colour slot is its
  // place in this list, so a missing column never repaints the others.
  const COMBINED = [
    { group: "engine", title: "Temperatures", unit: "°C", keys: ["coolant_C", "iat_C", "ambient_C"],
      tip: "Coolant, intake air and outside air on one scale" },
    { group: "fuel", title: "Fuel trims", unit: "%", keys: ["stft_pct", "ltft_pct"],
      tip: "Short- and long-term correction, bank 1. Positive means the ECU is adding fuel" },
    { group: "electrical", title: "Supply voltages", unit: "V", keys: ["ecuv_V", "atrv_V", "battery_V"],
      tip: "What the ECU is fed, what the reader measures at the OBD port, and the board's own reading" },
  ];
  const TILES = ["rpm", "speed_kmh", "coolant_C", "ecuv_V", "fuellvl_pct", "mil"];
  const VEHICLE_ORDER = ["vin", "calid", "ecu", "obdstd", "fueltype", "proto_name", "elm", "reader"];
  const MONO = new Set(["vin", "calid", "reader"]);
  const DT = /^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})$/;
  const NUM = /^-?\d+(\.\d+)?$/;

  const tl = new Timeline($("tip"));
  // A link can name the view: #range=7d, #range=all&gaps=0, #range=drive:3 (the 4th drive).
  const linked = new URLSearchParams(location.hash.slice(1));
  const state = {
    data: null, vehicle: null, status: null, zoom: null, tags: {}, chartSig: "", tableLimit: 200,
    range: linked.get("range") || load("range") || "drive",
    compress: linked.has("gaps") ? linked.get("gaps") !== "0" : load("compress") !== "0",
  };
  let charts = [];

  // ---- small helpers ------------------------------------------------------------------

  function load(k) { try { return localStorage.getItem("vroom." + k); } catch (e) { return null; } }
  function save(k, v) { try { localStorage.setItem("vroom." + k, v); } catch (e) { /* private window */ } }
  function remember() {                            // this browser, and the address bar for sharing
    save("range", state.range);
    save("compress", state.compress ? "1" : "0");
    history.replaceState(null, "", "#range=" + state.range + (state.compress ? "" : "&gaps=0"));
  }

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
  }

  function meta(key) {
    if (META[key]) return META[key];
    const m = /^(.*)_(C|pct|kmh|gps|deg|V|kPa|s|km)$/.exec(key);
    const base = (m ? m[1] : key).replace(/_/g, " ");
    return { name: base.charAt(0).toUpperCase() + base.slice(1), unit: m ? SUFFIX_UNITS[m[2]] : "", group: "other", tip: "Column " + key };
  }

  function withUnit(text, unit) {
    if (!unit) return text;
    return unit === "°" ? text + "°" : text + " " + unit;
  }

  function fmtDur(ms) {
    const s = Math.round(ms / 1000);
    if (s < 60) return s + " s";
    const m = Math.round(s / 60);
    if (m < 60) return m + " min";
    const h = Math.floor(m / 60);
    return h < 48 ? h + " h " + String(m % 60).padStart(2, "0") + " min" : Math.round(h / 24) + " days";
  }

  // A run of n rows lasts one row interval longer than the time between its end rows. The
  // interval is the run's own average: rows came every 30 s before fw 4.80, 10 s since.
  function spanMs(a, b, n) { return n > 1 ? ((b - a) * n) / (n - 1) : 0; }

  function ago(ts) {                               // ts in seconds
    if (!ts) return "never";
    const s = Date.now() / 1000 - ts;
    if (s < 5) return "just now";
    if (s < 90) return Math.round(s) + " s ago";
    if (s < 5400) return Math.round(s / 60) + " min ago";
    if (s < 36 * 3600) return Math.round(s / 3600) + " h ago";
    return fmtDay(ts * 1000) + " " + fmtHM(ts * 1000);
  }

  function fmtBytes(n) {
    if (n == null) return "?";
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
    return (n / 1048576).toFixed(2) + " MB";
  }

  function statusEl(kind, text) { return el("span", "status " + kind, text); }

  // ---- the CSV ------------------------------------------------------------------------

  function parseCsv(text) {
    if (text.indexOf('"') < 0) {                    // the board and the VM never need quotes
      return text.split("\n").map((line) => line.replace(/\r$/, "").split(","));
    }
    const rows = [];
    let row = [], cell = "", quoted = false;
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (quoted) {
        if (c !== '"') cell += c;
        else if (text[i + 1] === '"') { cell += '"'; i++; }
        else quoted = false;
      } else if (c === '"') quoted = true;
      else if (c === ",") { row.push(cell); cell = ""; }
      else if (c === "\n" || c === "\r") {
        if (c === "\r" && text[i + 1] === "\n") i++;
        row.push(cell); rows.push(row); row = []; cell = "";
      } else cell += c;
    }
    if (cell || row.length) { row.push(cell); rows.push(row); }
    return rows;
  }

  // A header line starts every block of rows (the board's own download repeats it when a
  // generation's columns differ), so each row is read against the header above it.
  function buildData(text) {
    const started = performance.now();
    const recs = [];
    let header = null, skipped = 0;
    for (const cells of parseCsv(text)) {
      if (cells.length === 1 && cells[0] === "") continue;
      if (cells[0] === "datetime") { header = cells; continue; }
      const m = DT.exec(cells[0]);
      if (!header || cells.length !== header.length || !m || +m[1] < 2020) { skipped++; continue; }
      recs.push({ t: new Date(+m[1], m[2] - 1, +m[3], +m[4], +m[5], +m[6]).getTime(), header, cells });
    }
    recs.sort((a, b) => a.t - b.t);
    const keys = [], index = new Map();
    for (const r of recs) {
      for (const k of r.header) if (k !== "datetime" && !index.has(k)) { index.set(k, keys.length); keys.push(k); }
    }
    const n = recs.length, T = new Float64Array(n);
    const raw = keys.map(() => new Array(n).fill(""));
    recs.forEach((r, i) => {
      T[i] = r.t;
      for (let c = 1; c < r.header.length; c++) raw[index.get(r.header[c])][i] = r.cells[c];
    });
    const cols = {};
    keys.forEach((k, j) => {
      const vals = raw[j], num = new Float64Array(n);
      let nonEmpty = 0, numeric = 0, dec = 0;
      for (let i = 0; i < n; i++) {
        const s = vals[i];
        num[i] = NaN;
        if (!s) continue;
        nonEmpty++;
        if (NUM.test(s)) {
          numeric++;
          num[i] = +s;
          const dot = s.indexOf(".");
          if (dot >= 0) dec = Math.max(dec, s.length - dot - 1);
        }
      }
      cols[k] = { key: k, raw: vals, num, nonEmpty, text: nonEmpty > 0 && numeric < nonEmpty, dec: Math.min(dec, 3), meta: meta(k) };
    });
    return { T, keys, cols, n, skipped, ms: performance.now() - started };
  }

  // ---- charts ---------------------------------------------------------------------------

  function chartDefs(d) {
    const numeric = d.keys.filter((k) => !d.cols[k].text && d.cols[k].nonEmpty > 0);
    const used = new Set(), defs = [];
    for (const key of numeric) {
      if (used.has(key)) continue;
      const combo = COMBINED.find((c) => c.keys.includes(key));
      if (combo) {
        const keys = combo.keys.filter((k) => numeric.includes(k));
        if (keys.length > 1) {
          keys.forEach((k) => used.add(k));
          defs.push({ group: combo.group, title: combo.title, unit: combo.unit, tip: combo.tip,
                      series: keys.map((k) => ({ key: k, slot: combo.keys.indexOf(k) + 1 })) });
          continue;
        }
      }
      used.add(key);
      const m = d.cols[key].meta;
      defs.push({ group: m.group, title: m.name, unit: m.unit, tip: m.tip, bool: !!m.bool, integer: !!m.integer,
                  series: [{ key, slot: 1 }] });
    }
    return defs;
  }

  function fmtValue(v, s, def) {
    if (!Number.isFinite(v)) return "no reading";
    if (def.bool) return v ? "on" : "off";
    return withUnit(fmtNum(v, s.dec), s.unit);
  }

  function buildCharts(defs) {
    for (const c of charts) c.chart.destroy();
    charts = [];
    const host = $("charts");
    host.replaceChildren();
    const d = state.data;
    for (const [gkey, gname] of GROUPS) {
      const mine = defs.filter((x) => x.group === gkey);
      if (!mine.length) continue;
      const section = el("section", "group");
      const grid = el("div", "group-grid");
      section.append(el("h2", null, gname), grid);
      host.append(section);
      for (const def of mine) {
        const card = el("div", "card");
        const head = el("div", "card-head");
        const title = el("h3", "card-title", def.title);
        if (def.unit && !def.bool) title.append(" ", el("span", "card-unit", "(" + def.unit + ")"));
        const stat = el("div", "card-stat");
        head.append(title, stat);
        card.append(head, el("p", "card-tip", def.tip));
        const series = def.series.map((s) => {
          const col = d.cols[s.key];
          return { key: s.key, name: col.meta.name, color: "--series-" + s.slot, values: col.num,
                   dec: col.meta.dec !== undefined ? col.meta.dec : col.dec, unit: col.meta.unit };
        });
        if (series.length > 1) {
          const legend = el("div", "legend");
          for (const s of series) {
            const key = el("span", "key", s.name);
            key.style.setProperty("--key", "var(" + s.color + ")");
            legend.append(key);
          }
          card.append(legend);
        }
        const plot = el("div", "plot");
        card.append(plot);
        grid.append(card);
        const chart = new TimeChart(tl, plot, {
          label: def.title, series, step: def.bool || def.integer, integer: def.integer, min0: def.integer,
          ticks: def.bool ? [{ v: 0, label: "off" }, { v: 1, label: "on" }] : null, domain: def.bool ? [-0.15, 1.15] : null,
          format: (v, s) => fmtValue(v, s, def),
          onRender: (stats) => renderStat(stat, series, stats, def),
        });
        chart.opts.step = def.bool || def.integer;
        series.forEach((s) => { s.step = chart.opts.step; });
        charts.push({ chart, def, series });
      }
    }
  }

  function renderStat(node, series, stats, def) {
    node.replaceChildren();
    const have = stats.filter((s) => s.n);
    if (!have.length) return;
    if (series.length === 1) {
      node.append(el("strong", null, fmtValue(stats[0].last, series[0], def)));
      if (!def.bool && stats[0].max > stats[0].min) {
        node.append(" · " + fmtNum(stats[0].min, series[0].dec) + "–" + fmtNum(stats[0].max, series[0].dec) + " in range");
      }
    } else {
      const lo = Math.min(...have.map((s) => s.min)), hi = Math.max(...have.map((s) => s.max));
      const dec = Math.max(...series.map((s) => s.dec));
      node.append(fmtNum(lo, dec) + "–" + fmtNum(hi, dec) + " in range");
    }
  }

  // ---- the range --------------------------------------------------------------------------

  function viewFor(range) {
    const d = state.data, segs = tl.segs;
    if (range.startsWith("drive")) {
      const s = range === "drive" ? segs[segs.length - 1] : segs[+range.slice(6)];
      if (s) return [s.t0, s.t1];
    }
    const span = { "24h": 1, "7d": 7, "30d": 30 }[range];
    if (span) return [Date.now() - span * 864e5, Date.now()];
    return [d.T[0] - 15e3, d.T[d.n - 1] + 15e3];
  }

  function applyView() {
    const d = state.data;
    for (const b of $("ranges").querySelectorAll("button")) {
      b.setAttribute("aria-pressed", String(b.dataset.range === state.range));
    }
    const sel = $("drive");
    const want = state.range === "drive" ? "drive:" + (tl.segs.length - 1) : state.range;
    sel.value = [...sel.options].some((o) => o.value === want) ? want : "";
    $("unzoom").hidden = !state.zoom;
    if (!d || !d.n) return;
    const v = state.zoom || viewFor(state.range);
    tl.setView(v[0], v[1]);
    const [i0, i1] = tl.rows();
    const empty = $("empty");
    empty.hidden = i1 > i0;
    if (i1 <= i0) {
      empty.textContent = "No rows in this range. The newest row is from " + fmtDateTime(d.T[d.n - 1]) +
        " — Last drive or All shows it.";
    }
    renderTiles(i0, i1);
    renderStates(i0, i1);
    renderTable();
  }

  function renderDrives() {
    const sel = $("drive");
    sel.replaceChildren(el("option", null, "—"));
    sel.options[0].value = "";
    const T = state.data.T;
    tl.segs.map((s, k) => [s, k]).reverse().forEach(([s, k]) => {
      const a = T[s.i0], b = T[s.i1 - 1];
      const o = el("option", null, fmtDateTime(a).slice(0, 15) + "  " + fmtHM(a) + "–" + fmtHM(b) +
        "  (" + (s.i1 - s.i0 > 1 ? fmtDur(spanMs(a, b, s.i1 - s.i0)) : "1 row") + ")");
      o.value = "drive:" + k;
      sel.append(o);
    });
  }

  // ---- panels -------------------------------------------------------------------------------

  function renderTiles(i0, i1) {
    const host = $("tiles"), d = state.data;
    host.replaceChildren();
    for (const key of TILES) {
      const col = d.cols[key];
      if (!col || !col.nonEmpty) continue;
      let i = i1 - 1;
      while (i >= i0 && !Number.isFinite(col.num[i])) i--;
      const tile = el("div", "tile");
      tile.append(el("div", "tile-label", col.meta.name));
      const value = el("div", "tile-value");
      if (i < i0) value.textContent = "—";
      else if (col.meta.bool) value.append(statusEl(col.num[i] ? "critical" : "good", col.num[i] ? "On" : "Off"));
      else {
        value.textContent = fmtNum(col.num[i], col.meta.dec !== undefined ? col.meta.dec : col.dec);
        if (col.meta.unit) value.append(el("span", "tile-unit", col.meta.unit));
      }
      tile.append(value, el("div", "tile-at", i < i0 ? "no reading in range" : "at " + fmtDateTime(d.T[i]).slice(4)));
      host.append(tile);
    }
  }

  function renderStates(i0, i1) {
    const host = $("states"), d = state.data, T = d.T;
    host.replaceChildren();
    const textKeys = d.keys.filter((k) => d.cols[k].text);
    if (!textKeys.length) { host.append(el("p", "muted", "The log has no text columns.")); return; }
    for (const key of textKeys) {
      const col = d.cols[key];
      const spans = [];
      let cur = null;
      for (let i = i0; i < i1; i++) {
        const v = col.raw[i];
        if (!v) { cur = null; continue; }
        if (cur && cur.value === v && T[i] - cur.t1 <= GAP_MS) { cur.t1 = T[i]; cur.rows++; continue; }
        cur = { value: v, t0: T[i], t1: T[i], rows: 1 };
        spans.push(cur);
      }
      host.append(el("h3", "card-title", col.meta.name));
      if (!spans.length) { host.append(el("p", "muted", "No readings in this range.")); continue; }
      const table = el("table", "spans");
      const head = el("tr");
      ["State", "From", "To", "For"].forEach((h) => head.append(el("th", null, h)));
      table.append(head);
      const shown = spans.slice(-12).reverse();
      for (const s of shown) {
        const tr = el("tr");
        tr.append(el("td", null, s.value), el("td", "num", fmtDateTime(s.t0).slice(4)),
                  el("td", "num", fmtHM(s.t1) + ":" + String(new Date(s.t1).getSeconds()).padStart(2, "0")),
                  el("td", "num", s.rows > 1 ? fmtDur(spanMs(s.t0, s.t1, s.rows)) : "1 row"));
        table.append(tr);
      }
      host.append(table);
      if (spans.length > shown.length) {
        host.append(el("p", "panel-note", (spans.length - shown.length) + " earlier changes in this range are in the table below."));
      }
    }
  }

  function renderVehicle() {
    const host = $("vehicle"), values = (state.vehicle && state.vehicle.values) || {};
    host.replaceChildren();
    const keys = VEHICLE_ORDER.filter((k) => values[k]);
    if (!keys.length) {
      host.append(el("dt", null, "Not read yet"),
                  el("dd", null, "The VM reads these while the engine runs within WiFi range of the board."));
      return;
    }
    for (const k of keys) {
      const v = values[k];
      const dd = el("dd");
      dd.append(el("span", MONO.has(k) ? "mono" : null, v.value));
      dd.append(el("span", "when", "seen " + ago(v.seen_ts) + (v.since_ts !== v.seen_ts ? " · same since " + ago(v.since_ts) : "")));
      host.append(el("dt", null, v.label), dd);
    }
  }

  function renderCodes() {
    const host = $("codes"), values = (state.vehicle && state.vehicle.values) || {}, d = state.data;
    host.replaceChildren();
    const facts = el("dl", "facts");
    const mil = d && d.cols.mil;
    if (mil && mil.nonEmpty) {
      let i = d.n - 1;
      while (i >= 0 && !Number.isFinite(mil.num[i])) i--;
      const dd = el("dd");
      dd.append(statusEl(mil.num[i] ? "critical" : "good", mil.num[i] ? "On" : "Off"),
                el("span", "when", "in the log at " + fmtDateTime(d.T[i]).slice(4)));
      facts.append(el("dt", null, "Check-engine light"), dd);
    }
    for (const part of ["stored", "pending", "perm"]) {
      const v = values["codes_" + part];
      if (!v) continue;
      const dd = el("dd");
      if (v.value === "none") dd.append(statusEl("good", "None"));
      else {
        const list = el("div", "code-list");
        v.value.split(",").forEach((c) => list.append(el("span", "code", c.trim())));
        dd.append(list);
      }
      dd.append(el("span", "when", "read " + ago(v.seen_ts)));
      facts.append(el("dt", null, v.label.replace(" fault codes", "")), dd);
    }
    host.append(facts);
    const monitors = Object.keys(values).filter((k) => k.startsWith("mon:"));
    if (monitors.length) {
      host.append(el("h3", "card-title", "Readiness monitors"));
      const ul = el("ul", "monitors");
      for (const k of monitors) {
        const v = values[k], name = k.slice(4);
        const kind = v.value === "complete" ? "good" : v.value === "incomplete" ? "warning" : "none";
        const li = el("li");
        li.append(statusEl(kind, name + " — " + v.value));
        ul.append(li);
      }
      host.append(ul);
    }
    if (!facts.children.length && !monitors.length) {
      host.append(el("p", "muted", "Fault codes have not been read yet. The VM reads them while the engine runs within WiFi range."));
    }
  }

  function renderBoard() {
    const host = $("board"), s = state.status;
    host.replaceChildren();
    if (!s) { host.append(el("dt", null, "Status"), el("dd", null, "No status from the puller yet")); return; }
    const row = (label, ...nodes) => { const dd = el("dd"); dd.append(...nodes); host.append(el("dt", null, label), dd); };
    const obd = s.obd || {}, log = obd.log || {}, pull = s.pull || {}, arch = s.archive || {}, b = s.board || {};
    row("Board in the car", s.reachable ? statusEl("good", "Answering") : statusEl("warning", "Not answering"),
        el("span", "when", "last answered " + ago(s.seen_ts) + " · checked " + ago(s.checked_ts)));
    if (obd.engine) {
      row("Engine", obd.engine === "running" ? "Running" : "Off",
          el("span", "when", "OBD link " + (obd.link || "?") + (s.reachable ? "" : " · as of the last answer")));
    }
    if (pull.ok_ts) {
      row("Last download", ago(pull.ok_ts),
          el("span", "when", fmtBytes(pull.bytes) + " in " + pull.seconds + " s · " + pull.added + " new rows" +
             (pull.dropped ? " · " + pull.dropped + " unreadable lines" : "")));
    }
    if (pull.error) row("Download problem", statusEl("warning", pull.error));
    if (s.pull_note) row("Puller", s.pull_note);
    if (log.bytes !== undefined) {
      row("Log on the board", fmtBytes(log.bytes),
          el("span", "when", (log.en ? "logging on" : "logging off") +
             (log.every_s ? " · a row every " + log.every_s + " s" : "") +
             (log.sweep_ms ? " · last sweep " + (log.sweep_ms / 1000).toFixed(1) + " s" : "")));
    }
    if (arch.rows !== undefined) {
      row("Kept on the VM", arch.rows.toLocaleString() + " rows",
          el("span", "when", (arch.first ? arch.first + " → " + arch.last + " · " : "") + fmtBytes(arch.bytes)));
    }
    if (state.data && state.data.skipped) row("Rows not drawn", String(state.data.skipped), el("span", "when", "no valid date"));
    if (b.fw) row("Firmware", b.fw, el("span", "when", "built " + b.build));
    if (b.uptime_s !== undefined) row("Board uptime", fmtDur(b.uptime_s * 1000), el("span", "when", "WiFi " + b.rssi + " dBm · as of " + ago(b.at_ts)));
    if (obd.board_v !== undefined) row("Battery (board)", obd.board_v.toFixed(2) + " V");
    if (b.last_run) row("Last engine run", fmtDateTime(b.last_run * 1000).slice(4), el("span", "when", "auto-start " + (b.as_en ? b.as_state : "off")));
  }

  function renderLive() {
    const s = state.status, box = $("live"), text = $("live-text");
    let kind = "critical", msg = "No status from the VM yet";
    if (state.error) msg = state.error;
    else if (s) {
      const age = Date.now() / 1000 - s.checked_ts;
      if (age > 300) msg = "Puller last checked " + ago(s.checked_ts) + " (it should every minute)";
      else if (!s.reachable) { kind = "warning"; msg = "Car out of reach · last answered " + ago(s.seen_ts); }
      else if (s.obd && s.obd.engine === "running") { kind = "good"; msg = "Engine running · checked " + ago(s.checked_ts); }
      else { kind = "good"; msg = "Car in reach, engine off · checked " + ago(s.checked_ts); }
    }
    box.dataset.state = kind;
    text.textContent = msg;
  }

  // ---- table ----------------------------------------------------------------------------------

  function renderTable() {
    const d = state.data;
    if (!d) return;
    const [i0, i1] = tl.rows();
    $("table-count").textContent = "(" + (i1 - i0).toLocaleString() + " in range)";
    const wrap = $("table-wrap");
    if (wrap.hidden) return;
    const keys = d.keys.filter((k) => d.cols[k].nonEmpty);
    const table = $("table");
    table.className = "rows";
    table.replaceChildren();
    const head = el("tr");
    head.append(el("th", null, "Date & time"));
    for (const k of keys) {
      const th = el("th", null, d.cols[k].meta.name);
      if (d.cols[k].meta.unit) { th.append(el("br"), el("span", "muted", d.cols[k].meta.unit)); }
      head.append(th);
    }
    const thead = el("thead");
    thead.append(head);
    const tbody = el("tbody");
    const stop = Math.max(i0, i1 - state.tableLimit);
    for (let i = i1 - 1; i >= stop; i--) {
      const tr = el("tr");
      tr.append(el("td", null, fmtDateTime(d.T[i])));
      for (const k of keys) tr.append(el("td", d.cols[k].text ? "text" : null, d.cols[k].raw[i]));
      tbody.append(tr);
    }
    table.append(thead, tbody);
    $("table-more").hidden = i1 - i0 <= state.tableLimit;
  }

  // ---- loading ------------------------------------------------------------------------------

  async function fetchChanged(name) {
    const res = await fetch("data/" + name, { cache: "no-cache" });
    if (!res.ok) throw new Error("data/" + name + ": HTTP " + res.status);
    const tag = [res.headers.get("ETag"), res.headers.get("Last-Modified")].join("|");
    if (tag !== "|" && state.tags[name] === tag) return null;
    state.tags[name] = tag;
    return res;
  }

  function setData(d) {
    state.data = d;
    tl.setTimes(d.T);
    const defs = chartDefs(d), sig = JSON.stringify(defs);
    if (sig !== state.chartSig) { state.chartSig = sig; buildCharts(defs); }
    else for (const c of charts) for (const s of c.series) s.values = d.cols[s.key].num;
    renderDrives();
    $("foot-parse").textContent = d.n.toLocaleString() + " rows, " + d.keys.length + " columns, read in " +
      Math.max(1, Math.round(d.ms)) + " ms";
    $("tiles").hidden = !d.n;
    if (!d.n) {
      $("empty").hidden = false;
      $("empty").textContent = "The log is empty so far. The board writes a row every 10 s while the engine runs, " +
        "and the VM picks new rows up within a couple of minutes of the car being in WiFi range.";
    }
    renderCodes();
    renderBoard();
    applyView();
  }

  async function refresh() {
    if (state.fetching) return;
    state.fetching = true;
    state.lastFetch = Date.now();
    try {
      const st = await fetchChanged("status.json");
      if (st) { state.status = await st.json(); renderBoard(); }
      state.error = null;
      const veh = await fetchChanged("vehicle.json").catch(() => null);
      if (veh) { state.vehicle = await veh.json(); renderVehicle(); renderCodes(); }
      const csv = await fetchChanged("obdlog.csv");
      if (csv) {
        for (const c of charts) c.chart.host.parentElement.style.opacity = "0.6";
        setData(buildData(await csv.text()));
        for (const c of charts) c.chart.host.parentElement.style.opacity = "";
      }
    } catch (e) {
      state.error = "Could not read the log from the VM (" + e.message + ")";
    } finally {
      state.fetching = false;
    }
    renderLive();
  }

  // ---- wiring -------------------------------------------------------------------------------

  $("ranges").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-range]");
    if (!b) return;
    state.range = b.dataset.range;
    state.zoom = null;
    remember();
    applyView();
  });
  $("drive").addEventListener("change", (e) => {
    if (!e.target.value) return;
    state.range = e.target.value;
    state.zoom = null;
    remember();
    applyView();
  });
  $("gaps").checked = state.compress;
  tl.compress = state.compress;
  $("gaps").addEventListener("change", (e) => {
    state.compress = e.target.checked;
    remember();
    tl.setCompress(state.compress);
  });
  $("unzoom").addEventListener("click", () => { state.zoom = null; applyView(); });
  tl.on("zoom", (a, b) => { state.zoom = [a, b]; applyView(); });
  tl.on("unzoom", () => { if (state.zoom) { state.zoom = null; applyView(); } });
  $("table-toggle").addEventListener("click", () => {
    const wrap = $("table-wrap");
    wrap.hidden = !wrap.hidden;
    $("table-toggle").textContent = wrap.hidden ? "Show the rows as a table" : "Hide the table";
    renderTable();
  });
  $("table-more").addEventListener("click", () => { state.tableLimit += 200; renderTable(); });
  // A tab coming back into view catches up at once, but visibility can flap (embedded
  // panes, tab previews), so never more than one fetch round per 10 s from this.
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && Date.now() - (state.lastFetch || 0) > 10e3) refresh();
  });

  renderVehicle();
  renderCodes();
  refresh();
  setInterval(() => { if (!document.hidden) refresh(); }, REFRESH_MS);
  setInterval(renderLive, 5000);
})();
