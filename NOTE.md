# NOTE — Legacy audit and modernization backlog

Last reviewed: 2026-10-28. This is a source-level inventory, **not** a successful build or runtime test.

## Goal and scope

curlhttpfs is intended to mount a remote large HTTP(S) file as a **read-only, on-demand** local file, without first downloading the complete file. Prioritize restoring a safe build and basic operation on current systems; defer feature additions and architectural rewrites. Preserve the existing C implementation where practical.

## Current repository state

- Default branch: `master`.
- At audit time, only `curlhttpfs.c` and `Makefile` exist at repository root; no Autotools inputs (`configure.ac`, `Makefile.am`) or README/tests were found.
- The Makefile hardcodes GCC, `/usr/include/fuse/`, `-lfuse`, `-lcurl`, and `-lpthread`. Its `mount` target contains a historical Java EE SDK download URL, which is unlikely to remain useful. Its `clean` target depends on unmounting and a nonstandard `trash` command.
- `curlhttpfs.c` defines `FUSE_USE_VERSION 26` (FUSE 2 API), uses libcurl easy handles, HEAD-like metadata requests, and byte-range GET requests. It exposes one file in a synthetic root directory.

## Priority 0 — correctness and safety, alongside dependency migration

- [ ] Bound the libcurl write callback to the caller-supplied buffer capacity; handle overflow and callback aborts. Currently a server ignoring `Range` or delivering too much body can overwrite memory.
- [ ] Check HTTP response status and `Content-Range`: for non-empty range reads, require appropriate `206 Partial Content` semantics, reject unexpected `200 OK` full-body responses, and handle `416` / EOF explicitly. Report the **actual** bytes received rather than the requested byte count.
- [ ] Restore default TLS certificate and hostname verification (currently `CURLOPT_SSL_VERIFYPEER=0` and `CURLOPT_SSL_VERIFYHOST=0`), including for metadata calls.
- [ ] Fix error-path cleanup (e.g., curl handles on failed metadata requests); translate network failures into meaningful filesystem errors.
- [ ] Enforce read-only opens (`O_RDONLY`), and verify behavior for attempts to write or access nonexistent paths.
- [ ] Avoid fixed-size path/name buffer overflow (`strcat` into `filename[512]`, URL copying/termination), and guard zero-length range requests (the current `size - 1` can underflow).
- [ ] Avoid mutable global metadata races where possible; ensure safe behavior for concurrent reads.
- [ ] Preserve proper timestamps if the server does not supply `Last-Modified`; validate unknown/missing `Content-Length`.

## Priority 1 — dependency and build modernization

- [ ] Migrate from FUSE 2 (`FUSE_USE_VERSION 26`) to libfuse 3, including `getattr`, `readdir`/filler and operation signatures, and use `pkg-config fuse3` for flags.
- [ ] Replace deprecated `CURLINFO_CONTENT_LENGTH_DOWNLOAD` with `CURLINFO_CONTENT_LENGTH_DOWNLOAD_T`; store file sizes as integral types (`curl_off_t` / `off_t`) rather than `double`. Validate libcurl's minimum supported version before setting requirements.
- [ ] Add `configure.ac` and `Makefile.am` (Autoconf/Automake), with compiler, libfuse3, libcurl and required thread checks through portable mechanisms (`pkg-config` where appropriate). Remove hardcoded include/library paths and nonportable Makefile commands.
- [ ] Document bootstrap/build/install/mount/unmount procedures and minimum dependencies; test clean out-of-tree builds if practical.
- [ ] Evaluate Linux first; assess macOS/macFUSE support separately. Windows would need a different filesystem frontend (e.g., WinFsp) and is **not** provided by Autotools alone.
- [ ] Review warnings on contemporary GCC/Clang with strict-but-practical warning flags. Do not claim compatibility until builds and mounts have passed.

## Priority 2 — automated verification

- [ ] Build a local HTTP fixture supporting `HEAD`, `Range`, redirects and controlled failures; avoid relying on the historical external URL.
- [ ] Test random/overlapping/sequential reads, small and large files, offsets beyond EOF, zero-byte reads, malformed/missing length headers, TLS failure, short responses and server ignoring ranges.
- [ ] Test concurrent reads, repeated mount/unmount and non-root execution; run under sanitizers where feasible.
- [ ] Consider CI matrix after establishing a passing Linux baseline.

