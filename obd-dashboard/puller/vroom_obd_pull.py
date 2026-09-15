#!/usr/bin/env python3
"""vroom_obd_pull.py -- keep the car's OBD-II log on the projects VM.

The board in the car (esp32-s3/voltage_monitor) writes /obdlog.csv: a row every 30 s
while the engine runs, in two generations of about 512 KB, the older dropped when the
newer fills. It only answers while the car is within WiFi range. This keeps every row
it ever served, and the single values the log has no column for.

One run is one check. vroom-obd-pull.timer starts it every minute:

  1. GET /obdjson -- the small answer the board's own Overview page polls. It says how
     big the log is (log.bytes) and whether the engine runs, and never opens the car
     link.
  2. When log.bytes changed, GET /obdlog.csv. The download is kept as
     obdlog-board.csv and its rows are merged into obdlog.csv, which only grows.
  3. While the engine runs, read the values the log has no column for -- VIN,
     calibration ID, ECU name, OBD standard, fuel type, protocol, fault codes and
     readiness -- and keep the last non-empty value of each in vehicle.json and
     vehicle-info.txt. The board forgets them at every reboot.
  4. Write status.json for the page.

Standard library only. Every request is a GET the board's own pages make. The board
reads VIN and fault codes only while a page asks for that category, so step 3 asks the
way the Vehicle and Fault codes pages do, then hands the board back to the Overview.
"""

import argparse
import csv
import http.client
import io
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

VERSION = "1"


@dataclass
class Config:
    board: str = "http://192.168.15.94"
    data_dir: Path = Path("/var/www/html/vroom/data")
    timeout_s: float = 15.0          # the small JSON calls; the car's WiFi link is slow and lossy
    csv_timeout_s: float = 180.0     # a full log is about 1 MB over that link
    min_gap_running_s: int = 120     # while the engine runs, download the growing log at most this often
    resync_s: int = 86400            # download an unchanged log this often anyway
    capture_every_s: int = 1800      # single values, while the engine runs
    board_every_s: int = 600         # board facts from /json
    settle_s: float = 5.0            # wait for the OBD task to read a category
    settle_tries: int = 4

    ENV = {
        "VROOM_BOARD_URL": "board",
        "VROOM_DATA_DIR": "data_dir",
        "VROOM_TIMEOUT_S": "timeout_s",
        "VROOM_CSV_TIMEOUT_S": "csv_timeout_s",
        "VROOM_MIN_GAP_RUNNING_S": "min_gap_running_s",
        "VROOM_RESYNC_S": "resync_s",
        "VROOM_CAPTURE_EVERY_S": "capture_every_s",
        "VROOM_BOARD_EVERY_S": "board_every_s",
    }

    @classmethod
    def from_env(cls, env) -> "Config":
        cfg = cls()
        for name, attr in cls.ENV.items():
            if env.get(name):
                setattr(cfg, attr, type(getattr(cfg, attr))(env[name]))
        cfg.board = cfg.board.rstrip("/")
        return cfg


def say(msg: str) -> None:
    print(msg, flush=True)                     # journald keeps it


# ---- the board --------------------------------------------------------------------------

class BoardError(Exception):
    pass


class Board:
    def __init__(self, base: str, timeout_s: float):
        self.base = base.rstrip("/")
        self.timeout_s = timeout_s

    def get(self, path: str, timeout: float = 0, tries: int = 2) -> bytes:
        last = ""
        for attempt in range(tries):
            if attempt:
                time.sleep(2)
            req = urllib.request.Request(self.base + path,
                                         headers={"User-Agent": "vroom-obd-pull/" + VERSION})
            try:
                with urllib.request.urlopen(req, timeout=timeout or self.timeout_s) as r:
                    return r.read()
            except urllib.error.HTTPError as e:
                last = f"{path}: HTTP {e.code}"
            except (urllib.error.URLError, http.client.HTTPException, OSError) as e:
                last = f"{path}: {getattr(e, 'reason', e)}"
        raise BoardError(last)

    def json(self, path: str) -> dict:
        raw = self.get(path)
        try:
            doc = json.loads(raw.decode("utf-8", errors="replace"))
        except ValueError as e:
            raise BoardError(f"{path}: not JSON ({e})") from None
        if not isinstance(doc, dict):
            raise BoardError(f"{path}: not a JSON object")
        return doc


# ---- the log ----------------------------------------------------------------------------

