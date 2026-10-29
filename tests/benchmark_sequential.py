#!/usr/bin/env python3
"""Run repeatable sequential-read timings against an already mounted curlhttpfs file.

Run the same workload with readahead=0 and readahead=1, using fresh cache
directories for cold-cache comparisons. This script intentionally makes no
speedup claim: the local HTTP server, kernel cache, and network affect results.
"""
import argparse
import json
import os
import statistics
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="Mounted remote file path")
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--chunk-kib", type=int, default=64)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if min(args.size_mib, args.chunk_kib, args.runs) <= 0:
        parser.error("all numeric values must be positive")
    limit = args.size_mib * 1024 * 1024
    block = args.chunk_kib * 1024
    durations = []
    amounts = []
    for _ in range(args.runs):
        done = 0
        started = time.monotonic()
        with open(args.path, "rb", buffering=0) as stream:
            while done < limit:
                data = stream.read(min(block, limit - done))
                if not data:
                    break
                done += len(data)
        durations.append(time.monotonic() - started)
        amounts.append(done)
    print(json.dumps({
        "path": os.path.abspath(args.path),
        "requested_mib": args.size_mib,
        "chunk_kib": args.chunk_kib,
        "runs": args.runs,
        "seconds": durations,
        "bytes": amounts,
        "median_seconds": statistics.median(durations),
        "median_mib_per_sec": statistics.median(
            [(b / 1048576) / max(t, 1e-9) for b, t in zip(amounts, durations)]
        ),
    }, indent=2))

if __name__ == "__main__":
    main()
