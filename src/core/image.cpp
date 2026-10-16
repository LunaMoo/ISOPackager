#include "core/image.h"

#include <algorithm>
#include <cstring>
#include <new>

#include <chdmodern/chdmodern.h>
#include <zlib.h>
#ifdef ISOPK_WITH_CSO
#include <lz4.h>
#endif

#include "core/util.h"

namespace isopk {

static uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t Le64(const uint8_t *p) { return Le32(p) | ((uint64_t)Le32(p + 4) << 32); }
static uint16_t Le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ---- FileImageReader --------------------------------------------------------------------------

bool FileImageReader::Open(const std::string &path) {
	if (!file_.Open(path, File::Read)) {
		error_ = file_.LastError();
		return false;
	}
	size_ = file_.Size();
	return true;
}

bool FileImageReader::ReadAt(uint64_t offset, void *buf, size_t len) {
	if (offset + len > size_) {
		error_ = "read past the end of the image";
		return false;
	}
	if (!file_.ReadExact(offset, buf, len)) {
		error_ = file_.LastError();
		return false;
	}
	return true;
}

// ---- CsoImageReader ---------------------------------------------------------------------------

bool CsoImageReader::Open(const std::string &path) {
	if (!file_.Open(path, File::Read)) {
		error_ = file_.LastError();
		return false;
	}
	uint8_t h[24];
	if (!file_.ReadExact(0, h, sizeof(h))) {
		error_ = "file too short";
		return false;
	}
	if (memcmp(h, "CISO", 4) == 0)
		kind_ = h[20] == 2 ? V2 : V1;
	else if (memcmp(h, "ZISO", 4) == 0)
		kind_ = ZSO;
	else {
		error_ = "not a CSO/ZSO file";
		return false;
	}
	total_ = Le64(h + 8);
	blockSize_ = Le32(h + 16);
	align_ = h[21];
	if (blockSize_ < 512 || blockSize_ > (1u << 24) || (blockSize_ & (blockSize_ - 1)) != 0 || align_ > 31) {
		error_ = "bad CSO header";
		return false;
	}
	uint64_t blocks = (total_ + blockSize_ - 1) / blockSize_;
	if (blocks > 0x7FFFFFFF) {
		error_ = "bad CSO header";
		return false;
	}
	blocks_ = (uint32_t)blocks;
	index_.resize(blocks_ + 1);
	std::vector<uint8_t> raw((blocks_ + 1) * 4);
	if (!file_.ReadExact(24, raw.data(), raw.size())) {
		error_ = "truncated CSO index";
		return false;
	}
	for (uint32_t i = 0; i <= blocks_; ++i)
		index_[i] = Le32(&raw[i * 4]);
	cache_.resize(blockSize_);
	return true;
}

bool CsoImageReader::ReadBlock(uint32_t b, uint8_t *out) {
	if (b >= blocks_) {
		error_ = "block out of range";
		return false;
	}
	const bool high = (index_[b] & 0x80000000u) != 0;
	const uint64_t pos = (uint64_t)(index_[b] & 0x7FFFFFFFu) << align_;
	const uint64_t end = (uint64_t)(index_[b + 1] & 0x7FFFFFFFu) << align_;
	if (end < pos) {
		error_ = "bad CSO index";
		return false;
	}
	uint64_t len = end - pos;
	// The last block of a file may be short; it is still stored as a whole block's worth when plain.
	uint32_t want = blockSize_;
	if ((uint64_t)b * blockSize_ + want > total_)
		want = (uint32_t)(total_ - (uint64_t)b * blockSize_);

	bool plain = false, lz4 = false;
	switch (kind_) {
	case V1: plain = high; break;
	case ZSO: plain = high; lz4 = !high; break;
	case V2: lz4 = high; plain = !high && len >= blockSize_; break;
	}
	if (plain) {
		len = std::min<uint64_t>(len, want);
		int64_t got = file_.ReadAt(pos, out, (size_t)len);
		if (got < (int64_t)want) {
			if (got < 0) {
				error_ = file_.LastError();
				return false;
			}
			memset(out + got, 0, want - got);
		}
		return true;
	}
	if (len > (uint64_t)blockSize_ * 2 + 4096) {
		error_ = "bad CSO block size";
		return false;
	}
	in_.resize((size_t)len);
	int64_t got = file_.ReadAt(pos, in_.data(), (size_t)len);
	if (got <= 0) {
		error_ = got < 0 ? file_.LastError() : std::string("truncated CSO");
		return false;
	}
	if (lz4) {
#ifdef ISOPK_WITH_CSO
		int r = LZ4_decompress_safe_partial((const char *)in_.data(), (char *)out, (int)got, (int)want, (int)blockSize_);
		if (r < (int)want) {
			error_ = Fmt("lz4 error in block %u", b);
			return false;
		}
		return true;
#else
		error_ = "lz4 support not built in";
		return false;
#endif
	}
	z_stream z{};
	if (inflateInit2(&z, -15) != Z_OK) {
		error_ = "inflateInit failed";
		return false;
	}
	z.next_in = in_.data();
	z.avail_in = (uInt)got;
	z.next_out = out;
	z.avail_out = want;
	int r = inflate(&z, Z_FINISH);
	uLong produced = z.total_out;
	inflateEnd(&z);
	if ((r != Z_STREAM_END && r != Z_OK && r != Z_BUF_ERROR) || produced < want) {
		error_ = Fmt("deflate error in block %u", b);
		return false;
	}
	return true;
}

bool CsoImageReader::ReadAt(uint64_t offset, void *buf, size_t len) {
	if (offset + len > total_) {
		error_ = "read past the end of the image";
		return false;
	}
	uint8_t *dst = (uint8_t *)buf;
	while (len > 0) {
		uint32_t b = (uint32_t)(offset / blockSize_);
		uint32_t within = (uint32_t)(offset % blockSize_);
		if (cached_ != (int64_t)b) {
			if (!ReadBlock(b, cache_.data())) {
				cached_ = -1;
				return false;
			}
			cached_ = b;
		}
		size_t n = std::min<size_t>(len, blockSize_ - within);
		memcpy(dst, cache_.data() + within, n);
		dst += n;
		offset += n;
		len -= n;
	}
	return true;
}

// ---- ChdImageReader ---------------------------------------------------------------------------

ChdImageReader::~ChdImageReader() {
	chdm_close(file_);
}

bool ChdImageReader::Open(const std::string &path) {
	if (chdm_open(path.c_str(), CHDM_OPEN_READ, nullptr, &file_) != CHDM_OK) {
		error_ = chdm_last_error_message();
		file_ = nullptr;
		return false;
	}
	chdm_info info{};
	chdm_get_info(file_, &info);
	size_ = info.logical_bytes;
	return true;
}

bool ChdImageReader::ReadAt(uint64_t offset, void *buf, size_t len) {
	uint8_t *dst = (uint8_t *)buf;
	while (len > 0) {
		uint32_t n = (uint32_t)std::min<size_t>(len, 1u << 30);
		if (chdm_read_bytes(file_, offset, dst, n) != CHDM_OK) {
			error_ = chdm_last_error_message();
			return false;
		}
		dst += n;
		offset += n;
		len -= n;
	}
	return true;
}

std::unique_ptr<ImageReader> OpenImageReader(const std::string &path, Format format, std::string *error) {
	std::unique_ptr<ImageReader> r;
	bool ok = false;
	switch (format) {
	case Format::ISO: {
		auto f = std::make_unique<FileImageReader>();
		ok = f->Open(path);
		r = std::move(f);
		break;
	}
	case Format::CSO:
	case Format::CSO2:
	case Format::ZSO: {
		auto f = std::make_unique<CsoImageReader>();
		ok = f->Open(path);
		r = std::move(f);
		break;
	}
	case Format::CHD: {
		auto f = std::make_unique<ChdImageReader>();
		ok = f->Open(path);
		r = std::move(f);
		break;
	}
	default:
		if (error)
			*error = Fmt("%s has no random access", FormatName(format));
		return nullptr;
	}
	if (!ok) {
		if (error)
			*error = r->Error();
		return nullptr;
	}
	return r;
}

// ---- RamBudget --------------------------------------------------------------------------------

RamBudget &RamBudget::Get() {
	static RamBudget budget;
	return budget;
}

void RamBudget::SetCapacity(uint64_t bytes) {
	{
		std::lock_guard<std::mutex> g(mutex_);
		capacity_ = bytes;
	}
	cv_.notify_all();
}

bool RamBudget::Acquire(uint64_t n, const std::atomic<bool> *cancel) {
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		if (n > capacity_.load())
			return false;
		if (used_.load() + n <= capacity_.load()) {
			used_ += n;
			return true;
		}
		if (cancel && cancel->load())
			return false;
		cv_.wait_for(lock, std::chrono::milliseconds(200));
	}
}

