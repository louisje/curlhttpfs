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

struct options {
	char *target_url;
	char *target_filename;
} options;

#define MY_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

static struct fuse_opt httpfs_opts[] = {
	MY_OPT_KEY("url=%s", target_url, 0),
	MY_OPT_KEY("filename=%s", target_filename, 0),
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
	0
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

static int httpfs_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, httpfs_file.filename) != 0)
	{
		return -ENOENT;
	}
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;
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
	httpfs_buffer_t buffer = {buf, 0, size};
	return read_curl_buffer(size, offset, &buffer);
}

static struct fuse_operations httpfs_oper = {
	.getattr	= httpfs_getattr,
	.readdir	= httpfs_readdir,
	.open		= httpfs_open,
	.read		= httpfs_read,
};

int main(int argc, char *argv[])
{
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

	if (fuse_main(args.argc, args.argv, &httpfs_oper, NULL))
	{
		printf("\n");
	}

	fuse_opt_free_args(&args);

	return 0;
}
