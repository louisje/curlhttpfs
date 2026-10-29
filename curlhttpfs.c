/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` hello.c -o hello
*/

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <fuse_opt.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <curl/curl.h>
#include <curl/easy.h>
#include <stdlib.h>
#include <stddef.h>
#include <libgen.h>
#include <pthread.h>
#include <inttypes.h>
#include <limits.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <sys/file.h>
#include <openssl/evp.h>

struct options {
	char *target_url;
	char *target_filename;
	char *cache_dir;
	unsigned long long cache_max_bytes;
	int readahead;
} options;

#define MY_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

static struct fuse_opt httpfs_opts[] = {
	MY_OPT_KEY("url=%s", target_url, 0),
	MY_OPT_KEY("filename=%s", target_filename, 0),
	MY_OPT_KEY("cache_dir=%s", cache_dir, 0),
	MY_OPT_KEY("cache_max_bytes=%llu", cache_max_bytes, 0),
	MY_OPT_KEY("readahead=%d", readahead, 0),
	FUSE_OPT_END
};

size_t my_write_callback(void *ptr, size_t size, size_t nmemb, void *stream);

struct httpfs_file {
	char filename[512];
	curl_off_t size;
	long filetime;
	int metadata_valid;
	char etag[256];
} httpfs_file = {
	"/",
	0,
	0,
	0,
	{0}
};

typedef struct httpfs_buffer {
	char *p;
	size_t len;
	size_t capacity;
} httpfs_buffer_t;

/* HTTP requests have bounded duration and only transient failures are retried. */
#define HTTP_TIMEOUT_MS 15000L
#define HTTP_CONNECT_TIMEOUT_MS 5000L
#define HTTP_RETRIES 3

static void configure_curl(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_URL, options.target_url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_TIMEOUT_MS);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
}

static int retryable(CURLcode result, long status)
{
	if (result == CURLE_OPERATION_TIMEDOUT || result == CURLE_COULDNT_CONNECT ||
	    result == CURLE_RECV_ERROR || result == CURLE_SEND_ERROR ||
	    result == CURLE_PARTIAL_FILE)
		return 1;
	return result == CURLE_OK && (status == 429 || status == 500 ||
	                             status == 502 || status == 503 || status == 504);
}

/* A range probe works even when HEAD/Accept-Ranges is unsupported or misleading. */
static size_t probe_body(void *ptr, size_t size, size_t count, void *unused)
{
	(void)ptr; (void)unused;
	return size * count;
}

typedef struct response_headers {
	int seen;
	char etag[256];
	unsigned long long start, end, total;
} response_headers_t;

static size_t parse_range_header(char *line, size_t size, size_t count, void *userdata)
{
	response_headers_t *headers = userdata;
	size_t n = size * count;
	unsigned long long a, b, c;
	char extra;
	if (n >= 5 && strncmp(line, "HTTP/", 5) == 0) {
		headers->seen = 0;
		headers->etag[0] = '\0';
	}
	if (n >= 5 && strncasecmp(line, "ETag:", 5) == 0) {
		size_t begin = 5, end = n;
		while (begin < end && isspace((unsigned char)line[begin])) ++begin;
		while (end > begin && isspace((unsigned char)line[end - 1])) --end;
		if (end > begin && end - begin < sizeof(headers->etag)) {
			memcpy(headers->etag, line + begin, end - begin);
			headers->etag[end - begin] = '\0';
		}
	}
	if (n >= 14 && strncasecmp(line, "Content-Range:", 14) == 0) {
		if (sscanf(line + 14, " bytes %llu-%llu/%llu %c", &a, &b, &c, &extra) == 3) {
			headers->seen = 1;
			headers->start = a; headers->end = b; headers->total = c;
		} else {
			headers->seen = -1;
		}
	}
	return n;
}

static int fetch_metadata(void)
{
	for (int attempt = 0; attempt < HTTP_RETRIES; ++attempt) {
		CURL *curl = curl_easy_init();
		CURLcode code;
		long status = 0;
		response_headers_t headers = {0};
		if (!curl) return -ENOMEM;
		configure_curl(curl);
		curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_body);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, parse_range_header);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
		curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)1);
		code = curl_easy_perform(curl);
		if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(curl);
		if (code == CURLE_OK && status == 206 && headers.seen == 1 &&
		    headers.start == 0 && headers.end == 0 && headers.total > 0 &&
		    headers.total <= (unsigned long long)LLONG_MAX) {
			httpfs_file.size = (curl_off_t)headers.total;
			strcpy(httpfs_file.etag, headers.etag);
			httpfs_file.metadata_valid = 1;
			return 0;
		}
		if (code == CURLE_OK && status == 416 && headers.seen != 1) {
			/* Verify a zero-byte resource using HEAD rather than trusting 416 alone. */
			CURL *head = curl_easy_init();
			if (!head) return -ENOMEM;
			configure_curl(head);
			curl_easy_setopt(head, CURLOPT_NOBODY, 1L);
			CURLcode head_code = curl_easy_perform(head);
			long head_status = 0;
			curl_off_t length = -1;
			if (head_code == CURLE_OK) {
				curl_easy_getinfo(head, CURLINFO_RESPONSE_CODE, &head_status);
				curl_easy_getinfo(head, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
			}
			curl_easy_cleanup(head);
			if (head_code == CURLE_OK && head_status == 200 && length == 0) {
				httpfs_file.size = 0;
				httpfs_file.metadata_valid = 1;
				return 0;
			}
		}
		if (!retryable(code, status)) return -EIO;
		if (attempt + 1 < HTTP_RETRIES) usleep((useconds_t)(100000U << attempt));
	}
	return -EIO;
}

static int httpfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	(void)fi;
	memset(stbuf, 0, sizeof(*stbuf));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, httpfs_file.filename)) return -ENOENT;
	if (!httpfs_file.metadata_valid) {
		int result = fetch_metadata();
		if (result != 0) return result;
	}
	stbuf->st_mode = S_IFREG | 0444;
	stbuf->st_nlink = 1;
	stbuf->st_size = (off_t)httpfs_file.size;
	return 0;
}

static int httpfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	(void) flags;
	(void) offset;
	(void) fi;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, httpfs_file.filename + 1, NULL, 0, 0);

	return 0;
}

typedef struct read_stream { pthread_mutex_t lock; off_t end; unsigned int streak; } read_stream_t;

static int httpfs_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, httpfs_file.filename) != 0)
	{
		return -ENOENT;
	}
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;
 	read_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) return -ENOMEM;
	pthread_mutex_init(&stream->lock, NULL);
	stream->end = -1;
	fi->fh = (uint64_t)(uintptr_t)stream;
	return 0;
}

static int httpfs_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	read_stream_t *stream = (read_stream_t *)(uintptr_t)fi->fh;
	if (stream) {
		pthread_mutex_destroy(&stream->lock);
		free(stream);
		fi->fh = 0;
	}
	return 0;
}

size_t my_write_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	size_t total_size = size * nmemb;
	struct httpfs_buffer *httpfs_buf = (struct httpfs_buffer *)stream;

	if (total_size > httpfs_buf->capacity - httpfs_buf->len)
		return 0;
	memcpy(httpfs_buf->p + httpfs_buf->len, ptr, total_size);
	httpfs_buf->len += total_size;

	return total_size;
}

static int read_curl_buffer(size_t size, off_t offset, httpfs_buffer_t *buffer)
{
	char range[100];
	unsigned long long start = (unsigned long long)offset;
	unsigned long long end = start + size - 1;
	snprintf(range, sizeof(range), "%llu-%llu", start, end);
	for (int attempt = 0; attempt < HTTP_RETRIES; ++attempt) {
		CURL *curl = curl_easy_init();
		CURLcode code;
		long status = 0;
		response_headers_t headers = {0};
		if (!curl) return -ENOMEM;
		buffer->len = 0;
		configure_curl(curl);
		curl_easy_setopt(curl, CURLOPT_RANGE, range);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, parse_range_header);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
		struct curl_slist *conditions = NULL;
		if (httpfs_file.etag[0]) {
			char conditional[sizeof(httpfs_file.etag) + 16];
			snprintf(conditional, sizeof(conditional), "If-Match: %s", httpfs_file.etag);
			conditions = curl_slist_append(NULL, conditional);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, conditions);
		}
		code = curl_easy_perform(curl);
		if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_slist_free_all(conditions);
		curl_easy_cleanup(curl);
		if (status == 412) return -ESTALE;
		if (code == CURLE_OK && status == 206 && headers.seen == 1 &&
		    (!httpfs_file.etag[0] || (headers.etag[0] && strcmp(headers.etag, httpfs_file.etag) == 0)) &&
		    headers.start == start && headers.end == end &&
		    headers.total == (unsigned long long)httpfs_file.size &&
		    buffer->len == size)
			return (int)buffer->len;
		if (!retryable(code, status)) return -EIO;
		if (attempt + 1 < HTTP_RETRIES) usleep((useconds_t)(100000U << attempt));
	}
	return -EIO;
}

/* Optional 64-KiB on-disk block cache, enabled only for strong ETags.
 * Each mount probes the remote object before using any cached blocks. */
#define CACHE_BLOCK_SIZE 65536U
#define CACHE_LOCK_STRIPES 64
static pthread_mutex_t cache_locks[CACHE_LOCK_STRIPES];
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t cache_init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cache_prune_lock = PTHREAD_MUTEX_INITIALIZER;
static void init_cache_locks(void)
{
	for (size_t i = 0; i < CACHE_LOCK_STRIPES; ++i)
		pthread_mutex_init(&cache_locks[i], NULL);
}
static char cache_namespace[1024];
static int cache_lock_fd = -1;
static pthread_mutex_t cache_process_lock = PTHREAD_MUTEX_INITIALIZER;

/* The root lock serializes cache readers/writers and global pruning across
 * processes sharing a directory. Other HTTP mounts do not need this lock. */
static int cache_global_lock(void)
{
	if (cache_lock_fd < 0) return -EIO;
	pthread_mutex_lock(&cache_process_lock);
	while (flock(cache_lock_fd, LOCK_EX) < 0) {
		if (errno != EINTR) { int error = -errno; pthread_mutex_unlock(&cache_process_lock); return error; }
	}
	return 0;
}

static void cache_global_unlock(void)
{
	if (cache_lock_fd >= 0) {
		flock(cache_lock_fd, LOCK_UN);
		pthread_mutex_unlock(&cache_process_lock);
	}
}


static int cache_init(void)
{
	unsigned char digest[32];
	EVP_MD_CTX *ctx;
	char hex[32 * 2 + 1];
	if (!options.cache_dir || !options.cache_dir[0] ||
	    httpfs_file.etag[0] != '"' ) return 0;
	if (mkdir(options.cache_dir, 0700) != 0 && errno != EEXIST) return -errno;
	if (cache_lock_fd < 0) {
		char lock_path[1200];
		if (snprintf(lock_path, sizeof(lock_path), "%s/.curlhttpfs.lock", options.cache_dir) >= (int)sizeof(lock_path)) return -ENAMETOOLONG;
		cache_lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (cache_lock_fd < 0) return -errno;
	}
	ctx = EVP_MD_CTX_new();
	if (!ctx) return -ENOMEM;
	int hashed = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
	    EVP_DigestUpdate(ctx, options.target_url, strlen(options.target_url)) == 1 &&
	    EVP_DigestUpdate(ctx, "\0", 1) == 1 &&
	    EVP_DigestUpdate(ctx, httpfs_file.etag, strlen(httpfs_file.etag)) == 1 &&
	    EVP_DigestFinal_ex(ctx, digest, NULL) == 1;
	EVP_MD_CTX_free(ctx);
	if (!hashed) return -EIO;
	for (size_t i = 0; i < sizeof(digest); ++i)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	if (snprintf(cache_namespace, sizeof(cache_namespace), "%s/%s",
	             options.cache_dir, hex) >= (int)sizeof(cache_namespace)) return -ENAMETOOLONG;
	if (mkdir(cache_namespace, 0700) != 0 && errno != EEXIST) return -errno;
	return 0;
}

static int digest_block(const char *data, size_t length, unsigned char digest[32])
{
	unsigned int n = 0;
	return EVP_Digest(data, length, digest, &n, EVP_sha256(), NULL) == 1 && n == 32;
}

/* Global cache quota across all URL/ETag namespaces. Only fixed-format
 * block files with existing SHA sidecars are considered. */
static int is_hex_name(const char *name, size_t width)
{
	if (strlen(name) != width) return 0;
	for (size_t i = 0; i < width; ++i)
		if (!isxdigit((unsigned char)name[i])) return 0;
	return 1;
}

static void cache_touch(const char *path)
{
	struct timespec now[2];
	if (clock_gettime(CLOCK_REALTIME, &now[0]) == 0) {
		now[1] = now[0];
		utimensat(AT_FDCWD, path, now, 0);
	}
}

/* The quota is best-effort between independent processes. */
static int block_file_lock(const char *directory, unsigned long long block, int nonblock);

static void cache_prune(unsigned long long protected_block)
{
	if (!options.cache_max_bytes || !options.cache_dir || !cache_namespace[0]) return;
	pthread_mutex_lock(&cache_prune_lock);
	if (cache_global_lock() != 0) { pthread_mutex_unlock(&cache_prune_lock); return; }
	for (;;) {
		DIR *root = opendir(options.cache_dir);
		if (!root) break;
		unsigned long long used = 0, oldest_block = 0;
		char oldest_dir[1024] = {0};
		struct timespec oldest = {0};
		int candidate = 0;
		struct dirent *ns;
		while ((ns = readdir(root)) != NULL) {
			if (!is_hex_name(ns->d_name, 64)) continue;
			char directory[1024];
			struct stat ds;
			if (snprintf(directory, sizeof(directory), "%s/%s", options.cache_dir, ns->d_name) >= (int)sizeof(directory) ||
			    lstat(directory, &ds) != 0 || !S_ISDIR(ds.st_mode)) continue;
			DIR *dir = opendir(directory);
			if (!dir) continue;
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL) {
				if (!is_hex_name(entry->d_name, 16)) continue;
				char path[1200], hash_path[1220];
				struct stat st, hs;
				if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path) ||
				    snprintf(hash_path, sizeof(hash_path), "%s.sha256", path) >= (int)sizeof(hash_path) ||
				    lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
				    lstat(hash_path, &hs) != 0 || !S_ISREG(hs.st_mode)) continue;
				unsigned long long bytes = (unsigned long long)st.st_size + (unsigned long long)hs.st_size;
				if (ULLONG_MAX - used < bytes) used = ULLONG_MAX;
				else used += bytes;
				unsigned long long block = strtoull(entry->d_name, NULL, 16);
				if (block == protected_block && strcmp(directory, cache_namespace) == 0) continue;
				if (!candidate || st.st_mtim.tv_sec < oldest.tv_sec ||
				    (st.st_mtim.tv_sec == oldest.tv_sec && st.st_mtim.tv_nsec < oldest.tv_nsec)) {
					candidate = 1;
					oldest = st.st_mtim;
					oldest_block = block;
					strcpy(oldest_dir, directory);
				}
			}
			closedir(dir);
		}
		closedir(root);
		if (used <= options.cache_max_bytes || !candidate) break;
		pthread_once(&cache_once, init_cache_locks);
		pthread_mutex_t *lock = &cache_locks[oldest_block % CACHE_LOCK_STRIPES];
		pthread_mutex_lock(lock);
		int candidate_fd = block_file_lock(oldest_dir, oldest_block, 1);
		if (candidate_fd < 0) {
			pthread_mutex_unlock(lock);
			break; /* busy block: defer quota enforcement */
		}
		char path[1200], hash_path[1220];
		if (snprintf(path, sizeof(path), "%s/%016llx", oldest_dir, oldest_block) < (int)sizeof(path) &&
		    snprintf(hash_path, sizeof(hash_path), "%s.sha256", path) < (int)sizeof(hash_path)) {
			unlink(path);
			unlink(hash_path);
		}
		flock(candidate_fd, LOCK_UN);
		close(candidate_fd);
		pthread_mutex_unlock(lock);
	}
	cache_global_unlock();
	pthread_mutex_unlock(&cache_prune_lock);
}

/* Advisory per-block file locks coordinate different mount processes.
 * The global lock only covers the short quota-pruning pass. */
static int block_file_lock(const char *directory, unsigned long long block, int nonblock)
{
	char path[1200];
	if (snprintf(path, sizeof(path), "%s/.lock-%016llx", directory, block) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return -errno;
	while (flock(fd, LOCK_EX | (nonblock ? LOCK_NB : 0)) < 0) {
		if (errno == EINTR) continue;
		int err = -errno;
		close(fd);
		return err;
	}
	return fd;
}

static int cache_block(char *dst, unsigned long long block)
{
	char path[1200], hash_path[1220], tmp[1250], hash_tmp[1250];
	unsigned long long offset = block * CACHE_BLOCK_SIZE;
	size_t length = (size_t)((unsigned long long)httpfs_file.size - offset);
	if (length > CACHE_BLOCK_SIZE) length = CACHE_BLOCK_SIZE;
	if (snprintf(path, sizeof(path), "%s/%016llx", cache_namespace, block) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	if (snprintf(hash_path, sizeof(hash_path), "%s.sha256", path) >= (int)sizeof(hash_path))
		return -ENAMETOOLONG;
	pthread_once(&cache_once, init_cache_locks);
	pthread_mutex_t *lock = &cache_locks[block % CACHE_LOCK_STRIPES];
	pthread_mutex_lock(lock);
	int block_fd = block_file_lock(cache_namespace, block, 0);
	if (block_fd < 0) { pthread_mutex_unlock(lock); return block_fd; }
	FILE *file = fopen(path, "rb");
	if (file) {
		size_t got = fread(dst, 1, length, file);
		int extra = fgetc(file);
		fclose(file);
		if (got == length && extra == EOF) {
			unsigned char expected[32], actual[32];
			FILE *hash = fopen(hash_path, "rb");
			if (hash) {
				size_t h = fread(expected, 1, sizeof(expected), hash);
				int trailing = fgetc(hash);
				fclose(hash);
				if (h == 32 && trailing == EOF &&
				    digest_block(dst, length, actual) &&
				    memcmp(actual, expected, 32) == 0) {
					cache_touch(path);
					flock(block_fd, LOCK_UN);
					close(block_fd);
					pthread_mutex_unlock(lock);
					return (int)length;
				}
			}
		}
	}
	/* Missing, truncated or corrupted blocks are fetched again. */
	httpfs_buffer_t buffer = {dst, 0, length};
	int result = read_curl_buffer(length, (off_t)offset, &buffer);
	if (result == (int)length) {
		unsigned char digest[32];
		if (digest_block(dst, length, digest) &&
		    snprintf(tmp, sizeof(tmp), "%s/.tmp-XXXXXX", cache_namespace) < (int)sizeof(tmp) &&
		    snprintf(hash_tmp, sizeof(hash_tmp), "%s/.hash-XXXXXX", cache_namespace) < (int)sizeof(hash_tmp)) {
			int fd = mkstemp(tmp);
			if (fd >= 0) {
				size_t written = 0;
				while (written < length) {
					ssize_t n = write(fd, dst + written, length - written);
					if (n <= 0) break;
					written += (size_t)n;
				}
				int persisted = written == length && fsync(fd) == 0;
				if (close(fd) != 0) persisted = 0;
				if (persisted) {
					int hd = mkstemp(hash_tmp);
					if (hd >= 0) {
						int hash_ok = write(hd, digest, 32) == 32 && fsync(hd) == 0;
						if (close(hd) != 0) hash_ok = 0;
						if (hash_ok && rename(tmp, path) == 0) {
							if (rename(hash_tmp, hash_path) != 0) unlink(hash_path);
						} else unlink(tmp);
						unlink(hash_tmp);
					}
				} else unlink(tmp);
			}
		}
	}
	flock(block_fd, LOCK_UN);
	close(block_fd);
	pthread_mutex_unlock(lock);
	if (result == (int)length) cache_prune(block);
	return result;
}

/* Single background prefetch worker. It never outlives fuse_main. */
static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cond = PTHREAD_COND_INITIALIZER;
static pthread_t prefetch_thread;
static int prefetch_started, prefetch_shutdown, prefetch_pending, prefetch_busy;
static unsigned long long prefetch_block;
static unsigned int prefetch_depth;

static void *prefetch_worker(void *unused)
{
	(void)unused;
	char *data = malloc(CACHE_BLOCK_SIZE);
	if (!data) return NULL;
	pthread_mutex_lock(&prefetch_lock);
	while (!prefetch_shutdown) {
		while (!prefetch_pending && !prefetch_shutdown)
			pthread_cond_wait(&prefetch_cond, &prefetch_lock);
		if (prefetch_shutdown) break;
		unsigned long long block = prefetch_block;
		unsigned int depth = prefetch_depth;
		prefetch_pending = 0;
		prefetch_busy = 1;
		pthread_mutex_unlock(&prefetch_lock);
		for (unsigned int i = 0; i < depth; ++i) {
			unsigned long long index = block + i;
			if (index >= ((unsigned long long)httpfs_file.size + CACHE_BLOCK_SIZE - 1) / CACHE_BLOCK_SIZE) break;
			pthread_mutex_lock(&prefetch_lock);
			int stop = prefetch_shutdown;
			pthread_mutex_unlock(&prefetch_lock);
			if (stop) break;
			if (cache_block(data, index) < 0) break;
		}
		pthread_mutex_lock(&prefetch_lock);
		prefetch_busy = 0;
	}
	pthread_mutex_unlock(&prefetch_lock);
	free(data);
	return NULL;
}

static void schedule_prefetch(unsigned long long next, unsigned int depth)
{
	if (next >= ((unsigned long long)httpfs_file.size + CACHE_BLOCK_SIZE - 1) / CACHE_BLOCK_SIZE)
		return;
	pthread_mutex_lock(&prefetch_lock);
	if (prefetch_started && !prefetch_shutdown && !prefetch_pending && !prefetch_busy) {
		prefetch_block = next;
		prefetch_depth = depth;
		prefetch_pending = 1;
		pthread_cond_signal(&prefetch_cond);
	}
	pthread_mutex_unlock(&prefetch_lock);
}

static int httpfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
	(void)fi;
	if (strcmp(path, httpfs_file.filename)) return -ENOENT;
	if (offset < 0) return -EINVAL;
	if (!httpfs_file.metadata_valid) {
		int result = fetch_metadata();
		if (result != 0) return result;
	}
	if (size == 0 || (curl_off_t)offset >= httpfs_file.size) return 0;
	if ((curl_off_t)size > httpfs_file.size - (curl_off_t)offset)
		size = (size_t)(httpfs_file.size - (curl_off_t)offset);
	if (size > INT_MAX) size = INT_MAX;
	if (options.cache_dir && options.cache_dir[0]) {
		if (!cache_namespace[0]) {
			pthread_mutex_lock(&cache_init_lock);
			int init_result = cache_namespace[0] ? 0 : cache_init();
			pthread_mutex_unlock(&cache_init_lock);
			if (init_result != 0) return init_result;
		}
		if (cache_namespace[0]) {
			size_t done = 0;
			char block_data[CACHE_BLOCK_SIZE];
			while (done < size) {
				unsigned long long position = (unsigned long long)offset + done;
				unsigned long long block = position / CACHE_BLOCK_SIZE;
				size_t within = (size_t)(position % CACHE_BLOCK_SIZE);
				int got = cache_block(block_data, block);
				if (got < 0) return got;
				if ((size_t)got <= within) return -EIO;
				size_t amount = (size_t)got - within;
				if (amount > size - done) amount = size - done;
				memcpy(buf + done, block_data + within, amount);
				done += amount;
			}
			if (options.readahead && fi && fi->fh) {
				read_stream_t *stream = (read_stream_t *)(uintptr_t)fi->fh;
				pthread_mutex_lock(&stream->lock);
				int sequential = stream->end == offset || (stream->end < 0 && offset == 0);
				stream->streak = sequential ? (stream->streak < 3 ? stream->streak + 1 : 3) : 0;
				stream->end = offset + (off_t)done;
				unsigned int depth = stream->streak;
				pthread_mutex_unlock(&stream->lock);
				if (depth > 0 && done > 0)
					schedule_prefetch(((unsigned long long)offset + done + CACHE_BLOCK_SIZE - 1) / CACHE_BLOCK_SIZE, depth);
			}
			return (int)done;
		}
	}
	httpfs_buffer_t buffer = {buf, 0, size};
	return read_curl_buffer(size, offset, &buffer);
}

static void httpfs_destroy(void *private_data);

static struct fuse_operations httpfs_oper = {
	.getattr	= httpfs_getattr,
	.readdir	= httpfs_readdir,
	.open		= httpfs_open,
	.release = httpfs_release,
	.read		= httpfs_read,
	.destroy = httpfs_destroy,
};

static void httpfs_destroy(void *private_data)
{
	(void)private_data;
	pthread_mutex_lock(&prefetch_lock);
	prefetch_shutdown = 1;
	pthread_cond_signal(&prefetch_cond);
	pthread_mutex_unlock(&prefetch_lock);
	if (prefetch_started) pthread_join(prefetch_thread, NULL);
}

static void print_httpfs_help(const char *program)
{
	printf("Usage: %s -o url=URL[,filename=NAME][,cache_dir=DIR][,cache_max_bytes=N][,readahead=1] MOUNTPOINT [FUSE options]\\n"
	       "\\nRead-only HTTP Range filesystem (64 KiB disk cache blocks).\\n"
	       "\\nHTTPFS options (-o key=value):\\n"
	       "  url=URL             Remote HTTP(S) file (required)\\n"
	       "  filename=NAME       Local filename in the mounted directory (default: URL basename)\\n"
	       "  cache_dir=DIR       Optional persistent disk cache; requires strong ETag\\n"
	       "  cache_max_bytes=N   Maximum shared disk cache bytes (default: 0 = unlimited)\\n"
	       "  readahead=1         Prefetch 1-3 blocks as sequential reads continue (default: disabled)\\n"
	       "\\nThe quota applies across URLs and ETag versions in cache_dir.\\n"
	       "HTTP Range reads use 5s connection / 15s request timeouts and up to 3 attempts.\\n"
	       "Concurrent cache blocks use per-block locks; global eviction is serialized.\\n"
	       "\\nFUSE options: see fusermount3 and libfuse documentation.\\n",
	       program);
}

int main(int argc, char *argv[])
{
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_httpfs_help(argv[0]);
			return 0;
		}
	}

	char temp_url[512];
	char *str;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	memset(&options, 0, sizeof(struct options));

	if (fuse_opt_parse(&args, &options, httpfs_opts, NULL) == -1)
	{
		fprintf(stderr, "ERROR: Fail to parse arguments!\n");
		return -1;
	}

	if (!options.target_url)
	{
		fprintf(stderr, "`url` is not specified!\n");
		return -1;
	}

	if (strlen(options.target_url) >= sizeof(temp_url))
	{
		fprintf(stderr, "ERROR: Target URL is too long!\n");
		return -1;
	}

	if (!options.target_filename || !(*options.target_filename))
	{
		strcpy(temp_url, options.target_url);
		str = strrchr(temp_url, '?');
		if (str)
		{
			*str = '\0';
		}
		if (snprintf(httpfs_file.filename, sizeof(httpfs_file.filename), "/%s", basename(temp_url)) >= (int)sizeof(httpfs_file.filename))
		return 1;
	}
	else
	{
		if (snprintf(httpfs_file.filename, sizeof(httpfs_file.filename), "/%s", options.target_filename) >= (int)sizeof(httpfs_file.filename))
		return 1;
	}

	if (options.readahead && options.cache_dir && options.cache_dir[0]) {
		if (pthread_create(&prefetch_thread, NULL, prefetch_worker, NULL) == 0) prefetch_started = 1;
	}

	if (fuse_main(args.argc, args.argv, &httpfs_oper, NULL))
	{
		printf("\n");
	}

	fuse_opt_free_args(&args);

	return 0;
}