void RamBudget::Release(uint64_t n) {
	{
		std::lock_guard<std::mutex> g(mutex_);
		used_ -= std::min<uint64_t>(n, used_.load());
	}
	cv_.notify_all();
}

// ---- MemoryImage ------------------------------------------------------------------------------

MemoryImage::~MemoryImage() {
	Free();
}

void MemoryImage::Free() {
	data_.reset();
	if (reserved_) {
		RamBudget::Get().Release(reserved_);
		reserved_ = 0;
	}
	size_ = 0;
}

bool MemoryImage::Begin(uint64_t size) {
	Free();
	oom_ = false;
	if (size > (uint64_t)SIZE_MAX) {
		oom_ = true;
		ImageWriter::error_ = "image too large for this platform's address space";
		return false;
	}
	if (!RamBudget::Get().Acquire(size, cancel_)) {
		oom_ = true;
		ImageWriter::error_ = Fmt("%s doesn't fit the RAM budget (%s)", HumanBytes(size).c_str(),
			HumanBytes(RamBudget::Get().Capacity()).c_str());
		return false;
	}
	reserved_ = size;
	data_.reset(new (std::nothrow) uint8_t[(size_t)std::max<uint64_t>(size, 1)]);
	if (!data_) {
		RamBudget::Get().Release(reserved_);
		reserved_ = 0;
		oom_ = true;
		ImageWriter::error_ = Fmt("could not allocate %s", HumanBytes(size).c_str());
		return false;
	}
	size_ = size;
	return true;
}

