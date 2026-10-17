// uvlite implementation: see uv.h.

#include "uv.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uvlite {

// ---- Thread pool (one per process, like libuv's) ------------------------------------------

class Pool {
public:
	static Pool &Get() {
		// Never destroyed: its threads may still be parked when the process exits.
		static Pool *pool = new Pool();
		return *pool;
	}
	void Submit(std::function<void()> task) {
		{
			std::lock_guard<std::mutex> g(mutex_);
			tasks_.push_back(std::move(task));
		}
		cv_.notify_one();
	}

private:
	Pool() {
		unsigned n = 4;  // libuv's default
		if (const char *env = getenv("UV_THREADPOOL_SIZE")) {
			long v = strtol(env, nullptr, 10);
			if (v > 0)
				n = (unsigned)std::min<long>(v, 1024);
		}
		for (unsigned i = 0; i < n; ++i)
			std::thread([this] { Run(); }).detach();
	}
	void Run() {
		for (;;) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_.wait(lock, [&] { return !tasks_.empty(); });
				task = std::move(tasks_.front());
				tasks_.pop_front();
			}
			task();
		}
	}
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> tasks_;
};

// ---- Loop -------------------------------------------------------------------------------------

struct Loop {
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::function<void()>> completed;  // callbacks to run on the loop thread
	int active = 0;                               // submitted, callback not yet run
};

static Loop *L(uv_loop_t *loop) {
	return static_cast<Loop *>(loop->impl_);
}

// Runs work on the pool, then done on the loop thread inside uv_run.
static void Submit(uv_loop_t *loop, std::function<void()> work, std::function<void()> done) {
	Loop *l = L(loop);
	{
		std::lock_guard<std::mutex> g(l->mutex);
		++l->active;
	}
	Pool::Get().Submit([l, work = std::move(work), done = std::move(done)]() mutable {
		work();
		{
			std::lock_guard<std::mutex> g(l->mutex);
			l->completed.push_back(std::move(done));
		}
		l->cv.notify_all();
	});
}

// ---- Files ------------------------------------------------------------------------------------

#ifdef _WIN32

// uv_file numbers for HANDLEs (small integers, like CRT descriptors).
static std::mutex g_filesMutex;
static std::vector<HANDLE> g_files;

static HANDLE Handle(uv_file f) {
	std::lock_guard<std::mutex> g(g_filesMutex);
	return f >= 3 && (size_t)(f - 3) < g_files.size() ? g_files[f - 3] : INVALID_HANDLE_VALUE;
}

static ssize_t WinError(DWORD e) {
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE: return UV_ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION: return UV_EACCES;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS: return UV_EEXIST;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY: return UV_ENOMEM;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL: return UV_ENOSPC;
	case ERROR_INVALID_HANDLE: return UV_EBADF;
	default: return UV_EIO;
	}
}

static std::wstring Wide(const char *utf8) {
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
	return w;
}