## Deferred features (not part of first modernization)

- Persistent block cache and resumability.
- Read-ahead, parallel range requests, retries/backoff and offline behavior.
- Remote-object consistency using ETag / Last-Modified / conditional range requests.
- Broader platform integration and UX improvements.

## Proposed implementation sequence

1. Establish a reproducible Linux build and tests; move to FUSE 3 and current libcurl APIs.
2. Fix memory safety, HTTP response validation and TLS defaults **before** considering mounting arbitrary remote servers safe.
3. Integrate Autotools and document supported dependencies.
4. Add regression tests and CI, then revisit deferred features.

No code or build files were changed during this audit.

## Cross-platform porting roadmap (Linux, macOS, Windows)

**Target:** support all three desktop platforms while keeping one shared C-based HTTP Range / metadata / caching core. Platform-specific filesystem adapters should be thin and isolated. This is a **roadmap, not a claim of current support**.

- [ ] **Linux:** use libfuse 3 as the primary filesystem adapter. Verify builds, read-only mounts, random reads, error handling, and mount/unmount on supported distributions.
- [ ] **macOS:** investigate macFUSE's supported API and installation requirements; do not assume Linux libfuse 3 headers or binaries are directly portable. Implement compatibility glue or a dedicated adapter where required; validate on both Intel and Apple Silicon.
- [ ] **Windows:** implement a WinFsp-based adapter (or evaluate its FUSE compatibility layer first), using native Windows path conventions, mount semantics and error mappings. Do not assume Unix FUSE or Autotools is sufficient for Windows.
- [ ] **Shared core:** separate URL parsing, libcurl sessions, HTTP metadata / range reads and buffering from FUSE/WinFsp callbacks. Keep filesystem semantics read-only on every platform.
- [ ] **Build systems:** retain the user's desired Autotools path for Unix-like platforms, but evaluate a Windows-friendly build configuration (potentially CMake or Meson) for the WinFsp target. Avoid replacing the existing build workflow until a tested migration plan exists.
- [ ] **CI:** gradually extend build-only GitHub Actions coverage from Linux to `macos-latest` and `windows-latest`, installing platform dependencies as needed. Add actual mounted-filesystem integration tests only where runner permissions/drivers allow; distinguish build success from mount success.
- [ ] **Compatibility:** define tested minimum OS/driver/library versions, and document macFUSE/WinFsp installation and privileges. Do not promise completely driverless mounting.

**Order:** finish a green Linux build and basic regression tests; isolate shared core; bring up macOS; add Windows; validate each platform independently. New caching and performance features remain deferred.

## Audit status update (2026-10-28)

Since the original inventory above, commits have begun adapting the code to FUSE 3 and modern libcurl, have replaced the hardcoded Makefile dependencies with pkg-config, and added a Linux GitHub Actions build workflow for Ubuntu 22.04/24.04. These are initial changes only; CI success and runtime mount behavior are **not yet confirmed**. The unchecked findings above remain open until separately verified; historical observations describe the original source snapshot.

## CI integration tests (2026-10-28)

- Added `tests/test_mount.py`: a loopback-only, deterministic HTTP server with HEAD and byte-range GET, exercised through an actual FUSE mount.
- Cases: directory listing and file size, random byte ranges, full sequential read, EOF, and rejection of write opens.
- `.github/workflows/build.yml` runs the test on Ubuntu 22.04 and 24.04 for each push and pull request, after compilation; requires a working `/dev/fuse` and `fusermount3`.
- The integration job **fails explicitly** if the runner has no FUSE device; it is not silently skipped. Build success is not equivalent to mount success.
- This first suite does not yet cover TLS failures, malformed responses, concurrency, timeouts, or simulated interrupted downloads. CI passing must be verified from actual Actions results.

## Incremental test coverage (2026-10-28)

Added two runnable FUSE integration cases to `tests/test_mount.py`:
- `test_zero_length_read`: zero-byte read does not advance the offset, and subsequent normal read still returns the right bytes.
- `test_concurrent_random_reads`: 32 separate random 4 KiB reads over 8 worker threads, checking every returned byte against deterministic source data.

Both tests run automatically under the existing Linux Actions workflow; no new CI configuration is needed.

