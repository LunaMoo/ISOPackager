// Everything a conversion can be told, shared by the CLI and the GUI, and saved as a small INI file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/formats.h"

namespace isopk {

// ---- CSO / ZSO / DAX (mCSO) -------------------------------------------------------------------

enum class CsoPreset { Fast, Standard, Max, Custom };
const char *CsoPresetName(CsoPreset p);

struct CsoSettings {
	// The default: libdeflate only, 64K blocks (lz4 HC for ZSO). When set, everything below is ignored.
	bool useDefault = true;
	static constexpr uint32_t DEFAULT_BLOCK = 65536;
	CsoPreset preset = CsoPreset::Standard;
	uint32_t blockSize = 0;  // 0 = automatic (2K below 2 GB, 16K above); power of two 2K..256K
	// Methods, used by the Custom preset. Deflate ones apply to CSO/CSO v2/DAX, lz4 ones to ZSO/CSO v2.
	bool zlib = true, zlibBrute = true, sevenZip = true, zopfli = false, libdeflate = false;
	bool lz4 = true, lz4hc = true, lz4Brute = false;
	uint32_t zopfliSplits = 0;  // Zopfli block splits, 0 = Zopfli's default (15)
	double precheck = 0.0;      // 0 = off, else skip slow trials on blocks libdeflate can't get below this %
	bool forceAll = false;      // compress every block, even when it doesn't save space

	// Raw maxcso::TaskFlags for compressing to format.
	uint32_t Flags(Format format) const;
	uint32_t EffectiveBlockSize(Format format) const;  // what goes into maxcso_options.block_size
	std::string Describe(Format format) const;
};

// maxcso::TaskFlags values (src/compress.h in maxcso).
namespace mcso_flags {
constexpr uint32_t NO_ZLIB_DEFAULT = 0x01, NO_ZLIB_BRUTE = 0x02, NO_ZLIB = 0x03, NO_ZOPFLI = 0x04, NO_7ZIP = 0x08,
	FORCE_ALL = 0x10, FMT_ZSO = 0x20, FMT_CSO_2 = 0x40, NO_LZ4_DEFAULT = 0x80, NO_LZ4_HC = 0x100, NO_LZ4_HC_BRUTE = 0x200,
	NO_LZ4 = 0x380, DECOMPRESS = 0x400, FMT_DAX = 0x800, NO_LIBDEFLATE = 0x1000;
constexpr uint32_t NO_ALL = NO_ZLIB | NO_ZOPFLI | NO_7ZIP | NO_LZ4 | NO_LIBDEFLATE;
}  // namespace mcso_flags

// ---- CHD (CHDmodern) --------------------------------------------------------------------------

struct ChdSettings {
	uint32_t hunkBytes = 0;          // multiple of 2048, 0 = chdman's default (4096)
	std::vector<uint32_t> codecs;    // up to 4 FourCCs, empty = chdman's default (lzma, zlib, huff, flac)

	uint32_t EffectiveHunk() const { return hunkBytes ? hunkBytes : 4096; }
	std::string CodecsString() const;          // "default" or "lzma+zstd"
	bool SetCodecs(const std::string &list);   // "default", "zstd", "lzma+zlib", ...
	std::string Describe() const;
};
// Codecs that make sense for DVD/UMD data.
const std::vector<std::string> &ChdCodecChoices();

// ---- PBP (iPBP) -------------------------------------------------------------------------------

enum class PbpLayout { File /* Game.pbp */, Folder /* Game/EBOOT.PBP */ };

struct PbpSettings {
	int compress = 1;            // 0 none, 1 LZRC (sign_np compatible), 2 LZRC max
	std::string contentId;       // empty = derived from the disc ID
	std::string startDatPng;     // optional 480x272 boot screen
	bool trimOnExtract = true;   // cut the extracted ISO at its ISO9660 volume size
	bool verifyMacs = true;      // check block MACs when reading a PBP
	PbpLayout layout = PbpLayout::Folder;
	std::string Describe() const;
};
const char *PbpModeName(int compress);

// ---- RVZ / WIA (iRVZ) -------------------------------------------------------------------------

// Methods, as irvz_compression: 0 none, 1 purge (WIA only), 2 bzip2, 3 LZMA, 4 LZMA2, 5 zstd (RVZ only).
enum RvzMethod { RVZ_NONE = 0, RVZ_PURGE = 1, RVZ_BZIP2 = 2, RVZ_LZMA = 3, RVZ_LZMA2 = 4, RVZ_ZSTD = 5 };
const char *RvzMethodName(int method);
// Levels a method takes (min > max when it has none): bzip2/LZMA/LZMA2 1-9, zstd -22..22.
void RvzMethodLevels(int method, int *min, int *max);
bool RvzMethodAllowed(Format format, int method);

struct RvzSettings {
	// The default: LZMA2 level 5, the format's default chunk (RVZ 128K, WIA 2M). When set,
	// everything below is ignored.
	bool useDefault = true;
	int method = RVZ_LZMA2;
	int level = 5;
	uint32_t chunk = 0;  // 0 = the format's default; RVZ: power of two from 32K or multiple of 2M; WIA: multiple of 2M