bool MemoryImage::WriteAt(uint64_t offset, const void *data, size_t len) {
	if (!data_ || offset + len > size_) {
		ImageWriter::error_ = "write outside the image";
		return false;
	}
	memcpy(data_.get() + offset, data, len);
	return true;
}

bool MemoryImage::Finish(uint64_t finalSize) {
	if (finalSize <= size_)
		size_ = finalSize;
	return true;
}

bool MemoryImage::ReadAt(uint64_t offset, void *buf, size_t len) {
	if (!data_ || offset + len > size_) {
		ImageReader::error_ = "read past the end of the image";
		return false;
	}
	memcpy(buf, data_.get() + offset, len);
	return true;
}

bool MemoryImage::LoadFile(const std::string &path) {
	File f;
	if (!f.Open(path, File::Read)) {
		ImageWriter::error_ = f.LastError();
		return false;
	}
	uint64_t size = f.Size();
	if (!Begin(size))
		return false;
	const size_t chunk = 8u << 20;
	for (uint64_t pos = 0; pos < size; pos += chunk) {
		if (cancel_ && cancel_->load()) {
			ImageWriter::error_ = "cancelled";
			Free();
			return false;
		}
		size_t n = (size_t)std::min<uint64_t>(chunk, size - pos);
		if (!f.ReadExact(pos, data_.get() + pos, n)) {
			ImageWriter::error_ = f.LastError();
			Free();
			return false;
		}
	}
	return true;
}

// ---- CompareWriter ----------------------------------------------------------------------------