static ssize_t DoOpen(const char *path, int flags) {
	const int accmode = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
	DWORD access = accmode == _O_WRONLY ? GENERIC_WRITE : accmode == _O_RDWR ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
	DWORD disp;
	if ((flags & _O_CREAT) && (flags & _O_EXCL))
		disp = CREATE_NEW;
	else if ((flags & _O_CREAT) && (flags & _O_TRUNC))
		disp = CREATE_ALWAYS;
	else if (flags & _O_CREAT)
		disp = OPEN_ALWAYS;
	else if (flags & _O_TRUNC)
		disp = TRUNCATE_EXISTING;
	else
		disp = OPEN_EXISTING;
	DWORD share = FILE_SHARE_READ | (access == GENERIC_READ ? FILE_SHARE_WRITE : 0);
	HANDLE h = CreateFile2(Wide(path).c_str(), access, share, disp, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return WinError(GetLastError());
	std::lock_guard<std::mutex> g(g_filesMutex);
	for (size_t i = 0; i < g_files.size(); ++i)
		if (g_files[i] == INVALID_HANDLE_VALUE) {
			g_files[i] = h;
			return (ssize_t)(i + 3);
		}
	g_files.push_back(h);
	return (ssize_t)(g_files.size() - 1 + 3);
}

static ssize_t DoClose(uv_file f) {
	HANDLE h;
	{
		std::lock_guard<std::mutex> g(g_filesMutex);
		if (f < 3 || (size_t)(f - 3) >= g_files.size() || g_files[f - 3] == INVALID_HANDLE_VALUE)
			return UV_EBADF;
		h = g_files[f - 3];
		g_files[f - 3] = INVALID_HANDLE_VALUE;
	}
	return CloseHandle(h) ? 0 : WinError(GetLastError());
}

static ssize_t DoRW(uv_file f, const uv_buf_t *bufs, unsigned n, int64_t offset, bool write) {
	HANDLE h = Handle(f);
	if (h == INVALID_HANDLE_VALUE)
		return UV_EBADF;
	ssize_t total = 0;
	for (unsigned i = 0; i < n; ++i) {
		size_t done = 0;
		while (done < bufs[i].len) {
			OVERLAPPED ov{};
			uint64_t pos = (uint64_t)offset + (uint64_t)total + done;
			ov.Offset = (DWORD)pos;
			ov.OffsetHigh = (DWORD)(pos >> 32);
			DWORD chunk = (DWORD)std::min<size_t>(bufs[i].len - done, 1u << 30), got = 0;
			BOOL ok = write ? WriteFile(h, bufs[i].base + done, chunk, &got, &ov) : ReadFile(h, bufs[i].base + done, chunk, &got, &ov);
			if (!ok) {
				DWORD e = GetLastError();
				if (!write && e == ERROR_HANDLE_EOF)
					return total + (ssize_t)done;
				return total + done > 0 ? (ssize_t)(total + done) : WinError(e);
			}
			if (got == 0)
				return total + (ssize_t)done;  // end of file
			done += got;
		}
		total += (ssize_t)done;
	}
	return total;
}

static ssize_t DoFstat(uv_file f, uv_stat_t *st) {
	HANDLE h = Handle(f);
	if (h == INVALID_HANDLE_VALUE)
		return UV_EBADF;
	FILE_STANDARD_INFO info{};
	if (!GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info)))
		return WinError(GetLastError());
	st->st_size = (uint64_t)info.EndOfFile.QuadPart;
	return 0;
}

static ssize_t DoUnlink(const char *path) {
	return DeleteFileW(Wide(path).c_str()) ? 0 : WinError(GetLastError());
}

#else

static ssize_t DoOpen(const char *path, int flags) {
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	int fd;
	do {
		fd = ::open(path, flags, 0644);
	} while (fd < 0 && errno == EINTR);
	return fd < 0 ? -errno : fd;
}

static ssize_t DoClose(uv_file f) {
	return ::close(f) == 0 ? 0 : -errno;
}

static ssize_t DoRW(uv_file f, const uv_buf_t *bufs, unsigned n, int64_t offset, bool write) {
	ssize_t total = 0;
	for (unsigned i = 0; i < n; ++i) {
		size_t done = 0;
		while (done < bufs[i].len) {
			off_t pos = (off_t)(offset + total + (int64_t)done);
			ssize_t r = write ? ::pwrite(f, bufs[i].base + done, bufs[i].len - done, pos) : ::pread(f, bufs[i].base + done, bufs[i].len - done, pos);
			if (r < 0) {
				if (errno == EINTR)
					continue;
				return total + done > 0 ? (ssize_t)(total + done) : -errno;
			}
			if (r == 0)
				return total + (ssize_t)done;
			done += (size_t)r;
		}
		total += (ssize_t)done;
	}
	return total;
}

static ssize_t DoFstat(uv_file f, uv_stat_t *st) {
	struct stat s;
	if (::fstat(f, &s) != 0)
		return -errno;
	st->st_size = (uint64_t)s.st_size;
	return 0;
}

static ssize_t DoUnlink(const char *path) {
	return ::unlink(path) == 0 ? 0 : -errno;
}

#endif

// Runs op now (no callback) or on the pool (callback later, from uv_run).
static int FsRequest(uv_loop_t *loop, uv_fs_t *req, uv_fs_cb cb, std::function<ssize_t(uv_fs_t *)> op) {
	req->loop = loop;
	req->result = 0;
	if (!cb) {
		req->result = op(req);
		return (int)(req->result < 0 ? req->result : (req->result > INT32_MAX ? INT32_MAX : req->result));
	}
	auto result = std::make_shared<ssize_t>(0);
	auto stat = std::make_shared<uv_stat_t>();
	Submit(
		loop,
		[req, op, result, stat] {
			uv_fs_t tmp = *req;
			*result = op(&tmp);
			*stat = tmp.statbuf;
		},
		[req, cb, result, stat] {
			req->result = *result;
			req->statbuf = *stat;
			cb(req);
		});
	return 0;
}

}  // namespace uvlite