# The firmware's column order: OBD_PIDS in obd_elm.cpp, then the four the log adds. Since
# fw 4.78 the board writes only the columns this car reports; the archive keeps this order
# for the columns it has and puts any it does not know last.
COLUMNS = [
    "datetime", "rpm", "speed_kmh", "coolant_C", "load_pct", "throttle_pct", "iat_C",
    "timing_deg", "maf_gps", "map_kPa", "oil_C", "fuelsys", "stft_pct", "ltft_pct", "o2s1_V",
    "o2s2_V", "lambda", "fuelpres_kPa", "fuellvl_pct", "baro_kPa", "ecuv_V", "runtime_s",
    "clrkm_km", "warmups", "milkm_km", "ambient_C", "atrv_V", "battery_V", "mil", "dtc_count",
]
DATETIME = re.compile(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}")


def parse_log(text: str) -> tuple:
    """(rows, dropped) for an OBD log as the board serves it.

    The download repeats the header wherever a generation's columns differ, and a value
    the car did not report is an empty cell. A row is {column: value} with the empty
    cells left out, so the same reading compares equal whichever firmware wrote it.
    """
    lines = text.split("\n")
    dropped = 1 if lines[-1].strip() else 0    # no final newline: the download stopped mid-row
    lines.pop()
    rows, header = [], None
    for line in lines:
        line = line.rstrip("\r")
        if not line.strip():
            continue
        cells = next(csv.reader([line]))
        if cells[0] == "datetime":
            header = cells
        elif header and len(cells) == len(header) and DATETIME.fullmatch(cells[0]):
            rows.append({c: v for c, v in zip(header, cells) if v != ""})
        else:
            dropped += 1
    return rows, dropped


def columns_for(rows: list) -> list:
    have = {}
    for r in rows:
        for c in r:
            have.setdefault(c, None)
    known = [c for c in COLUMNS if c in have]
    return known + [c for c in have if c not in COLUMNS]


def merge_rows(old: list, new: list) -> tuple:
    """(merged, added): old plus the rows of new it does not already hold, in time order."""
    seen = {tuple(sorted(r.items())) for r in old}
    added = []
    for r in new:
        key = tuple(sorted(r.items()))
        if key not in seen:
            seen.add(key)
            added.append(r)
    if not added:
        return old, 0
    merged = old + added
    merged.sort(key=lambda r: r["datetime"])   # stable: equal times keep their order
    return merged, len(added)


def render_csv(rows: list) -> str:
    cols = columns_for(rows) or ["datetime"]
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(cols)
    for r in rows:
        w.writerow([r.get(c, "") for c in cols])
    return out.getvalue()


# ---- single values ----------------------------------------------------------------------

# The values the log has no column for, in the order the text file lists them. Readiness
# monitors follow, one line each, under the names the board gives them.
VALUE_LABELS = {
    "vin": "VIN",
    "calid": "Calibration ID",
    "ecu": "ECU name",
    "obdstd": "OBD standard",
    "fueltype": "Fuel type",
    "proto_name": "Protocol",
    "elm": "Reader firmware",
    "reader": "Reader address",
    "codes_stored": "Stored fault codes",
    "codes_pending": "Pending fault codes",
    "codes_perm": "Permanent fault codes",
}


def single_values(vehicle, codes) -> dict:
    """{key: (label, value)} for the non-empty single values in a /obdjson?cat=vehicle and
    a ?cat=codes answer. A code list the car answered with no codes is a value ("none");
    a list it did not answer (null) is not."""
    found = {}

    def put(key, value, label=""):
        value = " ".join(str(value).split())   # one line each in the text file
        if value:
            found[key] = (VALUE_LABELS.get(key) or label or key, value)

    for doc in (vehicle, codes):
        for key in ("proto_name", "elm", "reader"):
            if doc and doc.get(key):
                put(key, doc[key])
    if vehicle:
        info = vehicle.get("info") or {}
        for key in ("vin", "calid", "ecu"):
            if info.get(key):
                put(key, info[key])
        for v in vehicle.get("vals") or []:
            if v.get("s") != 1 or not v.get("k"):     # 1 = a value; 0 not read, 2 no data, 3 unsupported
                continue
            if "x" in v:
                put(v["k"], v["x"], v.get("n", ""))
            elif "v" in v:
                put(v["k"], f"{v['v']} {v.get('u', '')}", v.get("n", ""))
    if codes:
        c = codes.get("codes") or {}
        if c.get("known"):
            for part in ("stored", "pending", "perm"):
                if isinstance(c.get(part), list):
                    put("codes_" + part, ", ".join(c[part]) or "none")
        for m in c.get("mon") or []:
            if m.get("n"):
                state = ("complete" if m.get("c") else "incomplete") if m.get("a") else "not available"
                put("mon:" + m["n"], state, "Readiness: " + m["n"])
    return found


def merge_values(store: dict, found: dict, now: int) -> None:
    values = store.setdefault("values", {})
    for key, (label, value) in found.items():
        cur = values.get(key) or {}
        since = cur.get("since_ts") if cur.get("value") == value else None
        values[key] = {"label": label, "value": value, "seen_ts": now, "since_ts": since or now}
    store["written_ts"] = now


