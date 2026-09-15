/* chart.js -- canvas time-series charts for the vroom OBD-II log. No dependencies.
 *
 * Every chart shares one Timeline: the rows' times, the range on screen, whether the
 * time between drives is hidden, and the hovered row. A crosshair, a zoom or a range
 * change therefore moves every chart together.
 *
 * Rows come every 10 s (30 s before fw 4.80) while the engine runs and not at all otherwise, so a week is a
 * few short drives separated by days of nothing. With the gaps hidden, each drive in
 * range gets width in proportion to its length and the drives sit side by side,
 * separated by a hairline; hovering still reports each reading's real date and time.
 */
(function () {
  "use strict";

  const GAP_MS = 5 * 60e3;       // a longer silence between rows ends a drive
  const HALF_ROW_MS = 15e3;      // a drive's span reaches half a row past its end rows
  const DAY_MS = 864e5;
  const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
  const STEPS = [60e3, 120e3, 300e3, 600e3, 900e3, 1800e3, 3600e3, 7200e3, 10800e3, 21600e3,
                 43200e3, DAY_MS, 2 * DAY_MS, 7 * DAY_MS, 14 * DAY_MS, 28 * DAY_MS];
  const FONT = "11px system-ui, -apple-system, 'Segoe UI', sans-serif";

  const p2 = (n) => String(n).padStart(2, "0");
  const fmtDay = (t) => { const d = new Date(t); return d.getDate() + " " + MONTHS[d.getMonth()]; };
  const fmtHM = (t) => { const d = new Date(t); return p2(d.getHours()) + ":" + p2(d.getMinutes()); };
  function fmtDateTime(t) {
    const d = new Date(t);
    return DAYS[d.getDay()] + " " + d.getFullYear() + "-" + p2(d.getMonth() + 1) + "-" + p2(d.getDate()) +
      "  " + p2(d.getHours()) + ":" + p2(d.getMinutes()) + ":" + p2(d.getSeconds());
  }
  function fmtNum(v, dec) {
    if (Math.abs(v) < 1e-9) v = 0;
    return v.toLocaleString(undefined, { minimumFractionDigits: dec, maximumFractionDigits: dec });
  }
  const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  function lowerBound(a, x, lo, hi) {           // first index with a[i] >= x
    while (lo < hi) { const m = (lo + hi) >>> 1; if (a[m] < x) lo = m + 1; else hi = m; }
    return lo;
  }
  function upperBound(a, x, lo, hi) {           // first index with a[i] > x
    while (lo < hi) { const m = (lo + hi) >>> 1; if (a[m] <= x) lo = m + 1; else hi = m; }
    return lo;
  }

  // Drives: runs of rows with no silence longer than GAP_MS.
  function segmentsOf(T) {
    const segs = [];
    let s = 0;
    for (let i = 1; i <= T.length; i++) {
      if (i === T.length || T[i] - T[i - 1] > GAP_MS) {
        segs.push({ i0: s, i1: i, t0: T[s] - HALF_ROW_MS, t1: T[i - 1] + HALF_ROW_MS });
        s = i;
      }
    }
    return segs;
  }

  function niceScale(min, max, count, integer, min0) {
    if (!(max > min)) {                           // a flat line: zero stays the floor (speed at idle)
      if (min >= 0 && (min0 || min === 0)) max = min + 1;
      else { const pad = Math.abs(max) * 0.05 || 1; min -= pad; max += pad; }
    }
    const raw = (max - min) / Math.max(1, count);
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const r = raw / mag;
    let step = (r < 1.5 ? 1 : r < 3 ? 2 : r < 7 ? 5 : 10) * mag;
    if (integer) step = Math.max(1, Math.round(step));
    const lo = Math.floor(min / step) * step;
    const hi = Math.ceil(max / step) * step;
    const ticks = [];
    for (let k = 0; lo + k * step <= hi + step * 1e-6; k++) ticks.push(lo + k * step);
    const dec = Math.max(0, Math.min(4, -Math.floor(Math.log10(step) + 1e-9)));
    return { lo, hi, ticks, dec };
  }

  const startOfDay = (t) => { const d = new Date(t); d.setHours(0, 0, 0, 0); return d.getTime(); };
  const isMidnight = (t) => { const d = new Date(t); return !d.getHours() && !d.getMinutes(); };
  function firstTick(t, step) {
    if (step < DAY_MS) { const d0 = startOfDay(t); return d0 + Math.ceil((t - d0) / step) * step; }
    const d = new Date(startOfDay(t));
    if (d.getTime() < t) d.setDate(d.getDate() + 1);
    return d.getTime();
  }
  function nextTick(t, step) {
    if (step < DAY_MS) {                        // re-anchor at midnight, so DST days stay aligned
      const n = t + step, d0 = startOfDay(n);
      return d0 > startOfDay(t) ? d0 : n;
    }
    const d = new Date(t);
    d.setDate(d.getDate() + Math.round(step / DAY_MS));
    return d.getTime();
  }

  class Timeline {
    constructor(tip) {
      this.tip = tip;
      this.T = new Float64Array(0);
      this.segs = [];
      this.view = [0, 1];
      this.compress = true;
      this.hover = -1;
      this.charts = new Set();
      this.handlers = {};
      this.frame = 0;
      const mq = window.matchMedia("(prefers-color-scheme: dark)");
      if (mq.addEventListener) mq.addEventListener("change", () => this.renderAll());
      window.addEventListener("scroll", () => { if (this.hover >= 0 && !this.tip.hidden) this.setHover(-1, null); },
                              { passive: true });
    }
    on(name, fn) { (this.handlers[name] = this.handlers[name] || []).push(fn); }
    emit(name, ...args) { for (const fn of this.handlers[name] || []) fn(...args); }

    setTimes(T) { this.T = T; this.segs = segmentsOf(T); this.hover = -1; this.tip.hidden = true; }
    setView(t0, t1) { if (!(t1 > t0)) t1 = t0 + 60e3; this.view = [t0, t1]; this.renderAll(); }
    setCompress(on) { this.compress = !!on; this.renderAll(); }
    zoom(t0, t1) { this.emit("zoom", t0, t1); }
    rows() {
      const T = this.T;
      return [lowerBound(T, this.view[0], 0, T.length), upperBound(T, this.view[1], 0, T.length)];
    }

    // Pieces of time on screen, each with its x span. One piece unless gaps are hidden.
    layout(left, width) {
      const [v0, v1] = this.view;
      if (!this.compress) return { pieces: [{ t0: v0, t1: v1, x0: left, x1: left + width }], scale: width / (v1 - v0) };
      const pieces = [];
      for (const s of this.segs) {
        const a = Math.max(s.t0, v0), b = Math.min(s.t1, v1);
        if (b > a) pieces.push({ t0: a, t1: b });
      }
      if (!pieces.length) return { pieces, scale: 0 };
      const gap = pieces.length > 1 ? Math.min(12, (width * 0.25) / (pieces.length - 1)) : 0;
      const total = pieces.reduce((n, p) => n + (p.t1 - p.t0), 0);
      const scale = (width - gap * (pieces.length - 1)) / total;
      let x = left;
      for (const p of pieces) { p.x0 = x; p.x1 = x + (p.t1 - p.t0) * scale; x = p.x1 + gap; }
      return { pieces, scale };
    }

    renderAll() {
      if (this.frame) return;
      this.frame = requestAnimationFrame(() => {
        this.frame = 0;
        for (const c of this.charts) c.render();
        this.emit("render");
      });
    }

    setHover(i, src, at) {
      this.hover = i;
      for (const c of this.charts) c.renderOverlay();
      if (i < 0 || !src) { this.tip.hidden = true; return; }
      src.fillTip(this.tip, i);
      this.tip.hidden = false;
      const r = this.tip.getBoundingClientRect(), pad = 14;
      let x = at.cx + pad, y = at.cy + pad;
      if (x + r.width > window.innerWidth - 4) x = at.cx - pad - r.width;
      if (y + r.height > window.innerHeight - 4) y = at.cy - pad - r.height;
      this.tip.style.left = Math.max(4, x) + "px";
      this.tip.style.top = Math.max(4, y) + "px";
    }
  }

  class TimeChart {
    // opts: label, series [{key, name, color (a CSS variable name), values Float64Array, dec, unit}],
    //       format(v, series) -> text, step, integer, min0, ticks [{v, label}] + domain [lo, hi],
    //       onRender(stats) with per-series {min, max, last, lastT, n} for the rows in range.
    constructor(timeline, host, opts) {
      this.tl = timeline;
      this.host = host;
      this.opts = opts;
      this.series = opts.series;
      this.f = null;
      this.drag = null;
      this.base = document.createElement("canvas");
      this.over = document.createElement("canvas");
      this.over.className = "over";
      this.over.tabIndex = 0;
      this.over.setAttribute("role", "img");
      this.over.setAttribute("aria-label", opts.label + ". Arrow keys step through the readings; the table below has every value.");
      this.empty = document.createElement("div");
      this.empty.className = "plot-empty";
      this.empty.textContent = "No readings in this range";
      this.empty.hidden = true;
      host.replaceChildren(this.base, this.over, this.empty);
      this.ro = new ResizeObserver(() => this.tl.renderAll());
      this.ro.observe(host);
      this.bind();
      timeline.charts.add(this);
    }

    destroy() { this.ro.disconnect(); this.tl.charts.delete(this); }

    prep(canvas) {
      const dpr = window.devicePixelRatio || 1, w = this.host.clientWidth, h = this.host.clientHeight;
      const W = Math.round(w * dpr), H = Math.round(h * dpr);
      if (canvas.width !== W || canvas.height !== H) { canvas.width = W; canvas.height = H; }
      const ctx = canvas.getContext("2d");
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, w, h);
      return { ctx, w, h };
    }

    render() {
      const { ctx, w, h } = this.prep(this.base);
      const tl = this.tl, T = tl.T, o = this.opts;
      const [i0, i1] = tl.rows();
      let min = Infinity, max = -Infinity;
      const stats = this.series.map((s) => {
        const st = { min: Infinity, max: -Infinity, last: NaN, lastT: NaN, n: 0 }, V = s.values;
        for (let i = i0; i < i1; i++) {
          const v = V[i];
          if (Number.isFinite(v)) {
            if (v < st.min) st.min = v;
            if (v > st.max) st.max = v;
            st.last = v; st.lastT = T[i]; st.n++;
          }
        }
        if (st.n) { min = Math.min(min, st.min); max = Math.max(max, st.max); }
        return st;
      });
      if (o.onRender) o.onRender(stats);
      const has = min <= max && w >= 60;
      this.empty.hidden = has;
      if (!has) { this.f = null; this.renderOverlay(); return; }

      const top = 8, bottom = h - 22, plotH = bottom - top;
      const sc = o.ticks
        ? { lo: o.domain[0], hi: o.domain[1], ticks: o.ticks.map((t) => t.v), labels: o.ticks.map((t) => t.label) }
        : niceScale(min, max, Math.max(2, Math.floor(plotH / 42)), o.integer, o.min0);
      const labels = sc.labels || sc.ticks.map((v) => fmtNum(v, sc.dec));
      ctx.font = FONT;
      const left = Math.ceil(Math.max(22, ...labels.map((s) => ctx.measureText(s).width)) + 10);
      const right = w - 10;
      const L = tl.layout(left, right - left);
      this.f = { i0, i1, top, bottom, plotH, left, right, lo: sc.lo, hi: sc.hi, L };
      if (!L.pieces.length) { this.f = null; this.empty.hidden = false; return; }

      const muted = cssVar("--muted"), grid = cssVar("--grid"), axis = cssVar("--axis"), surface = cssVar("--surface");
      ctx.lineWidth = 1;
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      sc.ticks.forEach((v, k) => {
        const y = Math.round(this.yOf(v)) + 0.5;
        ctx.strokeStyle = grid;
        ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
        ctx.fillStyle = muted;
        ctx.fillText(labels[k], left - 8, y);
      });
      ctx.strokeStyle = axis;
      ctx.beginPath(); ctx.moveTo(left, bottom + 0.5); ctx.lineTo(right, bottom + 0.5); ctx.stroke();
      if (tl.compress) {
        for (let k = 1; k < L.pieces.length; k++) {
          const x = Math.round((L.pieces[k - 1].x1 + L.pieces[k].x0) / 2) + 0.5;
          ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, bottom); ctx.stroke();
        }
      }
      this.drawXTicks(ctx, muted, axis);

      ctx.save();
      ctx.beginPath(); ctx.rect(left - 4, top - 4, right - left + 8, plotH + 8); ctx.clip();
      for (const s of this.series) this.drawSeries(ctx, s, surface);
      ctx.restore();
      this.renderOverlay();
    }

    yOf(v) { const f = this.f; return f.top + ((f.hi - v) / (f.hi - f.lo)) * f.plotH; }

    xOf(t) {
      const P = this.f.L.pieces;
      let lo = 0, hi = P.length - 1;
      while (lo < hi) { const m = (lo + hi + 1) >> 1; if (P[m].t0 <= t) lo = m; else hi = m - 1; }
      const p = P[lo];
      return !p || t < p.t0 || t > p.t1 ? NaN : p.x0 + (t - p.t0) * this.f.L.scale;
    }

    tOf(x) {                                       // a point in a gap snaps to the nearer drive
      const P = this.f.L.pieces;
      let best = P[0], dist = Infinity;
      for (const p of P) {
        if (x >= p.x0 && x <= p.x1) return p.t0 + (x - p.x0) / this.f.L.scale;
        const d = x < p.x0 ? p.x0 - x : x - p.x1;
        if (d < dist) { dist = d; best = p; }
      }
      return x < best.x0 ? best.t0 : best.t1;
    }

    drawXTicks(ctx, muted, axis) {
      const { L, left, right, bottom } = this.f;
      const step = STEPS.find((s) => s * L.scale >= 90) || STEPS[STEPS.length - 1];
      const starts = this.tl.compress && L.pieces.length > 1;
      const cands = [];
      for (const p of L.pieces) {
        if (starts) cands.push({ x: p.x0, t: p.t0 + HALF_ROW_MS, start: true });
        for (let t = firstTick(p.t0, step), n = 0; t <= p.t1 && n < 400; t = nextTick(t, step), n++) {
          cands.push({ x: p.x0 + (t - p.t0) * L.scale, t, start: false });
        }
      }
      cands.sort((a, b) => (a.start === b.start ? a.x - b.x : a.start ? -1 : 1));
      const placed = [];
      const tryPlace = (c, label) => {
        const w = ctx.measureText(label).width;
        let a = c.start ? c.x : c.x - w / 2;
        a = Math.max(left, Math.min(a, right - w));
        if (placed.some((q) => q !== c && a < q.b + 12 && a + w > q.a - 12)) return false;
        c.label = label; c.a = a; c.b = a + w;
        return true;
      };
      ctx.font = FONT;
      for (const c of cands) {
        const label = c.start ? fmtDay(c.t) + " " + fmtHM(c.t)
          : step >= DAY_MS || isMidnight(c.t) ? fmtDay(c.t) : fmtHM(c.t);
        if (tryPlace(c, label)) placed.push(c);
      }
      if (!starts && step < DAY_MS) {              // name the day on the first time label
        const first = placed.slice().sort((a, b) => a.x - b.x)[0];
        if (first && !isMidnight(first.t)) tryPlace(first, fmtDay(first.t) + " " + first.label);
      }
      ctx.textAlign = "left";
      ctx.textBaseline = "top";
      ctx.fillStyle = muted;
      ctx.strokeStyle = axis;
      for (const c of placed) {
        const x = Math.round(c.x) + 0.5;
        ctx.beginPath(); ctx.moveTo(x, bottom); ctx.lineTo(x, bottom + 4); ctx.stroke();
        ctx.fillText(c.label, c.a, bottom + 6);
      }
    }

    // One path per series. Several readings on one pixel column draw as their min-max span,
    // so a spike survives any zoom level. A reading with no neighbour becomes a dot.
    drawSeries(ctx, s, surface) {
      const f = this.f, T = this.tl.T, V = s.values, P = f.L.pieces, scale = f.L.scale;
      const color = cssVar(s.color);
      const dots = [];
      let pk = 0, prevPk = -1, prevT = -Infinity, run = 0;
      let bx = NaN, bFirst = 0, bMin = 0, bMax = 0, bLast = 0, lastX = 0, lastY = 0;
      ctx.beginPath();
      const flush = () => {
        if (Number.isNaN(bx)) return;
        if (!run) ctx.moveTo(bx, bFirst);
        else { if (s.step) ctx.lineTo(bx, lastY); ctx.lineTo(bx, bFirst); }
        ctx.lineTo(bx, bMin); ctx.lineTo(bx, bMax); ctx.lineTo(bx, bLast);
        run++; lastX = bx; lastY = bLast; bx = NaN;
      };
      const end = () => { flush(); if (run === 1) dots.push(lastX, lastY); run = 0; };
      for (let i = f.i0; i < f.i1; i++) {
        const v = V[i];
        if (!Number.isFinite(v)) { end(); continue; }
        const t = T[i];
        while (pk < P.length - 1 && t > P[pk].t1) pk++;
        if (t < P[pk].t0 || t > P[pk].t1) { end(); continue; }
        if (t - prevT > GAP_MS || pk !== prevPk) end();
        prevT = t; prevPk = pk;
        const x = Math.round(P[pk].x0 + (t - P[pk].t0) * scale);
        const y = this.yOf(v);
        if (x === bx) { if (y < bMin) bMin = y; if (y > bMax) bMax = y; bLast = y; }
        else { flush(); bx = x; bFirst = bMin = bMax = bLast = y; }
      }
      end();
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.lineJoin = "round";
      ctx.lineCap = "round";
      ctx.stroke();
      for (let k = 0; k < dots.length; k += 2) {
        ctx.beginPath(); ctx.arc(dots[k], dots[k + 1], 5, 0, 2 * Math.PI); ctx.fillStyle = surface; ctx.fill();
        ctx.beginPath(); ctx.arc(dots[k], dots[k + 1], 3, 0, 2 * Math.PI); ctx.fillStyle = color; ctx.fill();
      }
    }

    renderOverlay() {
      const { ctx } = this.prep(this.over);
      const f = this.f;
      if (!f) return;
      const d = this.drag;
      if (d && d.moved) {
        const a = Math.max(f.left, Math.min(d.x, d.cur)), b = Math.min(f.right, Math.max(d.x, d.cur));
        ctx.fillStyle = cssVar("--select");
        ctx.fillRect(a, f.top, Math.max(0, b - a), f.plotH);
      }
      const i = this.tl.hover;
      if (i < f.i0 || i >= f.i1) return;
      const x = this.xOf(this.tl.T[i]);
      if (Number.isNaN(x)) return;
      ctx.strokeStyle = cssVar("--muted");
      ctx.lineWidth = 1;
      const xx = Math.round(x) + 0.5;
      ctx.beginPath(); ctx.moveTo(xx, f.top); ctx.lineTo(xx, f.bottom); ctx.stroke();
      const surface = cssVar("--surface");
      for (const s of this.series) {
        const v = s.values[i];
        if (!Number.isFinite(v)) continue;
        const y = this.yOf(v);
        ctx.beginPath(); ctx.arc(x, y, 6, 0, 2 * Math.PI); ctx.fillStyle = surface; ctx.fill();
        ctx.beginPath(); ctx.arc(x, y, 4, 0, 2 * Math.PI); ctx.fillStyle = cssVar(s.color); ctx.fill();
      }
    }

    fillTip(tip, i) {
      tip.replaceChildren();
      for (const s of this.series) {
        const row = document.createElement("div");
        row.className = "tip-row";
        row.style.setProperty("--key", cssVar(s.color));
        const val = document.createElement("span");
        val.className = "tip-val";
        val.textContent = this.opts.format(s.values[i], s);
        const name = document.createElement("span");
        name.className = "tip-name";
        name.textContent = s.name;
        row.append(val, name);
        tip.append(row);
      }
      const time = document.createElement("div");
      time.className = "tip-time";
      time.textContent = fmtDateTime(this.tl.T[i]);
      tip.append(time);
    }

    rowAt(x) {
      const f = this.f, T = this.tl.T;
      if (!f || f.i1 <= f.i0) return -1;
      const t = this.tOf(x);
      let j = lowerBound(T, t, f.i0, f.i1);
      if (j >= f.i1) j = f.i1 - 1;
      if (j > f.i0 && Math.abs(T[j - 1] - t) <= Math.abs(T[j] - t)) j--;
      return j;
    }

    hoverAt(x, cx, cy) {
      const f = this.f;
      if (!f) return;
      if (x < f.left - 8 || x > f.right + 8) { this.tl.setHover(-1, null); return; }
      this.tl.setHover(this.rowAt(x), this, { cx, cy });
    }

    keyTo(i) {
      const f = this.f;
      i = Math.max(f.i0, Math.min(f.i1 - 1, i));
      const r = this.over.getBoundingClientRect(), x = this.xOf(this.tl.T[i]);
      this.tl.setHover(i, this, { cx: r.left + (Number.isNaN(x) ? f.right : x), cy: r.top + f.top + 16 });
    }

    bind() {
      const o = this.over;
      const localX = (e) => e.clientX - o.getBoundingClientRect().left;
      o.addEventListener("pointerdown", (e) => {
        if (e.button !== 0 || !this.f) return;
        this.drag = { x: localX(e), cur: localX(e), moved: false };
        o.setPointerCapture(e.pointerId);
      });
      o.addEventListener("pointermove", (e) => {
        const x = localX(e);
        if (this.drag) { this.drag.cur = x; if (Math.abs(x - this.drag.x) > 4) this.drag.moved = true; }
        this.hoverAt(x, e.clientX, e.clientY);
      });
      o.addEventListener("pointerup", (e) => {
        const d = this.drag;
        this.drag = null;
        if (d && d.moved && this.f && Math.abs(d.cur - d.x) >= 6) {
          const a = this.tOf(Math.min(d.x, d.cur)), b = this.tOf(Math.max(d.x, d.cur));
          this.tl.setHover(-1, null);
          if (b > a) this.tl.zoom(a, b); else this.renderOverlay();
        } else {
          this.renderOverlay();
          this.hoverAt(localX(e), e.clientX, e.clientY);
        }
      });
      o.addEventListener("pointercancel", () => { this.drag = null; this.renderOverlay(); });
      o.addEventListener("pointerleave", (e) => {
        if (!this.drag && e.pointerType !== "touch" && !o.matches(":focus-visible")) this.tl.setHover(-1, null);
      });
      o.addEventListener("dblclick", () => this.tl.emit("unzoom"));
      o.addEventListener("focus", () => {
        if (o.matches(":focus-visible") && this.f && this.f.i1 > this.f.i0) this.keyTo(this.f.i1 - 1);
      });
      o.addEventListener("blur", () => this.tl.setHover(-1, null));
      o.addEventListener("keydown", (e) => {
        const f = this.f;
        if (!f || f.i1 <= f.i0) return;
        let i = this.tl.hover;
        if (i < f.i0 || i >= f.i1) i = f.i1 - 1;
        const n = e.shiftKey ? 10 : 1;
        if (e.key === "ArrowLeft") i -= n;
        else if (e.key === "ArrowRight") i += n;
        else if (e.key === "Home") i = f.i0;
        else if (e.key === "End") i = f.i1 - 1;
        else if (e.key === "Escape") { this.tl.setHover(-1, null); return; }
        else return;
        e.preventDefault();
        this.keyTo(i);
      });
    }
  }

  window.VroomCharts = { Timeline, TimeChart, fmtDateTime, fmtDay, fmtHM, fmtNum, GAP_MS };
})();
