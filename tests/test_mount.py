#!/usr/bin/env python3
"""End-to-end test: loopback HTTP range server -> FUSE mount -> random reads."""
import contextlib
import http.server
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
import unittest

PAYLOAD = bytes((i * 37 + 11) % 256 for i in range(1024 * 1024 + 317))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_HEAD(self):
        self.respond(head_only=True)

    def do_GET(self):
        self.respond(head_only=False)

    def respond(self, head_only):
        if self.path != "/test.bin":
            self.send_error(404)
            return
        start, end = 0, len(PAYLOAD) - 1
        requested = self.headers.get("Range")
        if requested:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match:
                self.send_error(400)
                return
            start = int(match.group(1))
            end = min(int(match.group(2)), end) if match.group(2) else end
            if start >= len(PAYLOAD) or start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(PAYLOAD)}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        self.send_response(206 if requested else 200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        if requested:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(PAYLOAD)}")
        self.end_headers()
        if not head_only:
            self.wfile.write(PAYLOAD[start:end + 1])


class FuseIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not Path("/dev/fuse").exists():
            raise RuntimeError("/dev/fuse unavailable: integration test cannot run")
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.tmp = tempfile.TemporaryDirectory(prefix="curlhttpfs-test-")
        cls.mountpoint = Path(cls.tmp.name) / "mnt"
        cls.mountpoint.mkdir()
        port = cls.server.server_address[1]
        cls.proc = subprocess.Popen(
            [str(Path("curlhttpfs").resolve()), "-f", "-s",
             "-o", f"url=http://127.0.0.1:{port}/test.bin,filename=test.bin",
             str(cls.mountpoint)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if cls.proc.poll() is not None:
                raise RuntimeError(f"FUSE exited: {cls.proc.stderr.read().decode(errors='replace')}")
            if os.path.ismount(cls.mountpoint):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("FUSE mount timed out")

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "proc"):
            subprocess.run(["fusermount3", "-u", str(cls.mountpoint)], check=False)
            cls.proc.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired):
                cls.proc.wait(timeout=3)
            if cls.proc.poll() is None:
                cls.proc.kill()
                cls.proc.wait()
            cls.proc.stderr.close()
        if hasattr(cls, "server"):
            cls.server.shutdown()
            cls.server.server_close()
        if hasattr(cls, "tmp"):
            cls.tmp.cleanup()

    def test_file_metadata_and_directory(self):
        self.assertEqual(os.listdir(self.mountpoint), ["test.bin"])
        self.assertEqual((self.mountpoint / "test.bin").stat().st_size, len(PAYLOAD))

    def test_random_ranges_and_eof(self):
        with open(self.mountpoint / "test.bin", "rb", buffering=0) as f:
            for start, length in [(0, 1), (4093, 107), (65536, 2048),
                                  (len(PAYLOAD) - 13, 100), (123, 8192)]:
                f.seek(start)
                self.assertEqual(f.read(length), PAYLOAD[start:start + length])
            f.seek(len(PAYLOAD) + 5)
            self.assertEqual(f.read(8), b"")

    def test_sequential_read(self):
        with open(self.mountpoint / "test.bin", "rb") as f:
            self.assertEqual(f.read(), PAYLOAD)

    def test_read_only(self):
        with self.assertRaises(PermissionError):
            open(self.mountpoint / "test.bin", "wb")


if __name__ == "__main__":
    unittest.main(verbosity=2)
