// uvlite: the part of libuv that maxcso uses (an event loop, a thread pool, mutexes and file
// I/O), for platforms where libuv itself doesn't build, such as UWP. Same names, same
// semantics: requests with a callback run on the thread pool and their callbacks run in uv_run
// on the loop's thread; requests without a callback run synchronously and return the result.
//
// Selected with -DISOPK_CSO_PORTABLE=ON (the default for UWP); usable on any platform, which is
// how it is tested.
#pragma once

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#include <mutex>

#if defined(_WIN32) && !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
#define _SSIZE_T_
#define _SSIZE_T_DEFINED
#endif

// Error codes: negative errno values, like libuv on Unix.
#define UV_ENOENT (-2)
#define UV_EIO (-5)
#define UV_EBADF (-9)
#define UV_ENOMEM (-12)
#define UV_EACCES (-13)
#define UV_EEXIST (-17)
#define UV_EINVAL (-22)
#define UV_ENOSPC (-28)
#define UV_ECANCELED (-125)

typedef int uv_file;

typedef enum { UV_RUN_DEFAULT = 0, UV_RUN_ONCE, UV_RUN_NOWAIT } uv_run_mode;

typedef struct uv_loop_s {
	void *data;
	void *impl_;  // uvlite::Loop
} uv_loop_t;

typedef struct uv_buf_s {
	char *base;
	size_t len;
} uv_buf_t;

typedef struct uv_stat_s {
	uint64_t st_size;
} uv_stat_t;

typedef struct uv_fs_s uv_fs_t;
typedef void (*uv_fs_cb)(uv_fs_t *req);
struct uv_fs_s {
	void *data;
	ssize_t result;
	uv_stat_t statbuf;
	uv_loop_t *loop;
};

typedef struct uv_work_s uv_work_t;
typedef void (*uv_work_cb)(uv_work_t *req);
typedef void (*uv_after_work_cb)(uv_work_t *req, int status);
struct uv_work_s {
	void *data;
	uv_loop_t *loop;
};

// std::mutex is constant-initialised, so a static uv_mutex_t works before uv_mutex_init.
typedef struct uv_mutex_s {
	std::mutex m;
} uv_mutex_t;

int uv_loop_init(uv_loop_t *loop);
int uv_loop_close(uv_loop_t *loop);
int uv_run(uv_loop_t *loop, uv_run_mode mode);

int uv_queue_work(uv_loop_t *loop, uv_work_t *req, uv_work_cb work_cb, uv_after_work_cb after_work_cb);

int uv_mutex_init(uv_mutex_t *mutex);
void uv_mutex_destroy(uv_mutex_t *mutex);
void uv_mutex_lock(uv_mutex_t *mutex);
void uv_mutex_unlock(uv_mutex_t *mutex);

uv_buf_t uv_buf_init(char *base, unsigned int len);

int uv_fs_open(uv_loop_t *loop, uv_fs_t *req, const char *path, int flags, int mode, uv_fs_cb cb);
int uv_fs_close(uv_loop_t *loop, uv_fs_t *req, uv_file file, uv_fs_cb cb);
int uv_fs_read(uv_loop_t *loop, uv_fs_t *req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb);
int uv_fs_write(uv_loop_t *loop, uv_fs_t *req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb);
int uv_fs_fstat(uv_loop_t *loop, uv_fs_t *req, uv_file file, uv_fs_cb cb);
int uv_fs_unlink(uv_loop_t *loop, uv_fs_t *req, const char *path, uv_fs_cb cb);
void uv_fs_req_cleanup(uv_fs_t *req);