### Remaining test backlog (not implemented in this iteration)

- [ ] HTTP error behavior: server ignores `Range` and returns `200`; malformed/wrong `Content-Range`, `416`, redirect chains, missing or incorrect `Content-Length`.
- [ ] Interrupted/short HTTP responses, connection resets, retry policy, timeouts, and delayed responses.
- [ ] TLS hostname/certificate failures with a local HTTPS fixture.
- [ ] Remote content changes during a mounted session (ETag / Last-Modified consistency).
- [ ] Very large sparse/virtual objects and range bounds beyond 4 GiB.
- [ ] Multi-process concurrent reads, repeated mount/unmount, non-root access and sanitizer runs.
- [ ] macOS/Windows adapter builds and integration tests after those ports exist.

Do not mark any backlog item done merely because the current basic read tests pass.

## HTTP read reliability milestone (2026-10-29)

Implemented in `curlhttpfs.c`:
- HTTP byte-range capability/size probe using `GET Range: bytes=0-0`, requiring HTTP 206 with valid `Content-Range`; unsupported servers fail explicitly.
- Full read validation: matching `Content-Range` start/end/total, HTTP 206, and exact response length; reject incorrect or truncated replies rather than returning unverified bytes.
- Connection timeout 5 seconds, total per-request timeout 15 seconds; up to 3 attempts on connection/transfer timeouts and transient HTTP 429/500/502/503/504; exponential backoff (100 ms, 200 ms). Persistent protocol errors do not retry.
- No transparent persistence/cache has been added. libcurl default TLS verification remains enabled.
- Tests added: malformed `Content-Range` rejection, transient HTTP 503 recovery. These use a local fixture and real FUSE mount.

Known limitations/follow-up:
- [ ] Document configurable timeout/retry options instead of hardcoded defaults.
- [ ] Test socket resets, hanging connections and partial responses.
- [ ] Strengthen `Content-Range` parsing and rejection of duplicate conflicting headers; account for representation changes and ETag/If-Range.
- [ ] Add empty-file handling (a zero-byte object cannot satisfy `bytes=0-0` with 206).
- [ ] Audit metadata concurrency, FUSE error mapping and integer/size bounds.
- [ ] Verify this milestone's new tests on both Linux CI runners; do not infer pass status from prior commits.

## Empty files, interrupted transfers, and remote versions (2026-10-29)

- Implemented an empty-file fallback: a 416 response to the initial `bytes=0-0` probe is followed by a HEAD request; only an HTTP 200 with exact `Content-Length: 0` is accepted as an empty resource.
- For objects with an ETag, remember the probe's ETag, send `If-Match` on subsequent reads, reject HTTP 412 with `ESTALE`, and reject a 206 response whose ETag is absent or changed. This prevents silently combining different versions when a strong ETag is provided. ETag-less resources are **not yet** fully protected against same-size changes.
- Existing transient retry logic replays the entire range after an interrupted/partial transfer; a deterministic CI test now verifies that behavior.
- Added integration tests for empty objects, mid-transfer disconnection and ETag changes.

Remaining work:
- [ ] Support strong versus weak ETag distinctions; weak ETags cannot be used for reliable byte identity.
- [ ] Consider Last-Modified / If-Unmodified-Since fallback and metadata locking for concurrent first reads.
- [ ] Handle a HEAD-disabled empty-file server or unsupported HEAD with a separate policy.
- [ ] Harden unexpected HTTP encoding and oversized error response handling.
- [ ] Verify GitHub Actions Linux matrix for the above changes, including actual FUSE integration tests.

## First persistent cache milestone (2026-10-29)

- Added optional `-o cache_dir=/path/to/cache` for a persistent, on-disk, 64 KiB block cache. No cache is enabled by default.
- On each mount, the HTTP metadata probe obtains the current ETag. Only objects with a **strong quoted ETag** enter the persistent cache; unversioned/weak-ETag objects continue direct HTTP reads.
- Cache directory namespace is SHA-256 of URL and ETag; blocks are stored with atomic temporary-file rename, and checked for expected length before reuse. Successful downloads are reused across unmount/remount of the same URL and ETag.
- Each cache miss fetches the entire aligned block; the final block is smaller. A local HTTP fixture now checks the repeated mount uses only the metadata probe instead of redownloading its cached block.
- The initial implementation is intentionally minimal: no eviction, disk quota, encryption, offline mount, inter-process locking, metadata index, or configurable block size.
- Security note: cache files are local plaintext; use a private cache directory. Do not use an untrusted shared directory or assume existing cache data is cryptographically authenticated.

