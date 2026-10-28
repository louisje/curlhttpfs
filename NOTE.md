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
