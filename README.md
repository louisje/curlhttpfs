# curlhttpfs

Read-only FUSE3 mount of a remote HTTP(S) object, with HTTP Range reads,
validated persistent 64 KiB block caching and optional background prefetch.

## Build from a Git checkout

Dependencies: C compiler, GNU Autoconf, Automake, pkg-config, FUSE3
development files, libcurl development files, OpenSSL development files,
POSIX threads and an OS with `flock`.

```sh
autoreconf -fi
./configure --prefix=/usr/local
make -j2
make check
sudo make install
```

Alternatively, use `make install DESTDIR=/tmp/curlhttpfs-stage` to stage a
package without installing it system-wide. Release tarballs generated with
`make dist` include the generated `configure` script, so downstream builders
should only need `./configure && make && make install`.

## Mount a file

```sh
mkdir -p ~/remote ~/curlhttpfs-cache
curlhttpfs -o url=https://example.org/file.iso,cache_dir=$HOME/curlhttpfs-cache,readahead=1,prefetch_workers=2 ~/remote
ls -l ~/remote
fusermount3 -u ~/remote
```

Use `curlhttpfs --help` for mount options. The shared cache budget may be
set with `cache_max_bytes=N`; zero means unlimited.

## Platform status

Autotools makes configure/build checks more portable; it does **not**
automatically port FUSE or system-level APIs. The current source targets
POSIX systems with FUSE3, pthreads, `flock`, and `O_NOFOLLOW`. CI tests
Ubuntu 22.04 and Ubuntu 24.04. macOS or Windows need compatible FUSE
headers/libraries, supported system calls and additional verification.
`./configure` should fail if required development libraries or POSIX
capabilities are absent.

## Tests and performance

`make check` executes the loopback HTTP + real FUSE integration suite;
`/dev/fuse` must be available. CI also runs
`tests/benchmark_workers.py` for reproducible loopback comparisons,
not production cloud performance claims.

See [NOTE.md](NOTE.md) for design notes, limitations, and future work.