bool CompareWriter::Begin(uint64_t) {
	return true;
}

bool CompareWriter::WriteAt(uint64_t offset, const void *data, size_t len) {
	const uint8_t *p = (const uint8_t *)data;
	const uint64_t refSize = ref_->Size();
	// The part inside the reference.
	if (offset < refSize) {
		size_t inside = (size_t)std::min<uint64_t>(len, refSize - offset);
		thread_local std::vector<uint8_t> buf;
		size_t done = 0;
		while (done < inside) {
			size_t n = std::min<size_t>(inside - done, 4u << 20);
			buf.resize(n);
			bool ok;
			if (ref_->ThreadSafe()) {
				ok = ref_->ReadAt(offset + done, buf.data(), n);
			} else {
				std::lock_guard<std::mutex> g(refMutex_);
				ok = ref_->ReadAt(offset + done, buf.data(), n);
			}
			if (!ok) {
				readFailed_ = true;
				error_ = ref_->Error();
				return false;
			}
			if (memcmp(buf.data(), p + done, n) != 0) {
				for (size_t i = 0; i < n; ++i) {
					if (buf[i] != p[done + i]) {
						uint64_t at = offset + done + i;
						uint64_t cur = firstMismatch_.load();
						while (at < cur && !firstMismatch_.compare_exchange_weak(cur, at)) {
						}
						break;
					}
				}
			}
			done += n;
		}
		covered_ += inside;
	}
	// Padding past the end must be zero.
	if (offset + len > refSize) {
		size_t start = offset >= refSize ? 0 : (size_t)(refSize - offset);
		for (size_t i = start; i < len; ++i) {
			if (p[i] != 0) {
				extraData_ = true;
				break;
			}
		}
	}
	return true;
}

bool CompareWriter::Finish(uint64_t) {
	return true;
}

bool CompareWriter::Matched() const {
	return !readFailed_ && firstMismatch_.load() == UINT64_MAX && !extraData_ && covered_.load() >= ref_->Size();
}

std::string CompareWriter::Result() const {
	if (readFailed_)
		return "could not read the original: " + error_;
	if (firstMismatch_.load() != UINT64_MAX)
		return Fmt("MISMATCH at byte %llu", (unsigned long long)firstMismatch_.load());
	if (extraData_)
		return "extra non-zero data past the end";
	if (covered_.load() < ref_->Size())
		return Fmt("decompressed image is shorter (%llu of %llu bytes)", (unsigned long long)covered_.load(),
			(unsigned long long)ref_->Size());
	return "OK";
}

// ---- FileWriter -------------------------------------------------------------------------------

bool FileWriter::Begin(uint64_t) {
	if (!file_.Open(path_, File::Write)) {
		error_ = file_.LastError();
		return false;
	}
	return true;
}

bool FileWriter::WriteAt(uint64_t offset, const void *data, size_t len) {
	if (!file_.WriteAt(offset, data, len)) {
		error_ = file_.LastError();
		return false;
	}
	return true;
}

bool FileWriter::Finish(uint64_t finalSize) {
	bool ok = finalSize == UINT64_MAX || file_.Truncate(finalSize);
	file_.Close();
	return ok;
}

// ---- ISO9660 ----------------------------------------------------------------------------------