def local(ts) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts))


def render_values_txt(store: dict) -> str:
    values = store.get("values") or {}
    lines = [
        "vroom - OBD values that have no column in the log (obdlog.csv)",
        "",
        "Each is the last non-empty value the board in the car reported while the engine was",
        "running. An empty or unread value never replaces one. The projects VM keeps them",
        "because the board forgets them at every reboot.",
        "",
    ]
    if not values:
        return "\n".join(lines + ["Nothing yet: no value has been read while the engine was running."]) + "\n"
    order = [k for k in VALUE_LABELS if k in values] + [k for k in values if k not in VALUE_LABELS]
    table = [("Field", "Value", "Last seen", "Same value since")]
    for key in order:
        v = values[key]
        table.append((v["label"], v["value"], local(v["seen_ts"]), local(v["since_ts"])))
    widths = [max(len(row[i]) for row in table) for i in range(4)]

    def fmt(row):
        return "  ".join(cell.ljust(w) for cell, w in zip(row, widths)).rstrip()

    lines += [f"Written {local(store.get('written_ts', 0))}", "", fmt(table[0]),
              fmt(["-" * w for w in widths])]
    lines += [fmt(row) for row in table[1:]]
    return "\n".join(lines) + "\n"


# ---- files ------------------------------------------------------------------------------

def write_bytes(path: Path, data: bytes) -> None:
    """Replace path in one step, so the page never reads half a file."""
    tmp = path.with_name("." + path.name + ".tmp")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def write_text(path: Path, text: str) -> None:
    write_bytes(path, text.encode("utf-8"))


def write_json(path: Path, doc: dict) -> None:
    write_text(path, json.dumps(doc, indent=1) + "\n")


def read_json(path: Path) -> dict:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return doc if isinstance(doc, dict) else {}


# ---- one check --------------------------------------------------------------------------

OBD_FIELDS = ("reader", "link", "engine", "since", "elm", "proto_name", "atrv", "board_v", "mil", "dtc")
LOG_FIELDS = ("en", "bytes", "rows", "last", "every_s", "sweep_ms")   # the last two since fw 4.80
# /json also carries the WiFi network's name and the access point's address. The data
# folder is served to the LAN, so only these are kept.
BOARD_FIELDS = ("fw", "build", "uptime_s", "rssi", "vbatt", "time_ok", "as_en", "as_state", "last_run")
LIVE_SETTLE_S = 20        # a new link's first log sweep goes before any category read
CAPTURE_RETRY_S = 300


def pick(doc: dict, fields) -> dict:
    return {k: doc[k] for k in fields if k in doc}


def pull_decision(pull: dict, obd: dict, have_archive: bool, now: int, cfg: Config) -> tuple:
    fails = pull.get("fails") or 0
    if fails:
        wait = min(60 * 2 ** (fails - 1), 900) - (now - (pull.get("fail_ts") or 0))
        if wait > 0:
            return False, f"last download failed; trying again in {wait} s"
    board_bytes = (obd.get("log") or {}).get("bytes")
    last_ok = pull.get("ok_ts") or 0
    if not have_archive or not last_ok:
        return True, "first download"
    if board_bytes is not None and board_bytes != pull.get("board_bytes"):
        wait = cfg.min_gap_running_s - (now - last_ok)
        if obd.get("engine") == "running" and wait > 0:
            return False, f"log growing; next download in {wait} s"
        return True, "log changed"
    if board_bytes and now - last_ok >= cfg.resync_s:
        return True, "daily re-check"
    return False, "log unchanged"


def pull_log(cfg: Config, board, status: dict, now: int) -> None:
    pull = status.setdefault("pull", {})
    started = time.monotonic()

    def failed(why):
        pull.update(fail_ts=now, fails=(pull.get("fails") or 0) + 1, error=why)
        say("log download failed: " + why)

    try:
        raw = board.get("/obdlog.csv", timeout=cfg.csv_timeout_s, tries=1)
    except BoardError as e:
        return failed(str(e))
    text = raw.decode("utf-8", errors="replace")
    if text.strip() and not text.startswith("datetime"):
        return failed("the answer to /obdlog.csv is not an OBD log")
    rows, dropped = parse_log(text)
    d = cfg.data_dir
    write_bytes(d / "obdlog-board.csv", raw)
    archive = d / "obdlog.csv"
    old = parse_log(archive.read_text(encoding="utf-8", errors="replace"))[0] if archive.exists() else []
    merged, added = merge_rows(old, rows)
    if added or not archive.exists():
        write_text(archive, render_csv(merged))
    pull.update(ok_ts=now, fails=0, fail_ts=None, error=None,
                seconds=round(time.monotonic() - started, 1), bytes=len(raw), rows=len(rows),
                added=added, dropped=dropped,
                board_bytes=((status.get("obd") or {}).get("log") or {}).get("bytes"))
    status["archive"] = {
        "rows": len(merged),
        "bytes": archive.stat().st_size,
        "first": merged[0]["datetime"] if merged else None,
        "last": merged[-1]["datetime"] if merged else None,
        "columns": len(columns_for(merged)),
    }
    if added:
        say(f"log: {added} new rows, {len(merged)} kept")


