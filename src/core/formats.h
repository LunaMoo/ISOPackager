// Disc image formats ISOPackager reads and writes, and how to recognise them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace isopk {

enum class Format {
	Unknown = 0,
	ISO,   // plain 2048-byte-sector image
	CSO,   // CISO v1 (deflate), via mCSO
	CSO2,  // CISO v2 (deflate + lz4), via mCSO
	ZSO,   // ZISO (lz4), via mCSO
	DAX,   // DAX (zlib, fixed 8K frames), via mCSO
	CHD,   // MAME CHD, DVD type, via CHDmodern
	PBP,   // PSN EBOOT.PBP (NPUMDIMG), via iPBP
	WIA,   // GameCube / Wii WIA (wit), via iRVZ
	RVZ,   // GameCube / Wii RVZ (Dolphin), via iRVZ
	ZAR,   // ZArchive: several discs, folders and DLC in one file. Only made by "Create ZArchive"
	       // jobs, so it isn't in OutputFormats().
	CUE,   // CD image: cue sheet + .bin track files (read and written, via CHDmodern)
	CDIMG, // CD image read only: CloneCD .ccd/.img, Alcohol .mds/.mdf, .gdi, .nrg, bare raw .bin/.img
	Count
};

const char *FormatName(Format f);        // "CSO", "CHD", ...
const char *FormatKey(Format f);         // "cso", "cso2", "chd", ... (CLI and INI)
const char *FormatExtension(Format f);   // ".cso", ".chd", ".pbp" (PBP as a single file)
const char *FormatDescription(Format f); // one line for the UI
Format FormatFromName(const std::string &s);  // "cso", "cso2", "zso", "dax", "chd", "pbp", "iso"
bool IsMcsoFormat(Format f);
bool IsIrvzFormat(Format f);  // WIA, RVZ
bool IsCdImageFormat(Format f);  // CUE, CDIMG
// Whether the file starts with a raw CD sector's sync pattern (a raw .bin/.img/.iso data track).
bool HasRawCdSync(const std::string &utf8Path);
// .cue, .ccd, .mds, .gdi, .nrg, .toc: the file that describes a CD image's tracks.
bool IsCdDescriptor(const std::string &utf8Path);
// A CD image or a CD / GD-ROM CHD: these convert only to CD CHDs and cue/bin.
bool IsCdSource(const std::string &utf8Path, Format f);

// What kind of disc an image holds; decides which formats can store it (RVZ/WIA only take
// GameCube and Wii discs, EBOOT.PBP only PSP games).
enum class Platform { Unknown, PSP, GameCube, Wii, OtherIso, CD };
const char *PlatformLabel(Platform p);
// Empty when target can hold a disc of this platform, otherwise why not.
std::string TargetRejects(Format target, Platform p);
// Whether this build can read or write the format (CSO family needs mCSO).
bool FormatAvailable(Format f);
// The output formats, in the order the UI lists them.
const std::vector<Format> &OutputFormats();

// File extensions ISOPackager picks up when scanning folders (lower case, with the dot).
bool IsImageExtension(const std::string &extLower);

// Recognises an image by its header (falls back to the extension for headerless ISOs).
Format DetectFormat(const std::string &utf8Path);

// What ISOPackager knows about an image without decompressing all of it.
struct ImageInfo {
	Format format = Format::Unknown;
	uint64_t fileSize = 0;
	uint64_t isoSize = 0;      // uncompressed size, 0 if unknown
	uint32_t blockSize = 0;    // CSO block / CHD hunk / PBP block
	std::string detail;        // "zlib+lzma, hunk 4096", "LZRC", ...
	std::string discId;        // "ULUS10041" when found
	std::string title;         // from PARAM.SFO / the GameCube-Wii header when found
	Platform platform = Platform::Unknown;
	std::string error;         // set when the image could not be inspected
	uint32_t cdTracks = 0;     // CD images and CD CHDs
};

// Reads the header and, where the format allows random access, the disc ID and title.
ImageInfo InspectImage(const std::string &utf8Path);

}  // namespace isopk
