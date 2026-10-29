#!/usr/bin/env python3
"""Reproducible loopback HTTP Range benchmark for curlhttpfs worker counts.

The test measures cold cache and reports measurements, not a guaranteed
performance improvement. Requires Linux FUSE3, built ./curlhttpfs and fusermount3.
"""
import argparse
import contextlib
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import threading
import time

BLOCK = 65536

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_args):
        pass
    def do_GET(self):
        match = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
        if not match:
            self.send_error(400)
            return
        start, end = map(int, match.groups())
        n = self.server.size
        if start >= n or end >= n or start > end:
            self.send_error(416)
            return
        if end - start + 1 != 1:
            time.sleep(self.server.latency)
        self.send_response(206)
        self.send_header("ETag", '"benchmark-v1"')
        self.send_header("Content-Range", f"bytes {start}-{end}/{n}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self.wfile.write(bytes(((i * 37 + 11) % 256) for i in range(start, end + 1)))

def mounted(binary, mount, url, cache, workers):
    opts = f"url={url},filename=bench.bin,cache_dir={cache},readahead=1,prefetch_workers={workers}"
    return subprocess.Popen([str(binary), "-f", "-s", "-o", opts, str(mount)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

def run(binary, server, size, workers, latency):
    with tempfile.TemporaryDirectory(prefix="curlhttpfs-bench-") as tmp:
        mount = Path(tmp) / "mnt"
        mount.mkdir()
        cache = Path(tmp) / "cache"
        url = f"http://127.0.0.1:{server.server_address[1]}/benchmark.bin"
        proc = mounted(binary, mount, url, cache, workers)
        try:
            deadline = time.monotonic() + 12
            while not os.path.ismount(mount):
                if proc.poll() is not None:
                    raise RuntimeError(proc.stderr.read().decode(errors="replace"))
                if time.monotonic() > deadline:
                    raise RuntimeError("FUSE mount timed out")
                time.sleep(0.03)
            h = hashlib.sha256()
            began = time.monotonic()
            with open(mount / "bench.bin", "rb", buffering=0) as file:
                for _ in range(size // BLOCK):
                    data = file.read(BLOCK)
                    if len(data) != BLOCK:
                        raise RuntimeError("Unexpected short read")
                    h.update(data)
            elapsed = time.monotonic() - began
            return {"workers": workers, "seconds": elapsed,
                    "mib_per_sec": (size / 1048576) / elapsed,
                    "sha256": h.hexdigest(), "latency_seconds": latency}
        finally:
            if os.path.ismount(mount):
                subprocess.run(["fusermount3", "-u", str(mount)], check=True)
            proc.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired):
                proc.wait(timeout=3)
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            proc.stderr.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size-mib", type=int, default=2)
    parser.add_argument("--latency-ms", type=float, default=20)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--output", default=None)
    parser.add_argument("--binary", default="./curlhttpfs")
    args = parser.parse_args()
    if args.size_mib < 1 or args.runs < 1 or args.latency_ms < 0:
        parser.error("size and runs must be positive, latency nonnegative")
    binary = Path(args.binary).resolve()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.size = args.size_mib * 1048576
    server.latency = args.latency_ms / 1000
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    samples = []
    try:
        for _ in range(args.runs):
            # Rotate order to reduce systematic timing bias.
            for workers in (1, 2, 3, 4) if len(samples) < 4 else (4, 3, 2, 1):
                samples.append(run(binary, server, server.size, workers, server.latency))
    finally:
        server.shutdown()
        server.server_close()
    hashes = {x["sha256"] for x in samples}
    if len(hashes) != 1:
        raise RuntimeError("Content differs between worker configurations")
    summary = {
        "environment": "loopback HTTP with fixed per-range delay; cold disk cache per trial",
        "size_mib": args.size_mib, "latency_ms": args.latency_ms,
        "runs_per_worker": args.runs, "samples": samples,
        "median_mib_per_sec": {str(i): statistics.median(
            x["mib_per_sec"] for x in samples if x["workers"] == i
        ) for i in (1, 2, 3, 4)}
    }
    rendered = json.dumps(summary, indent=2)
    print(rendered)
    if args.output:
        Path(args.output).write_text(rendered + "\n")

if __name__ == "__main__":
    main()
