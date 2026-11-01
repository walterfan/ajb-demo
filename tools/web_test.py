"""Exercise the real Web GUI HTTP boundary and C++ renderer, without speakers."""

import argparse
import csv
import io
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]


class WebIntegrationTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls):
        (ROOT / "out").mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="web-test-", dir=ROOT / "out")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        cls.base = f"http://127.0.0.1:{port}"
        cls.process = subprocess.Popen([
            sys.executable, str(ROOT / "tools/web_server.py"), "--binary", str(cls.binary),
            "--port", str(port), "--output-dir", cls.temporary.name,
        ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                with urlopen(cls.base + "/api/config", timeout=2) as response:
                    cls.configuration = json.load(response)
                return
            except (URLError, TimeoutError):
                if cls.process.poll() is not None:
                    raise RuntimeError("Web server exited during startup")
                time.sleep(.1)
        raise RuntimeError("Web server did not start")

    @classmethod
    def stop_server(cls):
        cls.process.terminate()
        try:
            cls.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait(timeout=5)
        cls.temporary.cleanup()

    def request(self, path, data=None, headers=None):
        values = {"Origin": self.base, "X-AJB-Request": "1", "Content-Type": "application/json"}
        values.update(headers or {})
        body = None if data is None else json.dumps(data).encode("utf-8")
        request = Request(self.base + path, data=body, headers=values)
        try:
            response = urlopen(request, timeout=15)
        except HTTPError as error:
            response = error
        with response:
            return response.status, response.headers, response.read()

    def test_static_files_and_local_boundary(self):
        status, headers, body = self.request("/")
        self.assertEqual(status, 200)
        self.assertIn("试听对比".encode(), body)
        self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        for path in ("/app.js", "/styles.css", "/icon.svg"):
            self.assertEqual(self.request(path)[0], 200)
        for path in ("/pyproject.toml", "/.git/config", "/api/runs/missing/files/../../README.md"):
            self.assertEqual(self.request(path)[0], 404)
        self.assertEqual(self.request("/", headers={"Host": "evil.test"})[0], 400)
        self.assertEqual(self.request("/", headers={"Sec-Fetch-Site": "cross-site"})[0], 200)
        self.assertEqual(self.request("/api/source.wav", headers={"Sec-Fetch-Site": "cross-site"})[0], 403)
        self.assertEqual(self.request("/api/runs", {}, {"Origin": "https://evil.test"})[0], 403)
        self.assertEqual(self.request("/api/runs", {}, {"X-AJB-Request": "0"})[0], 403)
        self.assertEqual(self.request("/api/runs", {}, {"Content-Type": "text/plain"})[0], 415)
        self.assertEqual(self.request("/api/runs", {"large": "x" * 5000})[0], 413)

    def test_validation_rejects_unsafe_and_inconsistent_parameters(self):
        defaults = self.configuration["defaults"]
        for changes in ({"scenario": "unknown"}, {"input": "../../etc/passwd"},
                        {"underrun_policy": "unknown"}, {"rebuffer_resume_ms": 0},
                        {"rebuffer_timeout_ms": 2001}, {"seed": True}, {"seed": -1}, {"jitter_ms": 15},
                        {"network_base_ms": 1001}, {"network_base_ms": 950},
                        {"small_ms": defaults["large_ms"]}, {"min_ms": 200}):
            with self.subTest(changes=changes):
                self.assertEqual(self.request("/api/runs", defaults | changes)[0], 422)

    def test_rebuffer_renderer_and_frozen_source(self):
        config = self.configuration["defaults"] | {"underrun_policy": "rebuffer"}
        status, _, body = self.request("/api/runs", config)
        self.assertEqual(status, 202)
        job_id = json.loads(body)["id"]
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            status, _, body = self.request("/api/runs/" + job_id)
            self.assertEqual(status, 200)
            result = json.loads(body)
            if result["status"] != "rendering":
                break
            time.sleep(.15)
        self.assertEqual(result["status"], "ready", result)
        self.assertEqual(result["config"], config)
        track = result["tracks"][0]
        self.assertGreater(track["summary"]["rebuffer_ms"], 0)
        waiting = [row for row in track["ticks"] if row[6] == "rebuffer"]
        self.assertGreater(len(waiting), 0)
        for row in waiting:
            self.assertEqual(row[4], 0)
            self.assertEqual(row[5], 0)
            self.assertEqual(row[7:10], [None, None, None])
        self.assertTrue(any(e["kind"] == "rebuffer_start" for e in track["events"]))

    def test_renderer_results_downloads_and_audio_ranges(self):
        self.assertEqual(self.configuration["source"]["duration"], 45)
        status, _, body = self.request("/api/runs", self.configuration["defaults"])
        self.assertEqual(status, 202)
        job_id = json.loads(body)["id"]
        self.assertEqual(self.request("/api/runs", self.configuration["defaults"])[0], 409)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            status, _, body = self.request("/api/runs/" + job_id)
            self.assertEqual(status, 200)
            result = json.loads(body, parse_constant=lambda _: self.fail("Invalid non-finite JSON"))
            if result["status"] != "rendering":
                break
            time.sleep(.15)
        self.assertEqual(result["status"], "ready", result)
        self.assertEqual(result["config"], self.configuration["defaults"])
        self.assertEqual([t["id"] for t in result["tracks"]], ["fixed-20", "fixed-150", "adaptive"])
        for track in result["tracks"]:
            self.assertGreaterEqual(len(track["ticks"]), 4500)
            self.assertEqual(len(track["ticks"][0]), 13)
            for row in track["ticks"]:
                if row[7] is not None:
                    self.assertAlmostEqual(row[7] + row[8], row[9], places=6)
            status, headers, audio = self.request(
                f"/api/runs/{job_id}/files/{track['id']}.wav", headers={"Range": "bytes=0-127"})
            self.assertEqual(status, 206)
            self.assertEqual(len(audio), 128)
            self.assertEqual(audio[:4], b"RIFF")
            self.assertTrue(headers["Content-Range"].startswith("bytes 0-127/"))
        status, _, body = self.request(f"/api/runs/{job_id}/files/network.csv")
        self.assertEqual(status, 200)
        self.assertEqual(len(list(csv.DictReader(io.StringIO(body.decode())))), 4500)
        status, _, body = self.request(f"/api/runs/{job_id}/files/experiment.txt")
        self.assertEqual(status, 200)
        for key, value in self.configuration["defaults"].items():
            self.assertIn(f"{key}={value}\n", body.decode())
        self.assertEqual(self.request(f"/api/runs/{job_id}/files/README.md")[0], 404)
        self.assertEqual(self.request("/api/source.wav", headers={"Range": "bytes=0-127"})[0], 206)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    WebIntegrationTest.binary = args.binary.resolve(strict=True)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