namespace {

struct DirEntry {
	uint32_t lba = 0, size = 0;
	bool dir = false;
};

bool FindInDir(ImageReader &img, uint32_t lba, uint32_t size, const std::string &name, DirEntry *out) {
	size = std::min<uint32_t>(size, 1u << 20);
	std::vector<uint8_t> buf(size);
	if ((uint64_t)lba * 2048 + size > img.Size() || !img.ReadAt((uint64_t)lba * 2048, buf.data(), size))
		return false;
	uint32_t pos = 0;
	while (pos < size) {
		uint8_t len = buf[pos];
		if (len == 0) {
			// Records don't cross sectors: skip to the next one.
			pos = (pos / 2048 + 1) * 2048;
			continue;
		}
		if (pos + len > size || len < 34)
			break;
		uint8_t nameLen = buf[pos + 32];
		std::string n((const char *)&buf[pos + 33], std::min<size_t>(nameLen, len - 33));
		size_t semi = n.find(';');
		if (semi != std::string::npos)
			n.resize(semi);
		if (ToLower(n) == ToLower(name)) {
			out->lba = Le32(&buf[pos + 2]);
			out->size = Le32(&buf[pos + 10]);
			out->dir = (buf[pos + 25] & 2) != 0;
			return true;
		}
		pos += len;
	}
	return false;
}

bool ReadRoot(ImageReader &img, DirEntry *root) {
	uint8_t pvd[2048];
	if (img.Size() < 17 * 2048 || !img.ReadAt(16 * 2048, pvd, sizeof(pvd)) || memcmp(pvd + 1, "CD001", 5) != 0)
		return false;
	root->lba = Le32(pvd + 156 + 2);
	root->size = Le32(pvd + 156 + 10);
	root->dir = true;
	return true;
}

bool SfoString(const std::vector<uint8_t> &sfo, const char *key, std::string *out) {
	if (sfo.size() < 20 || memcmp(sfo.data(), "\0PSF", 4) != 0)
		return false;
	uint32_t keyTable = Le32(&sfo[8]), dataTable = Le32(&sfo[12]), count = Le32(&sfo[16]);
	for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= sfo.size(); ++i) {
		const uint8_t *e = &sfo[20 + i * 16];
		uint32_t k = keyTable + Le16(e), fmt = Le16(e + 2), len = Le32(e + 4), d = dataTable + Le32(e + 12);
		if (k >= sfo.size() || d + len > sfo.size())
			continue;
		const char *name = (const char *)&sfo[k];
		if (strncmp(name, key, sfo.size() - k) == 0 && fmt == 0x0204) {
			std::string s((const char *)&sfo[d], len);
			s.resize(strnlen(s.c_str(), s.size()));
			*out = s;
			return true;
		}
	}
	return false;
}

}  // namespace

uint64_t IsoVolumeSize(ImageReader &img) {
	uint8_t pvd[2048];
	if (img.Size() < 17 * 2048 || !img.ReadAt(16 * 2048, pvd, sizeof(pvd)) || memcmp(pvd + 1, "CD001", 5) != 0)
		return 0;
	return (uint64_t)Le32(pvd + 80) * Le16(pvd + 128);
}

bool ReadDiscInfo(ImageReader &img, std::string *discId, std::string *title) {
	DirEntry root;
	if (!ReadRoot(img, &root))
		return false;
	bool any = false;
	DirEntry e;
	if (discId && FindInDir(img, root.lba, root.size, "UMD_DATA.BIN", &e) && e.size > 0) {
		char buf[64] = {};
		uint32_t n = std::min<uint32_t>(e.size, sizeof(buf) - 1);
		if (img.ReadAt((uint64_t)e.lba * 2048, buf, n)) {
			std::string id(buf, strnlen(buf, n));
			id = id.substr(0, id.find('|'));
			id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
			if (!id.empty()) {
				*discId = id;
				any = true;
			}
		}
	}
	DirEntry game, sfoEntry;
	if (FindInDir(img, root.lba, root.size, "PSP_GAME", &game) && game.dir &&
		FindInDir(img, game.lba, game.size, "PARAM.SFO", &sfoEntry) && sfoEntry.size > 0 && sfoEntry.size < (1u << 20)) {
		std::vector<uint8_t> sfo(sfoEntry.size);
		if (img.ReadAt((uint64_t)sfoEntry.lba * 2048, sfo.data(), sfo.size())) {
			std::string s;
			if (title && SfoString(sfo, "TITLE", &s)) {
				*title = s;
				any = true;
			}
			if (discId && discId->empty() && SfoString(sfo, "DISC_ID", &s)) {
				*discId = s;
				any = true;
			}
		}
	}
	return any;
}

}  // namespace isopk