### Remaining cache engineering tasks

- [ ] Verify both Ubuntu Actions jobs pass, including remount-cache integration test.
- [ ] Replace deprecated OpenSSL SHA256 context functions with EVP digest API.
- [ ] Add tests for changed ETag across remount, weak or absent ETag bypass, truncated/corrupted cache files, and multi-process writers.
- [ ] Improve cache directory validation (symlinks, ownership, permissions), persist integrity metadata/hashes, safe eviction and capacity controls.
- [ ] Separate the caching core from the FUSE adapter before macOS/Windows ports.
- [ ] Coordinate concurrent readers/cache misses without serializing all reads; only the initial version uses one mutex for disk-cache operations.
- [ ] Consider Last-Modified fallback and behavior when a remote file changes during a mount.

## Cache integrity and concurrency improvement (2026-10-29)

- Implemented SHA-256 sidecar (`.sha256`, 32 raw bytes) for each persistent 64 KiB block. Reads validate both the exact length and digest; missing, truncated, or same-size corrupted blocks are fetched again. Legacy cache blocks without sidecars are refetched.
- Replaced the single cache mutex with 64 striped locks indexed by block number: different stripes may download concurrently, while the same block is deduplicated within a process.
- Serialized cache namespace initialization separately, and migrated namespace digest generation from deprecated SHA256 context routines to OpenSSL EVP.
- Added tests for same-size cache corruption and concurrent cached reads.
- Security / limitations: a sidecar hash detects accidental corruption only; it is **not authenticated** against malicious modification of both block and hash. Cache directory should be private. Multi-process coordination, atomic publication of block and sidecar as a pair, stronger filesystem durability, quota/LRU eviction, symlink-hardening, and lock contention benchmarks remain outstanding.
- Validate new work on Ubuntu 22.04 and 24.04 GitHub Actions; passing existing smoke tests is not sufficient.

## Bounded persistent cache and LRU (2026-10-29)

- Added optional FUSE mount option `cache_max_bytes=N`, used with `cache_dir=PATH`; `N` is a byte quota for **one URL + strong ETag cache namespace**. Example: `-o url=https://host/file.iso,cache_dir=/private/cache,cache_max_bytes=67108864` (64 MiB).
- When a successful block download is persisted, scan the current namespace and evict least-recently-used eligible blocks until their combined payload and SHA-256 sidecar sizes fit the quota. Cache hits refresh the payload timestamp; eviction removes both the block and its sidecar.
- The just-requested block is protected during its own prune pass; set the limit to at least 65568 bytes to fit a regular 64-KiB block and hash sidecar. `cache_max_bytes=0` means no quota, for backward compatibility.
- Added a FUSE integration test with a one-block quota, verifying eviction and reuse after remount.
- This first quota is **per object version**, not a global directory quota. Cache namespaces for older versions are not cleaned automatically; therefore total use of `cache_dir` can still grow unbounded across different URLs/ETags.
- Remaining: global quota across namespaces, multi-process safe eviction/locking, orphan temp/sidecar cleanup, directory permissions/symlink hardening, eviction benchmarks under concurrent access, and configurable policies. Timestamp-based LRU is approximate rather than a strict access-order index.

## Global cache quota (2026-10-29)

- Changed `cache_max_bytes=N` from a per-URL/ETag namespace budget to a **shared budget across every recognized cache namespace in `cache_dir`**. Each namespace remains keyed by URL and strong ETag, preserving existing block filenames and SHA-256 sidecars.
- Pruning scans all 64-character hex namespace directories and their 16-character hex block filenames, counts payload plus SHA-256 sidecar sizes, and evicts oldest blocks according to access-updated mtime until under budget. Directories belonging to other versions or URLs can now be reclaimed without deleting namespace directories.
- The in-flight block of the current request is excluded from its prune pass; blocks accessed by other processes are not protected.
- Added a GitHub Actions integration test using two remote URLs/ETags sharing one cache directory; it verifies cross-namespace eviction and refetch of evicted blocks.
- Important caveats: quota is **best effort**, enforced when new blocks finish downloading; multiple independent mounted processes do not coordinate quota, and temporary/orphan cache files are excluded. A quota smaller than one full block plus sidecar cannot be enforced strictly while the current block is protected. Remaining improvements include cross-process locking, startup cleanup/prune, namespace directory cleanup, symlink-safe access, and stricter global disk accounting.