def capture_due(status: dict, overview: dict, now: int, cfg: Config) -> bool:
    if overview.get("engine") != "running" or overview.get("link") != "live":
        return False
    if (overview.get("since") or 0) < LIVE_SETTLE_S:
        return False
    cap = status.get("capture") or {}
    if now - (cap.get("fail_ts") or 0) < CAPTURE_RETRY_S:
        return False
    return now - (cap.get("ok_ts") or 0) >= cfg.capture_every_s


def category_read(cat: str, doc: dict) -> bool:
    if cat == "vehicle":
        return bool((doc.get("info") or {}).get("known")) and all(
            v.get("s") != 0 for v in doc.get("vals") or [])
    return bool((doc.get("codes") or {}).get("known"))


def capture_values(cfg: Config, board, status: dict, now: int, sleep=time.sleep) -> None:
    """Ask for the Vehicle and Fault codes categories the way their pages do, wait for the
    OBD task to read them, then hand it back to the Overview (its default)."""
    cap = status.setdefault("capture", {})
    docs = {}
    try:
        for cat in ("vehicle", "codes"):
            doc = board.json("/obdjson?cat=" + cat)
            for _ in range(cfg.settle_tries):
                if category_read(cat, doc):
                    break
                sleep(cfg.settle_s)
                doc = board.json("/obdjson?cat=" + cat)
            docs[cat] = doc
    except BoardError as e:
        cap.update(fail_ts=now, error=str(e))
        say("single values: " + str(e))
    finally:
        try:
            board.json("/obdjson")
        except BoardError:
            pass
    found = single_values(docs.get("vehicle"), docs.get("codes"))
    if found:
        path = cfg.data_dir / "vehicle.json"
        store = read_json(path)
        merge_values(store, found, now)
        write_json(path, store)
        write_text(cfg.data_dir / "vehicle-info.txt", render_values_txt(store))
    if len(docs) == 2:
        cap.update(ok_ts=now, fail_ts=None, error=None, found=len(found))
        say(f"single values read while the engine runs: {len(found)}")


def run_once(cfg: Config, board, now=None, sleep=time.sleep) -> dict:
    now = int(time.time() if now is None else now)
    d = cfg.data_dir
    d.mkdir(parents=True, exist_ok=True)
    status = read_json(d / "status.json")
    status.update(version=VERSION, checked_ts=now)
    if not (d / "vehicle.json").exists():        # the page and the download link find both from the start
        write_json(d / "vehicle.json", {"values": {}})
    if not (d / "vehicle-info.txt").exists():
        write_text(d / "vehicle-info.txt", render_values_txt({}))
    try:
        overview = board.json("/obdjson")
    except BoardError as e:
        if status.get("reachable", True):
            say("board not answering: " + str(e))
        status.update(reachable=False, error=str(e))
        write_json(d / "status.json", status)
        return status
    if status.get("reachable") is False:
        say("board answering again")
    status.update(reachable=True, error=None, seen_ts=now)
    obd = pick(overview, OBD_FIELDS)
    obd["log"] = pick(overview.get("log") or {}, LOG_FIELDS)
    status["obd"] = obd

    go, why = pull_decision(status.get("pull") or {}, obd, (d / "obdlog.csv").exists(), now, cfg)
    status["pull_note"] = why
    if go:
        pull_log(cfg, board, status, now)
    if capture_due(status, overview, now, cfg):
        capture_values(cfg, board, status, now, sleep)
    if now - ((status.get("board") or {}).get("at_ts") or 0) >= cfg.board_every_s:
        try:
            status["board"] = dict(pick(board.json("/json"), BOARD_FIELDS), at_ts=now)
        except BoardError:
            pass
    write_json(d / "status.json", status)
    return status


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Keep the car's OBD-II log on this machine (one check).")
    ap.add_argument("--board", help="board base URL (default: $VROOM_BOARD_URL, else %s)" % Config.board)
    ap.add_argument("--data-dir", help="where the files go (default: $VROOM_DATA_DIR, else %s)" % Config.data_dir)
    args = ap.parse_args(argv)
    cfg = Config.from_env(os.environ)
    if args.board:
        cfg.board = args.board.rstrip("/")
    if args.data_dir:
        cfg.data_dir = Path(args.data_dir)
    run_once(cfg, Board(cfg.board, cfg.timeout_s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
