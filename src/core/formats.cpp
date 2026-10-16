#include "core/formats.h"

#include <cstring>

#include <chdmodern/chdmodern.h>
#include <ipbp/ipbp.h>

#include "core/fileio.h"
#include "core/image.h"
#include "core/util.h"

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
	case Format::CHD: return "MAME CHD (DVD), LZMA/zlib/zstd/FLAC hunks (PPSSPP)";
	case Format::PBP: return "PSN EBOOT.PBP, LZRC blocks (PSP, PPSSPP)";
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
	return Format::Unknown;
}

bool IsMcsoFormat(Format f) {
	return f == Format::CSO || f == Format::CSO2 || f == Format::ZSO || f == Format::DAX;
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
		for (Format f : { Format::CSO, Format::ZSO, Format::CHD, Format::PBP, Format::ISO, Format::CSO2, Format::DAX })
			if (FormatAvailable(f))
				v.push_back(f);
		return v;
	}();
	return list;
}

bool IsImageExtension(const std::string &ext) {
	return ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax" || ext == ".chd" || ext == ".pbp";
}

Format DetectFormat(const std::string &path) {
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
		return Format::PBP;
	if (n >= 0x8006 && memcmp(h + 0x8001, "CD001", 5) == 0)
		return Format::ISO;
	// A headerless or damaged image: trust the extension.
	std::string ext = ToLower(PathU8(U8Path(path).extension()));
	if (ext == ".iso")
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
			info.detail = Fmt("v%u, %s, hunk %u%s", ci.version, codecs.empty() ? "uncompressed" : codecs.c_str(), ci.hunk_bytes,
				ci.kind == CHDM_KIND_DVD ? "" : ci.kind == CHDM_KIND_CDROM ? ", CD-ROM (not a UMD image)" : "");
			if (ci.kind == CHDM_KIND_CDROM || ci.kind == CHDM_KIND_GDROM || ci.kind == CHDM_KIND_AV)
				info.error = "not a DVD/UMD CHD";
			chdm_close(f);
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
			info.title = pi.title;
			info.detail = pi.compressed ? "LZRC" : "uncompressed";
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

	if (info.discId.empty() && info.error.empty()) {
		std::string err;
		auto reader = OpenImageReader(path, info.format, &err);
		if (reader)
			ReadDiscInfo(*reader, &info.discId, &info.title);
	}
	return info;
}

}  // namespace isopk
