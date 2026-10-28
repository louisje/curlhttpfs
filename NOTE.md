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
