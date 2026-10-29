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
        if self.path not in ("/test.bin", "/bad.bin", "/flaky.bin", "/empty.bin", "/changed.bin", "/interrupted.bin", "/cache.bin", "/cache2.bin"):
            self.send_error(404)
            return
        if self.path == "/empty.bin":
            if self.headers.get("Range"):
                self.send_response(416)
                self.send_header("Content-Range", "bytes */0")
            else:
                self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
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
        if self.path == "/changed.bin" and requested:
            with self.server.request_lock:
                self.server.changed_requests += 1
                changed = self.server.changed_requests > 1
            if changed and self.headers.get("If-Match") == '"v1"':
                self.send_response(412)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        if self.path == "/interrupted.bin" and requested:
            with self.server.request_lock:
                self.server.interrupted_requests += 1
                interrupted = self.server.interrupted_requests == 2
            if interrupted:
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(PAYLOAD)}")
                self.send_header("Content-Length", str(end - start + 1))
                self.end_headers()
                self.wfile.write(PAYLOAD[start:start + 8])
                self.wfile.flush()
                self.close_connection = True
                return
        if self.path == "/cache.bin" and requested:
            with self.server.request_lock:
                self.server.cache_requests += 1
        self.send_response(206 if requested else 200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        if self.path in ("/changed.bin", "/cache.bin", "/cache2.bin"):
            self.send_header("ETag", '"v2"' if self.path == "/cache2.bin" else '"v1"')
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
        cls.server.changed_requests = 0
        cls.server.interrupted_requests = 0
        cls.server.cache_requests = 0
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
    def temporary_mount(self, name, cache_dir=None, cache_max_bytes=None):
        mount = Path(self.tmp.name) / name
        mount.mkdir(exist_ok=True)
        port = self.server.server_address[1]
        opts = f"url=http://127.0.0.1:{port}/{name}.bin,filename=remote.bin"
        if cache_dir is not None:
            opts += f",cache_dir={cache_dir}"
        if cache_max_bytes is not None:
            opts += f",cache_max_bytes={cache_max_bytes}"
        proc = subprocess.Popen(
            [str(Path("curlhttpfs").resolve()), "-f", "-s",
             "-o", opts,
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

    def test_empty_file(self):
        with self.temporary_mount("empty") as path:
            self.assertEqual(path.stat().st_size, 0)
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(32), b"")

    def test_etag_change_is_rejected(self):
        with self.server.request_lock:
            self.server.changed_requests = 0
        with self.temporary_mount("changed") as path:
            self.assertEqual(path.stat().st_size, len(PAYLOAD))
            with self.assertRaises(OSError):
                with open(path, "rb", buffering=0) as f:
                    f.read(64)

    def test_recover_interrupted_transfer(self):
        with self.server.request_lock:
            self.server.interrupted_requests = 0
        with self.temporary_mount("interrupted") as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(512), PAYLOAD[:512])
        with self.server.request_lock:
            self.assertGreaterEqual(self.server.interrupted_requests, 3)

    def test_persistent_block_cache_after_remount(self):
        cache_dir = Path(self.tmp.name) / "cache-files"
        with self.server.request_lock:
            self.server.cache_requests = 0
        with self.temporary_mount("cache", cache_dir=cache_dir) as path:
            with open(path, "rb", buffering=0) as f:
                f.seek(13)
                self.assertEqual(f.read(100), PAYLOAD[13:113])
        with self.server.request_lock:
            first_requests = self.server.cache_requests
        self.assertGreaterEqual(first_requests, 2)  # range probe and first full block
        with self.temporary_mount("cache", cache_dir=cache_dir) as path:
            with open(path, "rb", buffering=0) as f:
                f.seek(13)
                self.assertEqual(f.read(100), PAYLOAD[13:113])
        with self.server.request_lock:
            self.assertEqual(self.server.cache_requests, first_requests + 1)  # probe only

    def test_corrupted_cached_block_is_refetched(self):
        cache_dir = Path(self.tmp.name) / "corruption-cache"
        with self.server.request_lock:
            self.server.cache_requests = 0
        with self.temporary_mount("cache", cache_dir=cache_dir) as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(128), PAYLOAD[:128])
        blocks = [p for p in cache_dir.rglob("*") if p.is_file() and
                  not p.name.endswith(".sha256") and not p.name.startswith(".")]
        self.assertEqual(len(blocks), 1)
        with open(blocks[0], "r+b") as f:
            f.seek(42)
            b = f.read(1)
            f.seek(42)
            f.write(bytes([b[0] ^ 0xff]))
        with self.server.request_lock:
            before = self.server.cache_requests
        with self.temporary_mount("cache", cache_dir=cache_dir) as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(128), PAYLOAD[:128])
        with self.server.request_lock:
            self.assertEqual(self.server.cache_requests, before + 2)

    def test_concurrent_cached_block_reads(self):
        cache_dir = Path(self.tmp.name) / "parallel-cache"
        with self.temporary_mount("cache", cache_dir=cache_dir) as path:
            offsets = [i * 65536 + 20 for i in range(8)]
            def read_at(offset):
                with open(path, "rb", buffering=0) as f:
                    f.seek(offset)
                    return f.read(256)
            with ThreadPoolExecutor(max_workers=8) as pool:
                actual = list(pool.map(read_at, offsets))
            self.assertEqual(actual, [PAYLOAD[i:i+256] for i in offsets])

    def test_cache_quota_evicts_oldest_block(self):
        cache_dir = Path(self.tmp.name) / "quota-cache"
        quota = 66000  # One 64-KiB block plus its SHA-256 sidecar
        with self.temporary_mount("cache", cache_dir=cache_dir,
                                  cache_max_bytes=quota) as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(100), PAYLOAD[:100])
                f.seek(65536 + 10)
                self.assertEqual(f.read(100), PAYLOAD[65546:65646])
            blocks = [p for p in cache_dir.rglob("*")
                      if p.is_file() and len(p.name) == 16]
            self.assertEqual(len(blocks), 1)
            self.assertEqual(blocks[0].name, "0000000000000001")
        with self.temporary_mount("cache", cache_dir=cache_dir,
                                  cache_max_bytes=quota) as path:
            with open(path, "rb", buffering=0) as f:
                f.seek(65536 + 10)
                self.assertEqual(f.read(100), PAYLOAD[65546:65646])

    def test_global_quota_across_urls_and_versions(self):
        cache_dir = Path(self.tmp.name) / "shared-global-cache"
        quota = 66000
        with self.temporary_mount("cache", cache_dir=cache_dir,
                                  cache_max_bytes=quota) as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(100), PAYLOAD[:100])
        with self.temporary_mount("cache2", cache_dir=cache_dir,
                                  cache_max_bytes=quota) as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(100), PAYLOAD[:100])
        namespaces = [p for p in cache_dir.iterdir() if p.is_dir()]
        self.assertEqual(len(namespaces), 2)
        blocks = [p for p in cache_dir.rglob("*")
                  if p.is_file() and len(p.name) == 16]
        self.assertEqual(len(blocks), 1)
        with self.server.request_lock:
            before = self.server.cache_requests
        with self.temporary_mount("cache", cache_dir=cache_dir,
                                  cache_max_bytes=quota) as path:
            with open(path, "rb", buffering=0) as f:
                self.assertEqual(f.read(100), PAYLOAD[:100])
        with self.server.request_lock:
            self.assertEqual(self.server.cache_requests, before + 2)

if __name__ == "__main__":
    unittest.main(verbosity=2)
