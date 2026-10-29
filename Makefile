CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3)
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl)
CRYPTO_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl)
CRYPTO_LIBS := $(shell $(PKG_CONFIG) --libs openssl)

.PHONY: all clean check-deps
all: curlhttpfs

check-deps:
	@$(PKG_CONFIG) --exists fuse3 || { echo "Missing dependency: fuse3 development files" >&2; exit 1; }
	@$(PKG_CONFIG) --exists openssl || { echo "Missing dependency: openssl development files" >&2; exit 1; }
	@$(PKG_CONFIG) --exists libcurl || { echo "Missing dependency: libcurl development files" >&2; exit 1; }

curlhttpfs: curlhttpfs.c | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -D_FILE_OFFSET_BITS=64 $(FUSE_CFLAGS) $(CURL_CFLAGS) $(CRYPTO_CFLAGS) $< -o $@ $(LDFLAGS) $(FUSE_LIBS) $(CURL_LIBS) $(CRYPTO_LIBS) $(LDLIBS)

clean:
	rm -f curlhttpfs
