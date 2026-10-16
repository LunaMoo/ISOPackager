// Positional file I/O that is safe to use from several threads at once (pread / pwrite, or
// overlapped offsets on Windows), with UTF-8 paths everywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace isopk {

class File {
public:
	enum Mode { Read, Write /* create or truncate */ };

	File() = default;
	~File() { Close(); }
	File(const File &) = delete;
	File &operator=(const File &) = delete;
	File(File &&o) noexcept;
	File &operator=(File &&o) noexcept;

	bool Open(const std::string &utf8Path, Mode mode);
	void Close();
	bool IsOpen() const;

	uint64_t Size() const;
	// Returns bytes read (short only at end of file), or -1 on error.
	int64_t ReadAt(uint64_t offset, void *buf, size_t len) const;
	// Reads exactly len bytes or fails.
	bool ReadExact(uint64_t offset, void *buf, size_t len) const;
	bool WriteAt(uint64_t offset, const void *buf, size_t len);
	bool Truncate(uint64_t size);

	const std::string &LastError() const { return error_; }

private:
#ifdef _WIN32
	void *handle_ = nullptr;
#else
	int fd_ = -1;
#endif
	mutable std::string error_;
};

// Reads the first n bytes of a file. Returns the number read.
size_t ReadHead(const std::string &utf8Path, void *buf, size_t n);

}  // namespace isopk
