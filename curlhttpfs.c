/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` hello.c -o hello
*/

#define FUSE_USE_VERSION 26

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
	double size;
	long filetime;
} httpfs_file = {
	"/",
	0
};

typedef struct httpfs_buffer {
	char *p;
	long len;
} httpfs_buffer_t;

static int httpfs_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	CURL *curl;
	CURLcode code;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path, httpfs_file.filename) == 0) {
		stbuf->st_mode = S_IFREG | 0555;
		stbuf->st_nlink = 1;

		curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, options.target_url);
		curl_easy_setopt(curl, CURLOPT_FILETIME, 1);
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
		code = curl_easy_perform(curl);
		if (code != CURLE_OK)
		{
			return -ENOENT;
		}
		code = curl_easy_getinfo(curl, CURLINFO_FILETIME, &httpfs_file.filetime);
		if (code == CURLE_OK)
		{
			stbuf->st_mtime = (time_t)httpfs_file.filetime;
		}
		code = curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &httpfs_file.size);
		if (code == CURLE_OK)
		{
			stbuf->st_size = (off_t)httpfs_file.size;
		}
		curl_easy_cleanup(curl);
	}
	else
	{
		res = -ENOENT;
	}
	return res;
}

static int httpfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, httpfs_file.filename + 1, NULL, 0);

	return 0;
}

static int httpfs_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, httpfs_file.filename) != 0)
	{
		return -ENOENT;
	}
	/*
	if ((fi->flags & 3) != O_RDONLY)
	{
		return -EACCES;
	}
	*/
	return 0;
}

size_t my_write_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	int total_size = size * nmemb;
	struct httpfs_buffer *httpfs_buf = (struct httpfs_buffer *)stream;

	memcpy(httpfs_buf->p + httpfs_buf->len, ptr, total_size);
	httpfs_buf->len += total_size;

	return total_size;
}

long read_curl_buffer(size_t size, off_t offset, httpfs_buffer_t *httpfs_buf)
{
	char range[100];
	long res;
	CURL *curl = NULL;

	sprintf(range, "%llu-%llu", (unsigned long long)offset, (unsigned long long)offset + (unsigned long long)size - 1);

	curl = curl_easy_init();
	if (!curl)
	{
		return -1;
	}
	curl_easy_setopt(curl, CURLOPT_URL, options.target_url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, httpfs_buf);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
	curl_easy_setopt(curl, CURLOPT_RANGE, range);
	if (curl_easy_perform(curl) != CURLE_OK)
	{
		res = -1;
	}
	else
	{
		res = httpfs_buf->len;
	}
	curl_easy_cleanup(curl);
	return res;
}

static int httpfs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	(void) fi;
	CURL *curl;
	CURLcode code;
	struct httpfs_buffer httpfs_buf;

	if(strcmp(path, httpfs_file.filename) != 0)
	{
		return -ENOENT;
	}

	if (!httpfs_file.size) // Qoo
	{
		curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, options.target_url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		code = curl_easy_perform(curl);
		if (code != CURLE_OK)
		{
			return -ENOENT;
		}
		code = curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &httpfs_file.size);
		if (code != CURLE_OK)
		{
			return -ENOENT;
		}
		curl_easy_cleanup(curl);
	}

	if (offset >= httpfs_file.size)
	{
		return 0;
	}

	if (offset + size > httpfs_file.size)
	{
		size = httpfs_file.size - offset;
	}

	httpfs_buf.p = buf;
	httpfs_buf.len = 0;

	if (read_curl_buffer(size, offset, &httpfs_buf) < 0)
	{
		return -ENOENT;
	}
	else
	{
		return size;
	}
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

	if (strlen(options.target_url) > 512)
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
		strcat(httpfs_file.filename, basename(temp_url));
	}
	else
	{
		strcat(httpfs_file.filename, options.target_filename);
	}

	if (fuse_main(args.argc, args.argv, &httpfs_oper, NULL))
	{
		printf("\n");
	}

	fuse_opt_free_args(&args);

	return 0;
}
