// ZArchive (.zar): one archive holding several discs, folders and DLC files, built from a folder
// (its contents become the archive's root) and/or picked files and folders.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/formats.h"

namespace isopk {

struct Settings;
struct OpContext;

// What a "Create ZArchive" queue entry packs.
struct ZarSpec {
	std::string name;                 // archive name without .zar, e.g. "Legend of Heroes - Trails in the Sky"
	std::string rootDir;              // its contents go to the archive's root (may be empty)
	std::vector<std::string> extras;  // more files / folders, each added at the root under its own name
	uint64_t totalSize = 0;           // bytes of all files, for the queue

	// The folder the archive goes next to when the output is "next to source".
	std::string SourceDir() const;
	// Every path the user picked (rootDir first).
	std::vector<std::string> Paths() const;
};

// Fills totalSize (walks the folders). False when nothing is there to pack.
bool MeasureZarSpec(ZarSpec *spec, std::string *error);

// A file going into the archive.
struct ZarEntry {
	std::string source;       // UTF-8 path on disk
	std::string archivePath;  // "DLC/ULUS10000/data.bin"
	uint64_t size = 0;
	bool isDir = false;       // an (empty or not) folder to create
	Format disc = Format::Unknown;  // a disc image, and which format
};

// Lists what goes where, renaming clashes ("Game (2).iso": ZArchive names are case-insensitive).
// exclude: a path never to add (the archive being written).
bool PlanZarEntries(const ZarSpec &spec, const std::string &exclude, std::vector<ZarEntry> *entries, std::string *error);

// A file as it went into the archive, for verifying it.
struct ZarWritten {
	std::string archivePath;
	uint64_t size = 0, hash = 0;
};

// What happened, for the queue and the log.
struct ZarReport {
	uint64_t files = 0, inputBytes = 0, blocks = 0, storedBlocks = 0, precheckSkipped = 0;
	// compressed: from ISO; converted: from another format; kept: already in the chosen format
	int discsCompressed = 0, discsConverted = 0, discsKept = 0, discsViaTemp = 0, discsStored = 0;
	double compressDiscSecs = 0;
	std::vector<ZarWritten> written;
	std::string Summary() const;  // "3 files, 2 discs compressed first, 81% of blocks stored"
};

// Progress of one ZAR job: phase changes go through onPhase (true = compressing a disc first).
struct ZarProgress {
	std::function<void(bool compressingDisc, const std::string &what)> onPhase;
};

// Writes the archive to outPath. ctx.progress gets 0..1 over the whole archive.
bool WriteZArchive(const ZarSpec &spec, const std::vector<ZarEntry> &entries, const Settings &s, const std::string &outPath,
	OpContext &ctx, ZarProgress &progress, ZarReport *report);
// Reads every file back from the archive and compares it with what was written (by hash).
bool VerifyZArchive(const std::string &path, const std::vector<ZarWritten> &written, OpContext &ctx, std::string *verdict);

// ---- Extracting -------------------------------------------------------------------------------

// Files and bytes inside an archive (walks its file tree, nothing is decompressed).
bool ZArchiveContents(const std::string &zarPath, uint64_t *files, uint64_t *bytes, std::string *error);
// Writes every folder and file of the archive under outDir (created). Names that aren't safe
// on this system are adjusted. written gets what was written where (archivePath = the path
// relative to outDir), for verifying. ctx.progress gets 0..1.
bool ExtractZArchive(const std::string &zarPath, const std::string &outDir, OpContext &ctx, std::vector<ZarWritten> *written);
// Checks the archive's own SHA-256 (in its footer) over every byte. verdict: "OK" or what's wrong.
bool TestZArchive(const std::string &zarPath, OpContext &ctx, std::string *verdict);
// Reads the extracted files back and compares them (by hash) with what the archive held.
bool VerifyExtractedZArchive(const std::string &outDir, const std::vector<ZarWritten> &written, OpContext &ctx, std::string *verdict);

// A 64-bit hash of a byte stream, fed in pieces of any size (the same for any split).
class StreamHash {
public:
	void Update(const void *data, size_t len);
	uint64_t Final() const;

private:
	uint64_t h_ = 0x9E3779B97F4A7C15ull, total_ = 0;
	uint8_t tail_[8] = {};
	size_t tailLen_ = 0;
	void Word(uint64_t w);
};

}  // namespace isopk
