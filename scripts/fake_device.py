# Fake smolbase device for browser-testing the web UI without touching real
# hardware (ticket #39). Serves html/ directly (ungzipped) and stubs every
# /api endpoint with believable behavior — including the destructive ones, so
# join/forget/factory-reset/update flows can be clicked end-to-end.
#
#   uv run scripts/fake_device.py        # http://localhost:8123
#
# Stdlib only. State is in-memory and resets on restart.
import json
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "html"
PORT = 8123

SETTINGS = [
    # section, key, type, label, default, min, max
    ("system", "hostname", "string", "Hostname", "", None, None),
    ("system", "tz_name", "string", "Timezone", "Etc/UTC", None, None),
    ("system", "tz", "string", "POSIX TZ", "UTC0", None, None),
    ("system", "ntp", "string", "NTP server", "pool.ntp.org", None, None),
    ("system", "brightness", "int", "Brightness", 200, 0, 255),
    ("app", "col_hour", "string", "Clock hour color", "#ffffff", None, None),
    ("app", "col_min", "string", "Clock minute color", "#ffffff", None, None),
    ("app", "col_colon", "string", "Clock colon color", "#ffffff", None, None),
    ("app", "col_host", "string", "Hostname color", "#ffffff", None, None),
    ("app", "col_ip", "string", "IP address color", "#ffffff", None, None),
    ("app", "boing", "bool", "Boing ball", True, None, None),
]

# Stance A' (ticket #34): the app-registered App-tab note; set to None to fake
# an app without one, and flip APP_TAB_SUPPRESSED to fake a suppressing app.
APP_NOTE = ("These render here for free — registering a setting is all it takes. "
            "The landing page shows the other path: a custom UI over the same "
            "values. Apps with their own UI can suppress this tab entirely.")
APP_TAB_SUPPRESSED = False
values = {s[1]: s[4] for s in SETTINGS}
secrets = {}
scan_started = 0.0

NETWORKS = [
    {"ssid": "TestNet-Alpha", "rssi": -49, "secure": True},
    {"ssid": "TestNet-Bravo", "rssi": -78, "secure": True},
    {"ssid": "CoffeeShopFree", "rssi": -85, "secure": False},
]

MIME = {".html": "text/html", ".css": "text/css", ".js": "text/javascript",
        ".json": "application/json"}


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body_json(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            return json.loads(self.rfile.read(n))
        except (json.JSONDecodeError, ValueError):
            return None

    def do_GET(self):
        global scan_started
        path = self.path.split("?")[0]
        if path == "/api/status":
            return self._send(200, {
                "name": values["hostname"] or "smolbase-2e00", "ip": "127.0.0.1",
                "apMode": False, "fwVersion": "fake-0.0.0", "uptimeS": int(time.time() % 100000),
                "heapFree": 128000, "timeSynced": True, "rssi": -49,
                "ssid": "TestNet-Alpha"})
        if path == "/api/settings":
            out = []
            for sec, key, typ, label, default, mn, mx in SETTINGS:
                item = {"key": key, "section": sec, "type": typ, "label": label,
                        "default": default, "value": values[key]}
                if typ == "int":
                    item["min"], item["max"] = mn, mx
                out.append(item)
            doc: dict = {"settings": out}
            if APP_NOTE:
                doc["appNote"] = APP_NOTE
            if APP_TAB_SUPPRESSED:
                doc["appTabSuppressed"] = True
            return self._send(200, doc)
        if path == "/api/wifi/scan":
            if "refresh=1" in self.path or scan_started == 0:
                scan_started = time.time()
            if time.time() - scan_started < 3:  # exercise the scanning state
                return self._send(200, {"status": "scanning", "networks": []})
            return self._send(200, {"status": "done", "networks": NETWORKS})
        if path == "/api/secrets":
            return self._send(200, {k: True for k in secrets})
        if path == "/recover":
            return self._send(200, b"<h2>(embedded recovery page lives in firmware)</h2>",
                              "text/html")
        # static assets
        if path == "/":
            path = "/index.html"
        f = ROOT / path.lstrip("/")
        if f.is_file() and f.resolve().is_relative_to(ROOT):
            return self._send(200, f.read_bytes(), MIME.get(f.suffix, "text/plain"))
        return self._send(404, b"Not found", "text/plain")

    def do_POST(self):
        global values, secrets
        path = self.path.split("?")[0]
        if path == "/api/settings":
            doc = self._body_json()
            if not isinstance(doc, dict):
                return self._send(400, {"error": "expected a JSON object"})
            changed = False
            for k, v in doc.items():
                if k in values and values[k] != v:
                    values[k] = v
                    changed = True
            return self._send(200, {"ok": True, "changed": changed})
        if path == "/api/wifi":
            doc = self._body_json() or {}
            if not doc.get("ssid"):
                return self._send(400, {"error": "expected {ssid,pass}"})
            time.sleep(0.5)
            return self._send(200, {"ok": True, "restarting": True})
        if path in ("/api/wifi/forget", "/api/factory-reset"):
            time.sleep(0.5)
            return self._send(200, {"ok": True, "restarting": True})
        if path == "/api/update":
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n)
            # crude image-type guards mirroring the firmware's
            m = re.search(rb"\r\n\r\n", body)
            payload = body[m.end():] if m else body
            target_fs = "target=fs" in self.path
            if target_fs and payload[8:16] != b"littlefs":
                return self._send(400, {"error": "not a littlefs image (magic missing at offset 8)",
                                        "restarting": True})
            if not target_fs and (not payload or payload[0] != 0xE9):
                return self._send(400, {"error": "not an ESP32 app image (magic 0xE9 missing at byte 0)"})
            time.sleep(1)
            return self._send(200, {"ok": True, "restarting": True})
        if path == "/api/secrets":
            doc = self._body_json()
            if not isinstance(doc, dict):
                return self._send(400, {"error": "expected a JSON object"})
            for k, v in doc.items():
                if v is None:
                    secrets.pop(k, None)
                else:
                    secrets[k] = v
            return self._send(200, {"ok": True})
        return self._send(404, b"Not found", "text/plain")

    def log_message(self, fmt, *args):  # quiet
        pass


if __name__ == "__main__":
    print(f"fake smolbase on http://localhost:{PORT} (serving {ROOT})")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