## Shared cache process coordination and --help (2026-10-29)

- A root-level `.curlhttpfs.lock` file now coordinates participating curlhttpfs processes using `flock(LOCK_EX)` for cache reads, block publication, and global LRU pruning; the lock file is created with mode 0600 and no-follow semantics. This avoids two cooperating processes racing to overwrite or evict one another's blocks. Trade-off: disk-cache reads and downloads are serialized for processes using the same cache directory, even if they access different objects; optimize locking granularity later.
- Added native `--help` / `-h` descriptions for `url`, `filename`, `cache_dir`, and `cache_max_bytes`, with defaults, shared-quota behavior, HTTP timeout/retry notes, and a usage example. Previously only libfuse help was available and required an `url` option.
- Added automated tests asserting help output includes the supported options and two concurrent mount processes can read successfully while sharing a one-block quota.
- Pending work: concurrent-mount stress testing, robust symlink and cache-dir ownership validation, strict quota when multiple cache generations or temp files exist, stale temp cleanup, and finer-grained inter-process locks for throughput. `flock` is POSIX-specific; macOS/Windows filesystem frontends will require portability work.

## Per-block inter-process cache locks (2026-10-29)

- Replaced the former root-wide cache lock for every read with advisory `flock` files named `.lock-<16-hex-block-id>` within each URL/strong-ETag namespace. Two mounts can fetch **different blocks** concurrently. Matching blocks coordinate to avoid duplicate writes and validate the existing SHA-256 sidecar.
- Global `.curlhttpfs.lock` is retained for the short LRU quota pruning pass. Pruning tries to lock the candidate block non-blockingly and defers eviction if the candidate is in use. The existing in-process striped locks remain for FUSE worker threads.
- Added an integration test with two simultaneous FUSE mount processes using the same remote file and shared cache, each reading a different delayed HTTP range to detect unintended cross-process serialization.
- Limitations: best-effort quota can temporarily exceed the limit while blocks are busy; lock files persist and are not part of capacity accounting. `flock` is Unix-specific. Startup cleanup, orphan lock-file management, symlink/ownership hardening, and more extensive concurrency stress tests remain outstanding.
- Do not claim measured speedups without benchmarks; verify each Ubuntu Actions matrix job.

## Optional one-block background read-ahead (2026-10-29)

- Added `-o readahead=1` (default off) for mounts with `cache_dir` and a strong ETag. A dedicated background worker fetches at most one 64-KiB block ahead into the existing validated persistent cache.
- The first read at offset zero, or an immediately contiguous read, can schedule the next block. A random read starting elsewhere does not schedule speculative traffic. Only one background request can be pending or in flight.
- The worker uses existing HTTP timeout/retry, per-block interprocess locking, SHA-256 validation, and LRU quota code; completion is not required for the foreground read to succeed.
- Added integration tests for sequential prefetch and random-access bypass. `--help` includes the new option.
- Outstanding: real sequential-stream benchmarks, per-open-file rather than global sequence detection, cancellation/reprioritization, tuning of prefetch depth, adaptive disabling on slow networks, and shutdown behavior under a stalled HTTP request. Background requests may use additional bandwidth and cache quota.

## Per-open sequential detection and adaptive read-ahead (2026-10-29)

- Each FUSE open now creates a separate read-stream state stored in `fuse_file_info.fh`, destroyed by the release callback. Sequential-detection offsets and streak lengths no longer interfere between independent file handles.
- With `readahead=1` and a usable persistent cache, consecutive reads advance a bounded lookahead depth from 1 to 3 aligned 64 KiB blocks. Random seeks reset the stream's streak; speculative prefetch continues to be disabled by default.
- The existing single background worker processes up to the current requested lookahead depth, reusing the existing validated cache and cross-process block locks. Background work is skipped when an earlier prefetch is already queued/in flight.
- New regression tests verify independent read-stream detection across two open file descriptors and a three-block lookahead under repeated sequential reads.
- Remaining work: per-handle asynchronous scheduling/fairness rather than one worker, cancellation on seek/close, robust prefetch prioritization, true throughput benchmarking, and bounds on speculative bandwidth and cache churn. FUSE's own page cache/read-ahead can influence observed request shapes.

