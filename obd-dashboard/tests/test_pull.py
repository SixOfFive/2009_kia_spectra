"""Tests for obd-dashboard/puller/vroom_obd_pull.py -- no board, no network.

    python3 obd-dashboard/tests/test_pull.py

The fixtures follow the board's real formats: the 30-column header fw 4.75-4.77 wrote,
the 26 columns fw 4.78 writes for this car, and the /obdjson answers of fw 4.79. Values
are made up.
"""

import csv
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "puller"))
import vroom_obd_pull as pull  # noqa: E402

H30 = pull.COLUMNS
H26 = [c for c in H30 if c not in ("map_kPa", "oil_C", "o2s1_V", "fuelpres_kPa")]


def reading(dt, rpm, **extra):
    r = {"datetime": dt, "rpm": str(rpm), "speed_kmh": "0", "coolant_C": "40", "fuelsys": "Closed loop",
         "stft_pct": "-1.6", "ltft_pct": "0.8", "o2s2_V": "0.430", "ecuv_V": "14.17",
         "battery_V": "14.23", "mil": "0", "dtc_count": "0"}
    r.update(extra)
    return r


def csv_text(header, readings):
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(header)
    for r in readings:
        w.writerow([r.get(c, "") for c in header])
    return out.getvalue()


R1 = reading("2026-09-14 19:52:10", 1175)
R2 = reading("2026-09-14 19:52:40", 970)
R3 = reading("2026-09-14 19:53:11", 941, fuelsys="Open loop (load or deceleration)")

OVERVIEW = {"reader": "00:11:22:33:44:55", "link": "off", "engine": "off", "why": "", "elm": "",
            "proto_name": "", "proto": 0, "since": 400, "rssi": 0, "atrv": None, "board_v": 12.33,
            "log": {"en": True, "bytes": 1544, "rows": 0, "last": 0}, "cats": [], "vals": []}
BOARD_JSON = {"vbatt": 12.32, "rssi": -58, "uptime_s": 41324, "fw": "4.79", "build": "Sep 14 2026",
              "ssid": "a-network-name", "bssid": "72:00:00:00:00:99", "ip": "192.168.15.94",
              "time_ok": True, "as_en": True, "as_state": "watching", "last_run": 1789437114}
VIN = "TESTVIN0123456789"


def running(**extra):
    doc = dict(OVERVIEW, link="live", engine="running", since=60, elm="ELM327 v1.5",
               proto_name="ISO 15765-4 (CAN 11/500)")
    doc.update(extra)
    return doc


def vehicle_doc(known=True, vin=VIN):
    return dict(running(), vals=[
        {"k": "obdstd", "n": "OBD standard", "u": "", "d": 0, "s": 1, "x": "OBD-II (California ARB)"},
        {"k": "fueltype", "n": "Fuel type", "u": "", "d": 0, "s": 3},
    ], info={"known": known, "vin": vin if known else "", "calid": "", "ecu": "ECM-TEST"})


def codes_doc(known=True):
    c = {"known": known}
    if known:
        c.update(stored=["P0420"], pending=[], perm=None)
    c.update(diesel=False, mon=[{"n": "Misfire", "a": 1, "c": 1}, {"n": "Catalyst", "a": 1, "c": 0},
                                {"n": "Heated catalyst", "a": 0, "c": 0}])
    return dict(running(), vals=[], codes=c)


class FakeBoard(pull.Board):
    """Routes are path -> bytes, a dict (sent as JSON), an exception, or a list served in turn."""

    def __init__(self, routes):
        super().__init__("http://board.test", 1)
        self.routes = routes
        self.calls = []

    def get(self, path, timeout=0, tries=2):
        self.calls.append(path)
        r = self.routes.get(path)
        if isinstance(r, list):
            r = r.pop(0) if len(r) > 1 else r[0]
        if r is None:
            raise pull.BoardError(path + ": no route")
        if isinstance(r, Exception):
            raise r
        return r if isinstance(r, bytes) else json.dumps(r).encode()


