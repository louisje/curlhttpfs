#!/usr/bin/env python3
"""End-to-end test: loopback HTTP range server -> FUSE mount -> random reads."""
import contextlib
from concurrent.futures import ThreadPoolExecutor
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
        if self.path not in ("/test.bin", "/bad.bin", "/flaky.bin"):
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
        if self.path == "/flaky.bin" and requested:
            with self.server.request_lock:
                self.server.flaky_attempts += 1
                if self.server.flaky_attempts == 1:
                    self.send_response(503)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
        self.send_response(206 if requested else 200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        if requested:
            self.send_header("Content-Range", f"bytes {start + (1 if self.path == '/bad.bin' else 0)}-{end}/{len(PAYLOAD)}")
        self.end_headers()
        if not head_only:
            self.wfile.write(PAYLOAD[start:end + 1])


class FuseIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not Path("/dev/fuse").exists():
            raise RuntimeError("/dev/fuse unavailable: integration test cannot run")
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.request_lock = threading.Lock()
        cls.server.flaky_attempts = 0
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


    def test_zero_length_read(self):
        with open(self.mountpoint / "test.bin", "rb", buffering=0) as f:
            f.seek(4096)
            self.assertEqual(f.read(0), b"")
            self.assertEqual(f.tell(), 4096)
            self.assertEqual(f.read(32), PAYLOAD[4096:4128])

    def test_concurrent_random_reads(self):
        offsets = [(i * 7919) % (len(PAYLOAD) - 4096) for i in range(32)]

        def read_at(start):
            with open(self.mountpoint / "test.bin", "rb", buffering=0) as f:
                f.seek(start)
                return f.read(4096)

        with ThreadPoolExecutor(max_workers=8) as pool:
            received = list(pool.map(read_at, offsets))
        for start, data in zip(offsets, received):
            self.assertEqual(data, PAYLOAD[start:start + 4096])

    @contextlib.contextmanager
    def temporary_mount(self, name):
        mount = Path(self.tmp.name) / name
        mount.mkdir()
        port = self.server.server_address[1]
        proc = subprocess.Popen(
            [str(Path("curlhttpfs").resolve()), "-f", "-s",
             "-o", f"url=http://127.0.0.1:{port}/{name}.bin,filename=remote.bin",
             str(mount)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 12
            while not os.path.ismount(mount):
                if proc.poll() is not None:
                    self.fail(f"mount failed: {proc.stderr.read().decode(errors='replace')}")
                if time.monotonic() > deadline:
                    self.fail("mount timeout")
                time.sleep(0.1)
            yield mount / "remote.bin"
        finally:
            if os.path.ismount(mount):
                subprocess.run(["fusermount3", "-u", str(mount)], check=True)
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stderr.close()

    def test_reject_incorrect_content_range(self):
        with self.temporary_mount("bad") as path:
            with self.assertRaises(OSError):
                with open(path, "rb", buffering=0) as f:
                    f.read(64)

    def test_retry_transient_503(self):
        with self.server.request_lock:
            self.server.flaky_attempts = 0
        with self.temporary_mount("flaky") as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(256), PAYLOAD[:256])
        with self.server.request_lock:
            self.assertGreaterEqual(self.server.flaky_attempts, 2)

if __name__ == "__main__":
    unittest.main(verbosity=2)
