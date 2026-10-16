#include "core/fileio.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/util.h"

namespace isopk {

File::File(File &&o) noexcept {
	*this = std::move(o);
}

File &File::operator=(File &&o) noexcept {
	if (this != &o) {
		Close();
#ifdef _WIN32
		handle_ = o.handle_;
		o.handle_ = nullptr;
#else
		fd_ = o.fd_;
		o.fd_ = -1;
#endif
		error_ = std::move(o.error_);
	}
	return *this;
}

#ifdef _WIN32

static std::string WinError(DWORD e) {
	wchar_t *msg = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
		(LPWSTR)&msg, 0, nullptr);
	std::string out;
	if (msg) {
		int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
		out.resize(n > 0 ? n - 1 : 0);
		WideCharToMultiByte(CP_UTF8, 0, msg, -1, out.data(), n, nullptr, nullptr);
		LocalFree(msg);
		while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
			out.pop_back();
	}
	return out.empty() ? Fmt("error %lu", (unsigned long)e) : out;
}

bool File::Open(const std::string &path, Mode mode) {
	Close();
	std::wstring w = U8Path(path).wstring();
	DWORD access = mode == Read ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
	DWORD share = mode == Read ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
	DWORD disp = mode == Read ? OPEN_EXISTING : mode == Write ? CREATE_ALWAYS : OPEN_ALWAYS;
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
	HANDLE h = CreateFile2(w.c_str(), access, share, disp, nullptr);
#else
	HANDLE h = CreateFileW(w.c_str(), access, share, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
	if (h == INVALID_HANDLE_VALUE) {
		error_ = WinError(GetLastError());
		return false;
	}
	handle_ = h;
	return true;
}

void File::Close() {
	if (handle_) {
		CloseHandle((HANDLE)handle_);
		handle_ = nullptr;
	}
}

bool File::IsOpen() const {
	return handle_ != nullptr;
}

uint64_t File::Size() const {
	LARGE_INTEGER sz{};
	if (!handle_ || !GetFileSizeEx((HANDLE)handle_, &sz))
		return 0;
	return (uint64_t)sz.QuadPart;
}

int64_t File::ReadAt(uint64_t offset, void *buf, size_t len) const {
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;
	while (done < len) {
		OVERLAPPED ov{};
		uint64_t off = offset + done;
		ov.Offset = (DWORD)off;
		ov.OffsetHigh = (DWORD)(off >> 32);
		DWORD chunk = (DWORD)std::min<size_t>(len - done, 1u << 30), got = 0;
		if (!ReadFile((HANDLE)handle_, p + done, chunk, &got, &ov)) {
			DWORD e = GetLastError();
			if (e == ERROR_HANDLE_EOF)
				break;
			error_ = WinError(e);
			return -1;
		}
		if (got == 0)
			break;
		done += got;
	}
	return (int64_t)done;
}

bool File::WriteAt(uint64_t offset, const void *buf, size_t len) {
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;
	while (done < len) {
		OVERLAPPED ov{};
		uint64_t off = offset + done;
		ov.Offset = (DWORD)off;
		ov.OffsetHigh = (DWORD)(off >> 32);
		DWORD chunk = (DWORD)std::min<size_t>(len - done, 1u << 30), put = 0;
		if (!WriteFile((HANDLE)handle_, p + done, chunk, &put, &ov) || put == 0) {
			error_ = WinError(GetLastError());
			return false;
		}
		done += put;
	}
	return true;
}

bool File::Truncate(uint64_t size) {
	FILE_END_OF_FILE_INFO info{};
	info.EndOfFile.QuadPart = (LONGLONG)size;
	if (!SetFileInformationByHandle((HANDLE)handle_, FileEndOfFileInfo, &info, sizeof(info))) {
		error_ = WinError(GetLastError());
		return false;
	}
	return true;
}

#else

bool File::Open(const std::string &path, Mode mode) {
	Close();
	int flags = mode == Read ? O_RDONLY : mode == Write ? (O_RDWR | O_CREAT | O_TRUNC) : (O_RDWR | O_CREAT);
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	fd_ = ::open(path.c_str(), flags, 0644);
	if (fd_ < 0) {
		error_ = strerror(errno);
		return false;
	}
	return true;
}

void File::Close() {
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

bool File::IsOpen() const {
	return fd_ >= 0;
}

uint64_t File::Size() const {
	struct stat st;
	if (fd_ < 0 || fstat(fd_, &st) != 0)
		return 0;
	return (uint64_t)st.st_size;
}

int64_t File::ReadAt(uint64_t offset, void *buf, size_t len) const {
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;
	while (done < len) {
		ssize_t r = ::pread(fd_, p + done, len - done, (off_t)(offset + done));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			error_ = strerror(errno);
			return -1;
		}
		if (r == 0)
			break;
		done += (size_t)r;
	}
	return (int64_t)done;
}

bool File::WriteAt(uint64_t offset, const void *buf, size_t len) {
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;
	while (done < len) {
		ssize_t r = ::pwrite(fd_, p + done, len - done, (off_t)(offset + done));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			error_ = strerror(errno);
			return false;
		}
		done += (size_t)r;
	}
	return true;
}

bool File::Truncate(uint64_t size) {
	if (ftruncate(fd_, (off_t)size) != 0) {
		error_ = strerror(errno);
		return false;
	}
	return true;
}

#endif

bool File::ReadExact(uint64_t offset, void *buf, size_t len) const {
	int64_t r = ReadAt(offset, buf, len);
	if (r != (int64_t)len) {
		if (r >= 0)
			error_ = "unexpected end of file";
		return false;
	}
	return true;
}

size_t ReadHead(const std::string &path, void *buf, size_t n) {
	File f;
	if (!f.Open(path, File::Read))
		return 0;
	int64_t r = f.ReadAt(0, buf, n);
	return r < 0 ? 0 : (size_t)r;
}

}  // namespace isopk