## Fair prefetch scheduling and baseline benchmark (2026-10-29)

- Replaced the single latest-wins prefetch slot with a bounded 16-request FIFO. Each open FUSE file gets a unique numeric stream ID. At most one queued request per stream is kept, and newer requests update that stream's position/depth without replacing other streams.
- The background worker fetches one 64 KiB block per turn, then requeues the remaining lookahead behind other pending streams (round-robin-like fairness). Closing a file removes its queued request. An in-flight HTTP request is not cancelled and may finish.
- Added `tests/benchmark_sequential.py`, a standalone reproducible timing tool for an already-mounted file. For meaningful comparisons: use equally sized *fresh* cache directories and the same HTTP source, file, chunk size and read amount for `readahead=0` and `readahead=1`. It reports JSON duration and throughput per run; do not treat unmeasured speedup as established.
- Example: `python3 tests/benchmark_sequential.py /mnt/http/file.iso --size-mib 16 --chunk-kib 64 --runs 3`.
- Known tradeoffs: one global worker still serializes HTTP speculative transfers; an active transfer may continue after file close; queue is bounded (busy streams can drop new requests); Linux kernel page caching changes read shapes and warm-cache iterations cannot be compared to cold-cache iterations. Benchmarks have been added but no production throughput conclusion is claimed.

## Bounded parallel HTTP Range prefetch (2026-10-29)

- Added `prefetch_workers=N` with allowed range 1–4 (default 1), effective when `readahead=1` and `cache_dir` are enabled. `--help` documents the option.
- Replaced the one-thread background prefetch engine with a bounded pthread pool. Each adaptive lookahead depth is split into independently queued 64-KiB block jobs, so multiple workers can issue concurrent HTTP Range requests even for one sequential stream.
- The existing FIFO, per-stream request coalescing, ETag verification, block-level `flock`, SHA-256 validation, and LRU quota remain in effect; foreground reads are still independent of prefetch success.
- Added tests that mount with three workers and check that delayed HTTP Range requests overlap in real time, not just that reads eventually succeed.
- Performance caveat: concurrency is bounded, but actual end-to-end speedup has **not** been established by a controlled benchmark. Multiple workers can increase network, disk, and server load; cache evictions may make speculative prefetch less useful. Worker count of 1 preserves prior behavior.

## Controlled worker-count benchmark (2026-10-29)

- Added `tests/benchmark_workers.py` to compare `prefetch_workers=1,2,3,4` against a loopback HTTP Range server with a controlled per-request delay. Every trial creates a fresh cache directory, mounts FUSE, reads the same data and verifies the SHA-256 output agrees across configurations.
- Added CI measurements on Ubuntu 22.04 and 24.04 with `--size-mib 1 --latency-ms 15 --runs 1`, uploaded as `benchmark-workers-<os>.json` artifacts; measurements are informational, not pass/fail speed thresholds.
- Re-run for stronger comparisons with `python3 tests/benchmark_workers.py --size-mib 16 --latency-ms 40 --runs 5 --output results.json`. Prefer several repetitions and a fixed environment; the workflow's one-run smoke benchmark is not sufficient to choose an optimal default.
- Caution: loopback latency and workload differ from real remote storage; file-system cache, network jitter, and delayed requests may affect results. Do not infer a real-world speedup from CI alone. Keep default worker count at 1 pending evidence.

## First worker-count results and wider matrix (2026-10-29)

- Actual measured loopback results from Actions run 40820736708, with 1 MiB file, 15 ms simulated HTTP Range latency, and **one trial per worker count**:
  - Ubuntu 22.04: workers 1/2/3/4 = 2.718 / 3.536 / 3.808 / 3.791 MiB/s.
  - Ubuntu 24.04: workers 1/2/3/4 = 2.849 / 3.631 / 4.017 / 3.918 MiB/s.
