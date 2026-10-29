bin_PROGRAMS = curlhttpfs
curlhttpfs_SOURCES = curlhttpfs.c
curlhttpfs_CPPFLAGS = -D_FILE_OFFSET_BITS=64 $(FUSE3_CFLAGS) $(LIBCURL_CFLAGS) $(OPENSSL_CFLAGS)
curlhttpfs_CFLAGS = -Wall -Wextra
curlhttpfs_LDADD = $(FUSE3_LIBS) $(LIBCURL_LIBS) $(OPENSSL_LIBS)

EXTRA_DIST = NOTE.md tests/test_mount.py tests/benchmark_sequential.py tests/benchmark_workers.py

.PHONY: check-integration
check-integration: curlhttpfs
	python3 -m unittest discover -s $(srcdir)/tests -p 'test_*.py' -v

check-local: check-integration