class Tmp(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.cfg = pull.Config(board="http://board.test", data_dir=self.dir)
        self.sleeps = []

    def tearDown(self):
        self._tmp.cleanup()

    def run_once(self, board, now):
        return pull.run_once(self.cfg, board, now=now, sleep=self.sleeps.append)

    def read(self, name):
        return (self.dir / name).read_text(encoding="utf-8")


class ParseLog(unittest.TestCase):
    def test_repeated_header_and_both_formats(self):
        text = csv_text(H30, [R1, R2]) + csv_text(H26, [R2, R3])
        rows, dropped = pull.parse_log(text)
        self.assertEqual(dropped, 0)
        self.assertEqual(rows, [R1, R2, R2, R3])   # the empty cells are left out either way

    def test_a_row_cut_off_by_the_download_is_dropped(self):
        rows, dropped = pull.parse_log(csv_text(H26, [R1, R2]) + "2026-09-14 19:5")
        self.assertEqual((rows, dropped), ([R1, R2], 1))

    def test_malformed_lines_are_dropped(self):
        good = csv_text(H26, [R1])
        text = ("1,2,3\n" + good + "2026-09-14 19:53:00,1,2\n"
                + good.splitlines()[1].replace("2026-09-14", "garbage!!!") + "\n")
        rows, dropped = pull.parse_log(text)
        self.assertEqual((rows, dropped), ([R1], 3))

    def test_empty(self):
        self.assertEqual(pull.parse_log(""), ([], 0))


class Archive(unittest.TestCase):
    def test_merge_adds_only_new_rows_in_time_order(self):
        merged, added = pull.merge_rows([R1, R3], [R3, R2])
        self.assertEqual((merged, added), ([R1, R2, R3], 1))
        self.assertEqual(pull.merge_rows([R1], [R1]), ([R1], 0))

    def test_render_keeps_firmware_order_drops_empty_columns_and_round_trips(self):
        odd = dict(R2, zz_new="7", map_kPa="35")
        text = pull.render_csv([R1, odd])
        header = text.splitlines()[0].split(",")
        self.assertEqual(header[0], "datetime")
        self.assertEqual(header[-1], "zz_new")
        self.assertIn("map_kPa", header)
        self.assertNotIn("oil_C", header)           # no row has a value for it
        self.assertEqual(header.index("rpm") < header.index("map_kPa") < header.index("fuelsys"), True)
        self.assertEqual(pull.parse_log(text), ([R1, odd], 0))


class PullDecision(unittest.TestCase):
    cfg = pull.Config()

    def decide(self, pull_state, engine="off", bytes_=1544, have=True, now=10_000):
        obd = {"engine": engine, "log": {"bytes": bytes_}}
        return pull.pull_decision(pull_state, obd, have, now, self.cfg)[0]

    def test_cases(self):
        ok = {"ok_ts": 9_990, "board_bytes": 1544}
        self.assertTrue(self.decide({}))                                     # first download
        self.assertTrue(self.decide(ok, have=False))                          # archive missing
        self.assertFalse(self.decide(ok))                                     # unchanged
        self.assertTrue(self.decide(ok, bytes_=1700))                         # changed, engine off
        self.assertFalse(self.decide(ok, engine="running", bytes_=1700))      # growing: wait
        self.assertTrue(self.decide(dict(ok, ok_ts=9_000), engine="running", bytes_=1700))
        self.assertTrue(self.decide(dict(ok, ok_ts=10_000 - 86_400)))         # daily re-check
        self.assertFalse(self.decide(dict(ok, ok_ts=1, board_bytes=0), bytes_=0))
        self.assertFalse(self.decide(dict(ok, fails=3, fail_ts=9_900), bytes_=1700))  # backing off
        self.assertTrue(self.decide(dict(ok, fails=1, fail_ts=9_900), bytes_=1700))


class SingleValues(unittest.TestCase):
    def test_what_counts_as_a_value(self):
        found = pull.single_values(vehicle_doc(), codes_doc())
        values = {k: v for k, (label, v) in found.items()}
        self.assertEqual(values["vin"], VIN)
        self.assertEqual(values["ecu"], "ECM-TEST")
        self.assertNotIn("calid", values)                     # empty
        self.assertEqual(values["obdstd"], "OBD-II (California ARB)")
        self.assertNotIn("fueltype", values)                  # s=3: the car has no such value
        self.assertEqual(values["proto_name"], "ISO 15765-4 (CAN 11/500)")
        self.assertEqual(values["codes_stored"], "P0420")
        self.assertEqual(values["codes_pending"], "none")    # answered, no codes
        self.assertNotIn("codes_perm", values)                # null: not answered
        self.assertEqual(values["mon:Misfire"], "complete")
        self.assertEqual(values["mon:Catalyst"], "incomplete")
        self.assertEqual(values["mon:Heated catalyst"], "not available")
        self.assertEqual(found["mon:Misfire"][0], "Readiness: Misfire")

    def test_unknown_answers_give_nothing_but_the_link_facts(self):
        found = pull.single_values(vehicle_doc(known=False), codes_doc(known=False))
        self.assertNotIn("vin", found)
        self.assertNotIn("codes_stored", found)
        self.assertIn("elm", found)

    def test_since_moves_only_when_the_value_changes(self):
        store = {}
        pull.merge_values(store, {"vin": ("VIN", VIN)}, 100)
        pull.merge_values(store, {"vin": ("VIN", VIN)}, 200)
        self.assertEqual((store["values"]["vin"]["seen_ts"], store["values"]["vin"]["since_ts"]), (200, 100))
        pull.merge_values(store, {"vin": ("VIN", "OTHER")}, 300)
        self.assertEqual(store["values"]["vin"]["since_ts"], 300)

    def test_text_file(self):
        self.assertIn("Nothing yet", pull.render_values_txt({}))
        store = {}
        pull.merge_values(store, pull.single_values(vehicle_doc(), codes_doc()), 1_789_480_000)
        text = pull.render_values_txt(store)
        lines = text.splitlines()
        vin_line = next(line for line in lines if line.startswith("VIN "))
        self.assertIn(VIN, vin_line)
        self.assertLess(lines.index(vin_line), next(i for i, x in enumerate(lines) if x.startswith("Readiness")))


class RunOnce(Tmp):
    def routes(self, **over):
        r = {"/obdjson": dict(OVERVIEW), "/json": dict(BOARD_JSON),
             "/obdlog.csv": (csv_text(H30, [R1, R2]) + csv_text(H26, [R2, R3])).encode()}
        r.update(over)
        return r

    def test_first_download_and_what_status_keeps(self):
        board = FakeBoard(self.routes())
        status = self.run_once(board, 1_000)
        self.assertIn("/obdlog.csv", board.calls)
        self.assertFalse([c for c in board.calls if "cat=" in c])       # engine off: no category reads
        self.assertEqual(pull.parse_log(self.read("obdlog.csv"))[0], [R1, R2, R3])
        self.assertEqual((self.dir / "obdlog-board.csv").read_bytes(), self.routes()["/obdlog.csv"])
        self.assertEqual(status["archive"]["rows"], 3)
        self.assertEqual(status["pull"]["board_bytes"], 1544)
        saved = self.read("status.json")
        self.assertIn('"fw": "4.79"', saved)
        for secret in ("ssid", "bssid", "a-network-name", "192.168.15.94"):
            self.assertNotIn(secret, saved)
        self.assertIn("Nothing yet", self.read("vehicle-info.txt"))
        self.assertEqual(json.loads(self.read("vehicle.json")), {"values": {}})

    def test_unchanged_log_is_not_downloaded_again_and_new_rows_append(self):
        self.run_once(FakeBoard(self.routes()), 1_000)
        board = FakeBoard(self.routes())
        self.run_once(board, 1_060)
        self.assertNotIn("/obdlog.csv", board.calls)
        self.assertNotIn("/json", board.calls)                            # board facts: every 10 min
        r4 = reading("2026-09-15 07:00:00", 800)
        grown = self.routes(**{"/obdjson": dict(OVERVIEW, log={"en": True, "bytes": 1700, "rows": 1, "last": 5}),
                               "/obdlog.csv": csv_text(H26, [R3, r4]).encode()})
        status = self.run_once(FakeBoard(grown), 1_120)
        self.assertEqual(status["pull"]["added"], 1)
        self.assertEqual(pull.parse_log(self.read("obdlog.csv"))[0], [R1, R2, R3, r4])  # the board dropped R1, R2; the VM did not

    def test_unreachable_board(self):
        self.run_once(FakeBoard(self.routes()), 1_000)
        before = self.read("obdlog.csv")
        status = self.run_once(FakeBoard({}), 1_060)
        self.assertFalse(status["reachable"])
        self.assertEqual(status["seen_ts"], 1_000)
        self.assertEqual(self.read("obdlog.csv"), before)

    def test_values_are_read_while_running_and_the_board_is_handed_back(self):
        board = FakeBoard(self.routes(**{
            "/obdjson": running(),
            "/obdjson?cat=vehicle": [vehicle_doc(known=False), vehicle_doc()],
            "/obdjson?cat=codes": codes_doc(),
        }))
        status = self.run_once(board, 5_000)
        last_cat = max(i for i, c in enumerate(board.calls) if "cat=" in c)
        self.assertIn("/obdjson", board.calls[last_cat + 1:])
        self.assertEqual(self.sleeps, [self.cfg.settle_s])                 # the vehicle read took one wait
        self.assertEqual(status["capture"]["ok_ts"], 5_000)
        store = json.loads(self.read("vehicle.json"))
        self.assertEqual(store["values"]["vin"]["value"], VIN)
        self.assertIn(VIN, self.read("vehicle-info.txt"))
        board2 = FakeBoard(self.routes(**{"/obdjson": running()}))
        self.run_once(board2, 5_060)                                         # not due again for 30 min
        self.assertFalse([c for c in board2.calls if "cat=" in c])

    def test_empty_values_never_replace_kept_ones(self):
        routes = {"/obdjson": running(), "/obdjson?cat=vehicle": vehicle_doc(), "/obdjson?cat=codes": codes_doc()}
        self.run_once(FakeBoard(self.routes(**routes)), 5_000)
        blank = dict(vehicle_doc(), info={"known": True, "vin": "", "calid": "", "ecu": ""})
        routes.update({"/obdjson?cat=vehicle": blank})
        self.run_once(FakeBoard(self.routes(**routes)), 5_000 + 1_800)
        store = json.loads(self.read("vehicle.json"))
        self.assertEqual(store["values"]["vin"]["value"], VIN)
        self.assertEqual(store["values"]["vin"]["seen_ts"], 5_000)

    def test_a_failed_category_read_still_hands_the_board_back(self):
        board = FakeBoard(self.routes(**{
            "/obdjson": running(),
            "/obdjson?cat=vehicle": vehicle_doc(),
            "/obdjson?cat=codes": pull.BoardError("/obdjson?cat=codes: timed out"),
        }))
        status = self.run_once(board, 5_000)
        i = board.calls.index("/obdjson?cat=codes")
        self.assertIn("/obdjson", board.calls[i + 1:])
        self.assertEqual(status["capture"]["fail_ts"], 5_000)
        self.assertNotIn("ok_ts", status["capture"])
        self.assertIn(VIN, self.read("vehicle-info.txt"))                  # what was read is kept

    def test_no_values_before_the_link_settles(self):
        board = FakeBoard(self.routes(**{"/obdjson": running(since=5)}))
        self.run_once(board, 5_000)
        self.assertFalse([c for c in board.calls if "cat=" in c])


class ConfigFromEnv(unittest.TestCase):
    def test_types(self):
        cfg = pull.Config.from_env({"VROOM_BOARD_URL": "http://x/", "VROOM_DATA_DIR": "/tmp/v",
                                    "VROOM_MIN_GAP_RUNNING_S": "30", "VROOM_TIMEOUT_S": "2.5"})
        self.assertEqual((cfg.board, cfg.data_dir, cfg.min_gap_running_s, cfg.timeout_s),
                         ("http://x", Path("/tmp/v"), 30, 2.5))


if __name__ == "__main__":
    unittest.main(verbosity=1)
