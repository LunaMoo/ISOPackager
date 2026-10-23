#include "core/formats.h"

#include <algorithm>
#include <cstring>

#include <chdmodern/chdmodern.h>
#include <ipbp/ipbp.h>
#include <irvz/irvz.h>

#include "core/fileio.h"
#include "core/image.h"
#include "core/util.h"
#include "core/zar.h"

namespace isopk {

const char *FormatName(Format f) {
	switch (f) {
	case Format::ISO: return "ISO";
	case Format::CSO: return "CSO";
	case Format::CSO2: return "CSO v2";
	case Format::ZSO: return "ZSO";
	case Format::DAX: return "DAX";
	case Format::CHD: return "CHD";
	case Format::PBP: return "PBP";
	case Format::WIA: return "WIA";
	case Format::RVZ: return "RVZ";
	case Format::ZAR: return "ZAR";
	case Format::CUE: return "CUE/BIN";
	case Format::CDIMG: return "CD image";
	case Format::PSX: return "PBP (PSOne)";
	default: return "?";
	}
}

const char *FormatKey(Format f) {
	switch (f) {
	case Format::ISO: return "iso";
	case Format::CSO: return "cso";
	case Format::CSO2: return "cso2";
	case Format::ZSO: return "zso";
	case Format::DAX: return "dax";
	case Format::CHD: return "chd";
	case Format::PBP: return "pbp";
	case Format::WIA: return "wia";
	case Format::RVZ: return "rvz";
	case Format::ZAR: return "zar";
	case Format::CUE: return "cue";
	case Format::CDIMG: return "cd";
	case Format::PSX: return "psx";
	default: return "unknown";
	}
}

const char *FormatExtension(Format f) {
	switch (f) {
	case Format::ISO: return ".iso";
	case Format::CSO: return ".cso";
	case Format::CSO2: return ".cso";
	case Format::ZSO: return ".zso";
	case Format::DAX: return ".dax";
	case Format::CHD: return ".chd";
	case Format::PBP: return ".pbp";
	case Format::WIA: return ".wia";
	case Format::RVZ: return ".rvz";
	case Format::ZAR: return ".zar";
	case Format::CUE: return ".cue";
	case Format::PSX: return ".pbp";
	default: return "";
	}
}

const char *FormatDescription(Format f) {
	switch (f) {
	case Format::ISO: return "Uncompressed disc image";
	case Format::CSO: return "CISO v1, deflate blocks (PPSSPP, PSP CFW)";
	case Format::CSO2: return "CISO v2, deflate or lz4 per block (experimental)";
	case Format::ZSO: return "ZISO, lz4 blocks: fastest to read (PPSSPP, PSP CFW)";
	case Format::DAX: return "DAX, zlib 8K frames (old PSP CFW)";
	case Format::CHD: return "MAME CHD: DVD (LZMA/zlib/zstd/FLAC hunks) or CD (from CD images)";
	case Format::PBP: return "PSN EBOOT.PBP, LZRC blocks (PSP, PPSSPP)";
	case Format::WIA: return "WIA, GameCube/Wii discs: bzip2/LZMA/LZMA2 chunks (wit, Dolphin)";
	case Format::RVZ: return "RVZ, GameCube/Wii discs: zstd/LZMA2/bzip2 chunks, junk removed (Dolphin)";
	case Format::ZAR: return "ZArchive: several discs, folders and DLC in one file, zstd blocks (Cemu, PPSSPP)";
	case Format::CUE: return "CD image: cue sheet and .bin tracks (from CD CHDs)";
	case Format::CDIMG: return "CD image: CloneCD, Alcohol 120%, GDI, Nero or raw .bin";
	case Format::PSX: return "PSOne EBOOT.PBP: PlayStation discs, deflate blocks (POPS, Adrenaline, PS1 emulators)";
	default: return "";
	}
}

Format FormatFromName(const std::string &in) {
	std::string s = ToLower(in);
	if (!s.empty() && s[0] == '.')
		s.erase(0, 1);
	if (s == "iso") return Format::ISO;
	if (s == "cso" || s == "cso1" || s == "ciso") return Format::CSO;
	if (s == "cso2" || s == "csov2") return Format::CSO2;
	if (s == "zso" || s == "ziso") return Format::ZSO;
	if (s == "dax") return Format::DAX;
	if (s == "chd") return Format::CHD;
	if (s == "pbp" || s == "eboot") return Format::PBP;
	if (s == "wia") return Format::WIA;
	if (s == "rvz") return Format::RVZ;
	if (s == "zar" || s == "zarchive") return Format::ZAR;
	if (s == "psx" || s == "ps1" || s == "psone" || s == "pbp1") return Format::PSX;
	if (s == "cue" || s == "bin" || s == "cuebin" || s == "cue/bin") return Format::CUE;
	return Format::Unknown;
}

bool IsMcsoFormat(Format f) {
	return f == Format::CSO || f == Format::CSO2 || f == Format::ZSO || f == Format::DAX;
}

bool IsIrvzFormat(Format f) {
	return f == Format::WIA || f == Format::RVZ;
}

bool IsCdImageFormat(Format f) {
	return f == Format::CUE || f == Format::CDIMG || f == Format::PSX;
}

bool HasRawCdSync(const std::string &path) {
	static const uint8_t sync[12] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	uint8_t h[16];
	return ReadHead(path, h, 16) == 16 && memcmp(h, sync, 12) == 0;
}

bool IsCdDescriptor(const std::string &path) {
	std::string ext = ToLower(PathU8(U8Path(path).extension()));
	return ext == ".cue" || ext == ".ccd" || ext == ".mds" || ext == ".gdi" || ext == ".nrg" || ext == ".toc";
}

bool IsCdSource(const std::string &path, Format f) {
	if (IsCdImageFormat(f))
		return true;
	if (f != Format::CHD)
		return false;
	chdm_file *file = nullptr;
	if (chdm_open(path.c_str(), CHDM_OPEN_READ, nullptr, &file) != CHDM_OK)
		return false;
	chdm_info ci{};
	chdm_get_info(file, &ci);
	chdm_close(file);
	return ci.kind == CHDM_KIND_CDROM || ci.kind == CHDM_KIND_GDROM;
}

const char *PlatformLabel(Platform p) {
	switch (p) {
	case Platform::PSP: return "PSP";
	case Platform::GameCube: return "GameCube";
	case Platform::Wii: return "Wii";
	case Platform::OtherIso: return "ISO";
	case Platform::CD: return "CD";
	default: return "";
	}
}

std::string TargetRejects(Format target, Platform p) {
	if (p == Platform::CD && target != Format::CHD && target != Format::CUE && target != Format::PSX)
		return std::string("CD images convert to CHD, CUE/BIN or PBP (PSOne), not ") + FormatName(target);
	if (target == Format::PSX && p != Platform::Unknown && p != Platform::CD)
		return std::string("PSOne EBOOTs hold PlayStation discs") + (p == Platform::PSP ? " (this is a PSP game: use PBP)" : "");
	if (target == Format::CUE && p != Platform::Unknown && p != Platform::CD)
		return std::string("CUE/BIN only holds CD images") + (p == Platform::OtherIso ? "" : Fmt(" (this is a %s disc)", PlatformLabel(p)));
	if (IsIrvzFormat(target) && p != Platform::Unknown && p != Platform::GameCube && p != Platform::Wii)
		return std::string(FormatName(target)) + " only holds GameCube and Wii discs" + (p == Platform::PSP ? " (this is a PSP game)" : "");
	if (target == Format::PBP && p != Platform::Unknown && p != Platform::PSP)
		return std::string("EBOOT.PBP only holds PSP games") + (p == Platform::GameCube || p == Platform::Wii ? Fmt(" (this is a %s disc)", PlatformLabel(p)) : "");
	return {};
}

bool FormatAvailable(Format f) {
#ifdef ISOPK_WITH_CSO
	return f != Format::Unknown && f != Format::Count;
#else
	return f != Format::Unknown && f != Format::Count && !IsMcsoFormat(f);
#endif
}

const std::vector<Format> &OutputFormats() {
	static const std::vector<Format> list = [] {
		std::vector<Format> v;
		for (Format f : { Format::CSO, Format::ZSO, Format::CHD, Format::PBP, Format::PSX, Format::RVZ, Format::WIA, Format::CUE, Format::ISO, Format::CSO2, Format::DAX })
			if (FormatAvailable(f))
				v.push_back(f);
		return v;
	}();
	return list;
}

bool IsImageExtension(const std::string &ext) {
	// .zar / .wua (Wii U archives are ZArchives too) are queued to be extracted.
	return ext == ".zar" || ext == ".wua" || ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax" || ext == ".chd" || ext == ".pbp" || ext == ".wia" ||
		ext == ".rvz" || ext == ".gcm" || ext == ".cue" || ext == ".ccd" || ext == ".mds" || ext == ".mdf" || ext == ".gdi" ||
		ext == ".nrg" || ext == ".bin" || ext == ".img";
}

static uint64_t FileSize(const std::string &path) {
	std::error_code ec;
	uint64_t n = fs::file_size(U8Path(path), ec);
	return ec ? 0 : n;
}

// Tracks, sizes and the first data track of a CD image or CD CHD.
static bool InspectCd(const std::string &path, ImageInfo &info) {
	chdm_cd_image_info cd{};
	if (chdm_cd_inspect(path.c_str(), &cd) != CHDM_OK) {
		info.error = chdm_last_error_message();
		return false;
	}
	info.platform = Platform::CD;
	info.cdTracks = cd.tracks;
	info.isoSize = cd.data_bytes;
	std::string what = Fmt("%u track%s", cd.tracks, cd.tracks == 1 ? "" : "s");
	if (cd.audio_tracks)
		what += Fmt(" (%u audio)", cd.audio_tracks);
	if (cd.first_data[0])
		what += Fmt(", %s", cd.first_data);
	if (cd.gdrom)
		what += ", GD-ROM";
	if (cd.sessions > 1)
		what += Fmt(", %u sessions", cd.sessions);
	if (IsCdImageFormat(info.format)) {
		info.blockSize = 2352;
		info.detail = Fmt("%s, %s, %u file%s", what.c_str(), cd.format, cd.files, cd.files == 1 ? "" : "s");
	} else {
		info.detail += ", " + what;
	}
	return true;
}

// A ZArchive ends with its footer: version, then magic, both big endian. Checked before the
// headers, since an archive storing an ISO uncompressed at its start looks like one.
static bool IsZArchive(const std::string &path) {
	File f;
	if (!f.Open(path, File::Read))
		return false;
	const uint64_t size = f.Size();
	uint8_t t[8];
	if (size < 200 || !f.ReadExact(size - 8, t, 8))
		return false;
	static const uint8_t tail[8] = { 0x61, 0xBF, 0x3A, 0x01, 0x16, 0x9F, 0x52, 0xD6 };
	return memcmp(t, tail, 8) == 0;
}

Format DetectFormat(const std::string &path) {
	if (IsZArchive(path))
		return Format::ZAR;
	uint8_t h[0x8006] = {};
	size_t n = ReadHead(path, h, sizeof(h));
	if (n >= 24 && memcmp(h, "CISO", 4) == 0)
		return h[20] == 2 ? Format::CSO2 : Format::CSO;
	if (n >= 24 && memcmp(h, "ZISO", 4) == 0)
		return Format::ZSO;
	if (n >= 8 && memcmp(h, "DAX\0", 4) == 0)
		return Format::DAX;
	if (n >= 8 && memcmp(h, "MComprHD", 8) == 0)
		return Format::CHD;
	if (n >= 8 && memcmp(h, "\0PBP", 4) == 0)
		return ipbp_pbp_kind(path.c_str()) == IPBP_PBP_PSX ? Format::PSX : Format::PBP;
	if (n >= 4 && memcmp(h, "WIA\x01", 4) == 0)
		return Format::WIA;
	if (n >= 4 && memcmp(h, "RVZ\x01", 4) == 0)
		return Format::RVZ;
	std::string ext = ToLower(PathU8(U8Path(path).extension()));
	// CD images: descriptors by name, track files by their raw sectors.
	if (ext == ".cue")
		return Format::CUE;
	if (ext == ".ccd" || ext == ".mds" || ext == ".mdf" || ext == ".gdi" || ext == ".nrg" || ext == ".toc")
		return Format::CDIMG;
	static const uint8_t sync[12] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	const bool rawSync = n >= 16 && memcmp(h, sync, 12) == 0;
	if (rawSync && (ext == ".iso" || ext == ".bin" || ext == ".img" || ext == ".raw"))
		return Format::CDIMG;
	if (n >= 0x8006 && memcmp(h + 0x8001, "CD001", 5) == 0)
		return Format::ISO;
	// Audio track files have no sync: whole 2352-byte sectors (read through the cue sheet naming them).
	if ((ext == ".bin" || ext == ".img" || ext == ".raw") && n > 0 && FileSize(path) % 2352 == 0)
		return Format::CDIMG;
	// GameCube / Wii images have no ISO9660 volume: their magic words are in the disc header.
	if (n >= 0x20 && (memcmp(h + 0x18, "\x5D\x1C\x9E\xA3", 4) == 0 || memcmp(h + 0x1C, "\xC2\x33\x9F\x3D", 4) == 0))
		return Format::ISO;
	// A headerless or damaged image: trust the extension.
	if (ext == ".iso" || ext == ".gcm")
		return Format::ISO;
	return Format::Unknown;
}

ImageInfo InspectImage(const std::string &path) {
	ImageInfo info;
	{
		File f;
		if (!f.Open(path, File::Read)) {
			info.error = f.LastError();
			return info;
		}
		info.fileSize = f.Size();
	}
	info.format = DetectFormat(path);
	switch (info.format) {
	case Format::ZAR: {
		uint64_t files = 0, bytes = 0;
		std::string err;
		if (ZArchiveContents(path, &files, &bytes, &err)) {
			info.isoSize = bytes;
			info.detail = Fmt("%llu file%s, extracts to a folder", (unsigned long long)files, files == 1 ? "" : "s");
		} else {
			info.error = err;
		}
		break;
	}
	case Format::ISO:
		info.isoSize = info.fileSize;
		info.blockSize = 2048;
		break;
	case Format::CSO:
	case Format::CSO2:
	case Format::ZSO: {
		uint8_t h[24];
		if (ReadHead(path, h, 24) == 24) {
			memcpy(&info.isoSize, h + 8, 8);
			memcpy(&info.blockSize, h + 16, 4);
			info.detail = Fmt("block %u, align %u", info.blockSize, h[21]);
		}
		break;
	}
	case Format::DAX: {
		uint8_t h[16];
		if (ReadHead(path, h, 16) == 16) {
			uint32_t total;
			memcpy(&total, h + 4, 4);
			info.isoSize = total;
			info.blockSize = 8192;
		}
		break;
	}
	case Format::CHD: {
		chdm_file *f = nullptr;
		if (chdm_open(path.c_str(), CHDM_OPEN_READ, nullptr, &f) == CHDM_OK) {
			chdm_info ci{};
			chdm_get_info(f, &ci);
			info.isoSize = ci.logical_bytes;
			info.blockSize = ci.hunk_bytes;
			std::string codecs;
			for (uint32_t c : ci.compression) {
				if (!c)
					continue;
				char name[5];
				chdm_codec_name(c, name);
				if (!codecs.empty())
					codecs += "+";
				codecs += name;
			}
			info.detail = Fmt("v%u, %s, hunk %u", ci.version, codecs.empty() ? "uncompressed" : codecs.c_str(), ci.hunk_bytes);
			chdm_close(f);
			if (ci.kind == CHDM_KIND_CDROM || ci.kind == CHDM_KIND_GDROM)
				InspectCd(path, info);
			else if (ci.kind == CHDM_KIND_AV)
				info.error = "a LaserDisc / A/V CHD, not a disc image";
		} else {
			info.error = chdm_last_error_message();
		}
		break;
	}
	case Format::PBP: {
		ipbp_job *job = ipbp_job_create();
		ipbp_pbp_info pi{};
		if (ipbp_read_pbp_info(job, path.c_str(), &pi) == IPBP_OK) {
			info.isoSize = pi.iso_size;
			info.blockSize = pi.block_basis * 2048;
			info.discId = pi.disc_id;
			info.discId.erase(std::remove(info.discId.begin(), info.discId.end(), '-'), info.discId.end());
			info.title = pi.title;
			info.detail = pi.compressed ? "LZRC" : "uncompressed";
		} else {
			info.error = ipbp_job_error(job);
		}
		ipbp_job_destroy(job);
		break;
	}
	case Format::WIA:
	case Format::RVZ: {
		irvz_file *f = nullptr;
		if (irvz_open(path.c_str(), &f) == IRVZ_OK) {
			irvz_info ri{};
			irvz_get_info(f, &ri);
			info.isoSize = ri.iso_size;
			info.blockSize = ri.chunk_size;
			info.discId = ri.game_id;
			info.title = ri.title;
			info.platform = ri.disc_type == 1 ? Platform::GameCube : ri.disc_type == 2 ? Platform::Wii : Platform::Unknown;
			std::string level = ri.compression == IRVZ_COMPRESSION_NONE || ri.compression == IRVZ_COMPRESSION_PURGE
				? std::string() : Fmt(" level %d", ri.compression_level);
			std::string chunk = ri.chunk_size >= (1u << 20) ? Fmt("%uM", ri.chunk_size >> 20) : Fmt("%uK", ri.chunk_size >> 10);
			info.detail = Fmt("%s%s, chunk %s", irvz_compression_name(ri.compression), level.c_str(), chunk.c_str());
			irvz_close(f);
		} else {
			info.error = std::string("iRVZ: ") + irvz_last_error();
		}
		break;
	}
	case Format::CUE:
	case Format::CDIMG:
		InspectCd(path, info);
		break;
	case Format::PSX: {
		ipbp_job *job = ipbp_job_create();
		ipbp_psx_info pi;
		if (ipbp_read_psx_info(job, path.c_str(), &pi) == IPBP_OK) {
			info.platform = Platform::CD;
			info.discId = pi.disc[0].disc_id[0] ? pi.disc[0].disc_id : pi.disc_id;
			info.title = pi.title;
			info.blockSize = 16 * 2352;
			uint32_t tracks = 0, audio = 0;
			uint64_t stored = 0;
			for (uint32_t d = 0; d < pi.discs; ++d) {
				info.isoSize += (uint64_t)pi.disc[d].frames * 2352;
				stored += pi.disc[d].stored_bytes;
				tracks += pi.disc[d].tracks;
				for (uint32_t t = 0; t < pi.disc[d].tracks; ++t)
					audio += pi.disc[d].track[t].audio ? 1 : 0;
			}
			info.cdTracks = pi.disc[0].tracks;
			info.detail = Fmt("%u disc%s, %u track%s", pi.discs, pi.discs == 1 ? "" : "s", tracks, tracks == 1 ? "" : "s");
			if (audio)
				info.detail += Fmt(" (%u audio)", audio);
			info.detail += stored < info.isoSize ? ", deflate" : ", stored";
			info.detail += pi.has_data_psp ? ", with DATA.PSP" : ", no DATA.PSP (emulators)";
		} else {
			info.error = ipbp_job_error(job);
		}
		ipbp_job_destroy(job);
		break;
	}
	default:
		info.error = "not a recognised disc image";
		break;
	}
	if (info.format == Format::PBP && info.error.empty())
		info.platform = Platform::PSP;

	if (info.error.empty() && info.platform != Platform::CD && (info.discId.empty() || info.platform == Platform::Unknown)) {
		std::string err;
		auto reader = OpenImageReader(path, info.format, &err);
		if (reader) {
			std::string id, title;
			ReadDiscInfo(*reader, &id, &title, &info.platform);
			if (info.discId.empty())
				info.discId = id;
			if (info.title.empty())
				info.title = title;
		}
	}
	return info;
}

}  // namespace isopk
