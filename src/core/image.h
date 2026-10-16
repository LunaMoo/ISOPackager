// Images as data: random-access readers over every format, the writers the libraries decompress
// into (RAM, byte-for-byte comparison, nothing), and the RAM budget that keeps parallel repacks
// from asking for more memory than the machine has.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/fileio.h"
#include "core/formats.h"

namespace isopk {

// ---- Readers ----------------------------------------------------------------------------------

class ImageReader {
public:
	virtual ~ImageReader() = default;
	virtual uint64_t Size() const = 0;
	// Reads exactly len bytes (fails past the end).
	virtual bool ReadAt(uint64_t offset, void *buf, size_t len) = 0;
	// True when ReadAt may be called from several threads at once.
	virtual bool ThreadSafe() const { return false; }
	const std::string &Error() const { return error_; }

protected:
	std::string error_;
};

class FileImageReader : public ImageReader {
public:
	bool Open(const std::string &path);
	uint64_t Size() const override { return size_; }
	bool ReadAt(uint64_t offset, void *buf, size_t len) override;
	bool ThreadSafe() const override { return true; }

private:
	File file_;
	uint64_t size_ = 0;
};

// CSO v1, CSO v2 and ZSO, decompressed a block at a time (like PPSSPP reads them).
class CsoImageReader : public ImageReader {
public:
	bool Open(const std::string &path);
	uint64_t Size() const override { return total_; }
	bool ReadAt(uint64_t offset, void *buf, size_t len) override;
	uint32_t BlockSize() const { return blockSize_; }
	uint32_t Blocks() const { return blocks_; }
	// Decompresses one whole block into out (BlockSize() bytes). Used by the benchmark.
	bool ReadBlock(uint32_t block, uint8_t *out);

private:
	File file_;
	enum Kind { V1, V2, ZSO } kind_ = V1;
	uint64_t total_ = 0;
	uint32_t blockSize_ = 0, blocks_ = 0;
	int align_ = 0;
	std::vector<uint32_t> index_;
	std::vector<uint8_t> in_, cache_;
	int64_t cached_ = -1;
};

struct chdm_file;
class ChdImageReader : public ImageReader {
public:
	~ChdImageReader() override;
	bool Open(const std::string &path);
	uint64_t Size() const override { return size_; }
	bool ReadAt(uint64_t offset, void *buf, size_t len) override;
	chdm_file *Handle() const { return file_; }

private:
	chdm_file *file_ = nullptr;
	uint64_t size_ = 0;
};

// The right reader for the format, or null (PBP and DAX have no random access).
std::unique_ptr<ImageReader> OpenImageReader(const std::string &path, Format format, std::string *error);

// ---- RAM budget -------------------------------------------------------------------------------

class RamBudget {
public:
	static RamBudget &Get();
	void SetCapacity(uint64_t bytes);
	uint64_t Capacity() const { return capacity_.load(); }
	uint64_t InUse() const { return used_.load(); }
	// Waits until n bytes fit. False at once if n can never fit, or when cancel is set.
	bool Acquire(uint64_t n, const std::atomic<bool> *cancel);
	void Release(uint64_t n);

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<uint64_t> capacity_{ 0 }, used_{ 0 };
};

// ---- Writers ----------------------------------------------------------------------------------

class ImageWriter {
public:
	virtual ~ImageWriter() = default;
	// Called once with the (maximum) image size before any write.
	virtual bool Begin(uint64_t size) = 0;
	// May be called from several threads, at any offset within the size.
	virtual bool WriteAt(uint64_t offset, const void *data, size_t len) = 0;
	// Called at the end when the library knows the final size (PBP trims its block padding).
	virtual bool Finish(uint64_t finalSize) { return true; }
	const std::string &Error() const { return error_; }

protected:
	std::string error_;
};

// A whole decompressed image in RAM; it's a writer (to fill it) and a reader (to compress from it).
class MemoryImage : public ImageWriter, public ImageReader {
public:
	explicit MemoryImage(const std::atomic<bool> *cancel = nullptr) : cancel_(cancel) {}
	~MemoryImage() override;

	bool Begin(uint64_t size) override;
	bool WriteAt(uint64_t offset, const void *data, size_t len) override;
	bool Finish(uint64_t finalSize) override;

	uint64_t Size() const override { return size_; }
	bool ReadAt(uint64_t offset, void *buf, size_t len) override;
	bool ThreadSafe() const override { return true; }

	// Loads a plain file (an ISO) into RAM.
	bool LoadFile(const std::string &path);
	const uint8_t *Data() const { return data_.get(); }
	// Begin() failed because the RAM wasn't available (budget or allocation).
	bool OutOfMemory() const { return oom_; }
	const std::string &Error() const { return ImageWriter::error_; }
	void Free();

private:
	const std::atomic<bool> *cancel_;
	std::unique_ptr<uint8_t[]> data_;
	uint64_t size_ = 0, reserved_ = 0;
	bool oom_ = false;
};

// Checks what a library decompresses against a reference image, without storing it anywhere.
// Bytes past the reference's end must be zero (PBP pads to whole blocks).
class CompareWriter : public ImageWriter {
public:
	explicit CompareWriter(ImageReader *reference) : ref_(reference) {}
	bool Begin(uint64_t size) override;
	bool WriteAt(uint64_t offset, const void *data, size_t len) override;
	bool Finish(uint64_t finalSize) override;
	// "OK" or what differed. Call after the library returned.
	std::string Result() const;
	bool Matched() const;

private:
	ImageReader *ref_;
	std::mutex refMutex_;  // for references that aren't thread safe
	std::atomic<uint64_t> covered_{ 0 };
	std::atomic<uint64_t> firstMismatch_{ UINT64_MAX };
	std::atomic<bool> extraData_{ false }, readFailed_{ false };
};

// Throws the data away (benchmark decompression timing without verify).
class NullWriter : public ImageWriter {
public:
	bool Begin(uint64_t) override { return true; }
	bool WriteAt(uint64_t, const void *, size_t) override { return true; }
};

// Straight to a file at random offsets.
class FileWriter : public ImageWriter {
public:
	explicit FileWriter(const std::string &path) : path_(path) {}
	bool Begin(uint64_t size) override;
	bool WriteAt(uint64_t offset, const void *data, size_t len) override;
	bool Finish(uint64_t finalSize) override;

private:
	std::string path_;
	File file_;
};

// ---- Disc metadata ----------------------------------------------------------------------------

// Reads UMD_DATA.BIN and PSP_GAME/PARAM.SFO through the ISO9660 file system.
bool ReadDiscInfo(ImageReader &image, std::string *discId, std::string *title);
// ISO9660 volume size in bytes (the size a good dump has), or 0.
uint64_t IsoVolumeSize(ImageReader &image);

}  // namespace isopk