using namespace uvlite;

int uv_loop_init(uv_loop_t *loop) {
	loop->data = nullptr;
	loop->impl_ = new (std::nothrow) Loop();
	return loop->impl_ ? 0 : UV_ENOMEM;
}

int uv_loop_close(uv_loop_t *loop) {
	Loop *l = L(loop);
	if (!l)
		return 0;
	{
		std::lock_guard<std::mutex> g(l->mutex);
		if (l->active > 0)
			return -16;  // UV_EBUSY
	}
	delete l;
	loop->impl_ = nullptr;
	return 0;
}

int uv_run(uv_loop_t *loop, uv_run_mode mode) {
	Loop *l = L(loop);
	std::unique_lock<std::mutex> lock(l->mutex);
	for (;;) {
		if (l->completed.empty()) {
			if (l->active == 0 || mode == UV_RUN_NOWAIT)
				break;
			l->cv.wait(lock, [&] { return !l->completed.empty(); });
		}
		std::function<void()> done = std::move(l->completed.front());
		l->completed.pop_front();
		lock.unlock();
		done();  // may submit more requests
		lock.lock();
		--l->active;
		if (mode == UV_RUN_ONCE)
			break;
	}
	return l->active > 0 ? 1 : 0;
}

int uv_queue_work(uv_loop_t *loop, uv_work_t *req, uv_work_cb work_cb, uv_after_work_cb after_work_cb) {
	if (!work_cb)
		return UV_EINVAL;
	req->loop = loop;
	Submit(
		loop, [req, work_cb] { work_cb(req); },
		[req, after_work_cb] {
			if (after_work_cb)
				after_work_cb(req, 0);
		});
	return 0;
}

int uv_mutex_init(uv_mutex_t *) {
	return 0;
}

void uv_mutex_destroy(uv_mutex_t *) {
}

void uv_mutex_lock(uv_mutex_t *mutex) {
	mutex->m.lock();
}

void uv_mutex_unlock(uv_mutex_t *mutex) {
	mutex->m.unlock();
}

uv_buf_t uv_buf_init(char *base, unsigned int len) {
	uv_buf_t b;
	b.base = base;
	b.len = len;
	return b;
}

int uv_fs_open(uv_loop_t *loop, uv_fs_t *req, const char *path, int flags, int, uv_fs_cb cb) {
	std::string p = path ? path : "";
	return FsRequest(loop, req, cb, [p, flags](uv_fs_t *) { return DoOpen(p.c_str(), flags); });
}

int uv_fs_close(uv_loop_t *loop, uv_fs_t *req, uv_file file, uv_fs_cb cb) {
	return FsRequest(loop, req, cb, [file](uv_fs_t *) { return DoClose(file); });
}

int uv_fs_read(uv_loop_t *loop, uv_fs_t *req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb) {
	std::vector<uv_buf_t> b(bufs, bufs + nbufs);  // libuv copies the array too
	return FsRequest(loop, req, cb, [file, b, offset](uv_fs_t *) { return DoRW(file, b.data(), (unsigned)b.size(), offset, false); });
}

int uv_fs_write(uv_loop_t *loop, uv_fs_t *req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb) {
	std::vector<uv_buf_t> b(bufs, bufs + nbufs);
	return FsRequest(loop, req, cb, [file, b, offset](uv_fs_t *) { return DoRW(file, b.data(), (unsigned)b.size(), offset, true); });
}

int uv_fs_fstat(uv_loop_t *loop, uv_fs_t *req, uv_file file, uv_fs_cb cb) {
	return FsRequest(loop, req, cb, [file](uv_fs_t *r) { return DoFstat(file, &r->statbuf); });
}

int uv_fs_unlink(uv_loop_t *loop, uv_fs_t *req, const char *path, uv_fs_cb cb) {
	std::string p = path ? path : "";
	return FsRequest(loop, req, cb, [p](uv_fs_t *) { return DoUnlink(p.c_str()); });
}

void uv_fs_req_cleanup(uv_fs_t *) {
}