	// What actually goes to iRVZ for format (methods and chunks it can't take are adjusted).
	int EffectiveMethod(Format format) const;
	int EffectiveLevel(Format format) const;
	uint32_t EffectiveChunk(Format format) const;
	std::string Describe(Format format) const;
};

// ---- ZArchive (.zar) --------------------------------------------------------------------------

struct ZarSettings {
	int level = 22;            // zstd 1..22, highest by default
	double precheck = 95.0;    // store blocks zstd's fastest level can't get below this % (0 = always try)
	bool storeDiscs = false;   // don't compress disc images at all (they're already CSO/CHD/...)
	bool compressDiscs = false;  // compress plain ISOs with ISOPackager first (in RAM), then store them
	Format discFormat = FormatAvailable(Format::CSO) ? Format::CSO : Format::CHD;
	std::string Describe() const;
};

// ---- Where outputs go -------------------------------------------------------------------------

enum class Conflict { Skip, Overwrite, Rename };
const char *ConflictName(Conflict c);

struct OutputSettings {
	std::string dir;            // empty = next to each source
	bool mirrorTree = true;     // keep the scanned folders' structure under dir
	Conflict conflict = Conflict::Skip;
	bool deleteSource = false;  // after a successful (and, with verify on, verified) conversion
};

// ---- Engine -----------------------------------------------------------------------------------

struct EngineSettings {
	bool verify = true;         // decode every output again and compare with the source
	bool repackInRam = true;    // compressed -> compressed through RAM instead of a temporary ISO
	uint64_t ramBudget = 0;     // bytes for in-RAM images across all jobs, 0 = automatic
	std::string tempDir;        // temporary ISOs when RAM isn't enough; empty = output folder
	unsigned jobs = 1;          // files converted at once (each library is multithreaded already)
	unsigned threads = 0;       // worker threads for iPBP / mCSO's pool, 0 = all cores
	bool lowPriority = true;    // below-normal process priority while working
};

// The benchmark list a fresh install starts with: libdeflate 256K CSO, chdman's default CHD, LZRC PBP.
std::vector<std::string> DefaultBenchCases();

struct Settings {
	Format target = FormatAvailable(Format::CSO) ? Format::CSO : Format::CHD;
	// Each container has its own settings.
	CsoSettings cso, zso, cso2, dax;
	ChdSettings chd;
	PbpSettings pbp;
	RvzSettings rvz, wia;
	ZarSettings zar;
	OutputSettings out;
	EngineSettings engine;
	bool recursive = true;      // when adding folders
	// GUI only
	std::vector<std::string> benchCases = DefaultBenchCases();  // serialised BenchCase list (bench.h)
	int uiTheme = 0;            // 0 dark, 1 light, 2 classic
	float uiScale = 1.0f;       // on top of the display's DPI scale

	// The settings of an mCSO container (CSO, ZSO, CSO v2, DAX) or an iRVZ one (RVZ, WIA).
	CsoSettings &Cso(Format f);
	const CsoSettings &Cso(Format f) const;
	RvzSettings &Rvz(Format f);
	const RvzSettings &Rvz(Format f) const;

	bool Load(const std::string &path);
	bool Save(const std::string &path) const;
	static std::string DefaultPath();  // per-user config file
};

}  // namespace isopk