- Both single-run measurements favor 3 over 4, but the sample size is inadequate for choosing the production default or enabling runtime autotuning.
- Extend CI to two repeatable scenarios: 1 MiB at 15 ms, and 2 MiB at 40 ms; two fresh-cache repetitions per worker count. Each scenario exports an independent JSON artifact. Differences reflect simulated HTTP latency, not a guarantee of real-world storage performance.
- Keep `prefetch_workers` default = 1. Consider automatic adjustment only after larger repeated trials, server-side request-rate limits, bandwidth costs, cache hit rates, and real provider behavior have been characterized.

## HTTP rate-limit benchmarking and Retry-After (2026-10-29)

- Previous benchmark run 40821240582, 2 MiB per trial / 40 ms Range delay / two runs: Ubuntu 22.04 workers 1/2/3/4 = 1.270/2.044/2.167/2.146 MiB/s; Ubuntu 24.04 = 1.284/1.987/2.129/2.073 MiB/s. These figures are local simulator measurements, not provider benchmarks.
- `tests/benchmark_workers.py` now supports `--rate-limit=N` for a server-side one-second-window request cap. It emits HTTP 429 and `Retry-After: 1` when the block request rate is exceeded, and records `throttled_429` per trial. Probe requests are excluded from the simulated limit.
- HTTP Range retries now honor integer-second Retry-After on 429 responses (bounded to 5 seconds) instead of always using the short exponential delay. Other failure types retain existing backoff.
- CI adds a throttling scenario (1 MiB, 40 ms, 12 block requests per second, one trial per worker), alongside the existing latency matrix. Benchmark runs still use fresh caches; no automatic worker-count tuning is enabled.
- Outstanding: cross-process/global rate-limit coordination, variable/real provider rate caps, adaptive concurrency that reduces pressure after 429, and careful measurement of retries, latency, and aggregate requested bytes. The simulator's per-trial counter/window and small CI sample do not establish production behavior.

### Rate-limit benchmark result (Actions 40824563169)

- 1 MiB, 40 ms delay, 12 block requests/second, one trial/worker. Ubuntu 22.04 workers 1/2/3/4 throughput: 0.834/0.865/0.822/0.823 MiB/s; HTTP 429 counts: 4/7/13/13.
- Ubuntu 24.04 throughput: 0.737/0.866/0.833/0.825 MiB/s; 429 counts: 5/5/13/13.
- Under this artificial rate cap, worker count 2 was fastest in both single trials; increasing concurrency to 3–4 doubled or tripled 429s in some runs without improving throughput. This contrasts with the earlier unconstrained latency benchmarks (3 was best).
- Therefore keep static default=1; dynamic concurrency should be designed to respond to rate-limit signals rather than assume higher concurrency is universally better. Repeat trials before choosing production thresholds.

## Adaptive Concurrency — future design (2026-10-29)

- Keep `prefetch_workers=N` as the **configured upper bound**, not necessarily the active concurrency. Keep the current default 1 until real-provider measurements justify otherwise.
- Pass HTTP outcomes (especially 429, Retry-After, timeout and successful requests) from the request layer to the background scheduler without changing foreground read error semantics. Do not infer rate-limit behavior from throughput alone.
- Design a conservative additive-increase/multiplicative-decrease controller: reduce active speculative requests after 429, respect bounded Retry-After cooldown, increase only after a stable success window, and cap at the configured number.
- Coordinate cooldown and worker activity under scheduler locks, but do not hold a scheduler lock while doing HTTP I/O. Keep foreground reads prioritized over speculation, and avoid spinning or bursts under rate limits.
- Test under controlled rate caps, intermittent 429, high latency, low bandwidth, and multiple open streams; evaluate network cost, retransmissions, cache churn, and end-to-end throughput before enabling automatic adjustment.
- This is a **proposed future direction**, not implemented in the current release.

## Autotools integration — build system plan (2026-10-29)

- Migrate the maintained build entry point to `configure.ac` and `Makefile.am` using autoconf/automake and pkg-config checks for FUSE3, libcurl, and OpenSSL, plus POSIX pthread support.
- Validate `autoreconf -fi && ./configure && make && make check && make install DESTDIR=...` on the CI Linux matrix. Keep the runtime FUSE integration suite and benchmark scripts.
- Autotools portability does **not** imply that FUSE3, `flock`, `O_NOFOLLOW`, and Linux-specific runtime assumptions are immediately portable to every OS. Check capabilities explicitly and report unsupported targets.
