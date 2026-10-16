#include "core/settings.h"

#include <cstdio>
#include <cstdlib>
#include <map>

#include "core/fileio.h"
#include "core/sysinfo.h"
#include "core/util.h"

namespace isopk {

using namespace mcso_flags;

const char *CsoPresetName(CsoPreset p) {
	switch (p) {
	case CsoPreset::Fast: return "fast";
	case CsoPreset::Standard: return "standard";
	case CsoPreset::Max: return "max";
	default: return "custom";
	}
}

uint32_t CsoSettings::Flags(Format f) const {
	uint32_t flags = 0;
	const bool v2 = f == Format::CSO2, zso = f == Format::ZSO;
	switch (preset) {
	case CsoPreset::Fast:
		if (v2)
			flags = NO_ZLIB_BRUTE | NO_ZOPFLI | NO_7ZIP | NO_LZ4_HC_BRUTE | NO_LZ4_HC | NO_LIBDEFLATE;
		else if (zso)
			flags = NO_ZLIB | NO_7ZIP | NO_ZOPFLI | NO_LIBDEFLATE | NO_LZ4_HC_BRUTE | NO_LZ4_HC;
		else
			flags = NO_ZOPFLI | NO_LIBDEFLATE | NO_LZ4 | NO_ZLIB_BRUTE | NO_7ZIP;
		break;
	case CsoPreset::Standard:
		if (v2)
			flags = NO_ZOPFLI | NO_LZ4_HC_BRUTE;
		else if (zso)
			flags = NO_ZLIB | NO_7ZIP | NO_ZOPFLI | NO_LZ4_HC_BRUTE | NO_LIBDEFLATE;
		else
			flags = NO_ZOPFLI | NO_LIBDEFLATE | NO_LZ4;
		break;
	case CsoPreset::Max:
		if (v2)
			flags = 0;
		else if (zso)
			flags = NO_ZLIB | NO_7ZIP | NO_ZOPFLI | NO_LIBDEFLATE;
		else
			flags = NO_LZ4;
		break;
	case CsoPreset::Custom:
		flags = NO_ALL;
		if (zlib) flags &= ~NO_ZLIB_DEFAULT;
		if (zlibBrute) flags &= ~NO_ZLIB_BRUTE;
		if (sevenZip) flags &= ~NO_7ZIP;
		if (zopfli) flags &= ~NO_ZOPFLI;
		if (libdeflate) flags &= ~NO_LIBDEFLATE;
		if (lz4) flags &= ~NO_LZ4_DEFAULT;
		if (lz4hc) flags &= ~NO_LZ4_HC;
		if (lz4Brute) flags &= ~NO_LZ4_HC_BRUTE;
		if (zso)
			flags |= NO_ZLIB | NO_7ZIP | NO_ZOPFLI | NO_LIBDEFLATE;
		else if (!v2)
			flags |= NO_LZ4;
		// A format whose every method is off would store everything uncompressed: keep the basic one.
		if (zso && (flags & NO_LZ4) == NO_LZ4)
			flags &= ~NO_LZ4_DEFAULT;
		if (!zso && (flags & (NO_ZLIB | NO_ZOPFLI | NO_7ZIP | NO_LIBDEFLATE)) == (NO_ZLIB | NO_ZOPFLI | NO_7ZIP | NO_LIBDEFLATE) &&
			(v2 ? (flags & NO_LZ4) == NO_LZ4 : true))
			flags &= ~NO_ZLIB_DEFAULT;
		break;
	}
	if (forceAll)
		flags |= FORCE_ALL;
	if (v2)
		flags |= FMT_CSO_2;
	else if (zso)
		flags |= FMT_ZSO;
	else if (f == Format::DAX)
		flags |= FMT_DAX;
	return flags;
}

uint32_t CsoSettings::EffectiveBlockSize(Format f) const {
	if (f == Format::DAX || blockSize == 0)
		return 0xFFFFFFFFu;  // automatic (DAX has a fixed frame size)
	return blockSize;
}

std::string CsoSettings::Describe(Format f) const {
	std::string s = CsoPresetName(preset);
	if (preset == CsoPreset::Custom) {
		uint32_t fl = Flags(f);
		std::string m;
		auto add = [&](bool on, const char *name) {
			if (on) {
				if (!m.empty())
					m += "+";
				m += name;
			}
		};
		add(!(fl & NO_ZLIB_DEFAULT), "zlib");
		add(!(fl & NO_ZLIB_BRUTE), "zlib-brute");
		add(!(fl & NO_7ZIP), "7z");
		add(!(fl & NO_ZOPFLI), "zopfli");
		add(!(fl & NO_LIBDEFLATE), "libdeflate");
		add(!(fl & NO_LZ4_DEFAULT), "lz4");
		add(!(fl & NO_LZ4_HC), "lz4hc");
		add(!(fl & NO_LZ4_HC_BRUTE), "lz4hc-brute");
		s = m;
	}
	if (f != Format::DAX)
		s += blockSize ? Fmt(" %uK", blockSize / 1024) : " auto-block";
	if (zopfliSplits && (Flags(f) & NO_ZOPFLI) == 0)
		s += Fmt(" splits%u", zopfliSplits);
	if (precheck > 0)
		s += Fmt(" precheck%.0f%%", precheck);
	if (forceAll)
		s += " force-all";
	return s;
}

std::string ChdSettings::CodecsString() const {
	if (codecs.empty())
		return "default";
	std::string s;
	for (uint32_t c : codecs) {
		if (!s.empty())
			s += "+";
		s += FourCCName(c);
	}
	return s;
}

bool ChdSettings::SetCodecs(const std::string &list) {
	std::string l = ToLower(Trim(list));
	if (l.empty() || l == "default") {
		codecs.clear();
		return true;
	}
	std::vector<uint32_t> out;
	for (const std::string &name : Split(l, '+')) {
		bool known = false;
		for (const std::string &c : ChdCodecChoices())
			known |= c == name;
		if (!known || out.size() >= 4)
			return false;
		out.push_back(FourCC(name.c_str()));
	}
	codecs = out;
	return true;
}

std::string ChdSettings::Describe() const {
	return Fmt("%s hunk %u", CodecsString().c_str(), EffectiveHunk());
}

const std::vector<std::string> &ChdCodecChoices() {
	static const std::vector<std::string> list = { "lzma", "zlib", "zstd", "huff", "flac" };
	return list;
}

const char *PbpModeName(int compress) {
	return compress == 2 ? "max" : compress ? "lzrc" : "none";
}

std::string PbpSettings::Describe() const {
	return compress == 2 ? "LZRC max" : compress ? "LZRC" : "uncompressed";
}

const char *ConflictName(Conflict c) {
	switch (c) {
	case Conflict::Overwrite: return "overwrite";
	case Conflict::Rename: return "rename";
	default: return "skip";
	}
}

// ---- INI --------------------------------------------------------------------------------------

std::string Settings::DefaultPath() {
	return PathU8(U8Path(AppDataDir()) / "isopackager.ini");
}

bool Settings::Load(const std::string &path) {
	File f;
	if (!f.Open(path, File::Read))
		return false;
	std::string text(f.Size(), '\0');
	if (!text.empty() && !f.ReadExact(0, text.data(), text.size()))
		return false;
	std::map<std::string, std::string> kv;
	for (const std::string &raw : Split(text, '\n')) {
		std::string line = Trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
			continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		kv[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
	}
	auto str = [&](const char *k, std::string &v) {
		auto it = kv.find(k);
		if (it != kv.end())
			v = it->second;
	};
	auto boolean = [&](const char *k, bool &v) {
		auto it = kv.find(k);
		if (it != kv.end())
			v = it->second == "1" || ToLower(it->second) == "true";
	};
	auto u32 = [&](const char *k, uint32_t &v) {
		auto it = kv.find(k);
		if (it != kv.end())
			v = (uint32_t)strtoul(it->second.c_str(), nullptr, 10);
	};
	auto uns = [&](const char *k, unsigned &v) {
		uint32_t t = v;
		u32(k, t);
		v = t;
	};
	auto dbl = [&](const char *k, double &v) {
		auto it = kv.find(k);
		if (it != kv.end())
			v = strtod(it->second.c_str(), nullptr);
	};

	std::string s;
	str("target", s);
	if (FormatFromName(s) != Format::Unknown && FormatAvailable(FormatFromName(s)))
		target = FormatFromName(s);
	s.clear();
	str("cso.preset", s);
	for (CsoPreset p : { CsoPreset::Fast, CsoPreset::Standard, CsoPreset::Max, CsoPreset::Custom })
		if (s == CsoPresetName(p))
			cso.preset = p;
	u32("cso.block", cso.blockSize);
	boolean("cso.zlib", cso.zlib);
	boolean("cso.zlib_brute", cso.zlibBrute);
	boolean("cso.7zip", cso.sevenZip);
	boolean("cso.zopfli", cso.zopfli);
	boolean("cso.libdeflate", cso.libdeflate);
	boolean("cso.lz4", cso.lz4);
	boolean("cso.lz4hc", cso.lz4hc);
	boolean("cso.lz4_brute", cso.lz4Brute);
	u32("cso.zopfli_splits", cso.zopfliSplits);
	dbl("cso.precheck", cso.precheck);
	boolean("cso.force_all", cso.forceAll);

	u32("chd.hunk", chd.hunkBytes);
	s.clear();
	str("chd.codecs", s);
	if (!s.empty())
		chd.SetCodecs(s);

	{
		uint32_t c = (uint32_t)pbp.compress;
		u32("pbp.compress", c);
		pbp.compress = c > 2 ? 1 : (int)c;
	}
	str("pbp.content_id", pbp.contentId);
	str("pbp.startdat", pbp.startDatPng);
	boolean("pbp.trim", pbp.trimOnExtract);
	boolean("pbp.verify_macs", pbp.verifyMacs);
	s.clear();
	str("pbp.layout", s);
	if (s == "file")
		pbp.layout = PbpLayout::File;
	else if (s == "folder")
		pbp.layout = PbpLayout::Folder;

	str("out.dir", out.dir);
	boolean("out.mirror", out.mirrorTree);
	s.clear();
	str("out.conflict", s);
	for (Conflict c : { Conflict::Skip, Conflict::Overwrite, Conflict::Rename })
		if (s == ConflictName(c))
			out.conflict = c;
	boolean("out.delete_source", out.deleteSource);

	boolean("engine.verify", engine.verify);
	boolean("engine.ram_repack", engine.repackInRam);
	s.clear();
	str("engine.ram_budget", s);
	if (!s.empty())
		ParseSize(s, &engine.ramBudget);
	str("engine.temp_dir", engine.tempDir);
	uns("engine.jobs", engine.jobs);
	uns("engine.threads", engine.threads);
	boolean("engine.low_priority", engine.lowPriority);
	boolean("recursive", recursive);
	if (engine.jobs == 0)
		engine.jobs = 1;
	return true;
}

bool Settings::Save(const std::string &path) const {
	std::error_code ec;
	fs::create_directories(U8Path(path).parent_path(), ec);
	std::string t;
	t += "# ISOPackager settings\n";
	t += Fmt("target=%s\n", FormatKey(target));
	t += Fmt("cso.preset=%s\ncso.block=%u\n", CsoPresetName(cso.preset), cso.blockSize);
	t += Fmt("cso.zlib=%d\ncso.zlib_brute=%d\ncso.7zip=%d\ncso.zopfli=%d\ncso.libdeflate=%d\n", cso.zlib, cso.zlibBrute, cso.sevenZip,
		cso.zopfli, cso.libdeflate);
	t += Fmt("cso.lz4=%d\ncso.lz4hc=%d\ncso.lz4_brute=%d\n", cso.lz4, cso.lz4hc, cso.lz4Brute);
	t += Fmt("cso.zopfli_splits=%u\ncso.precheck=%g\ncso.force_all=%d\n", cso.zopfliSplits, cso.precheck, cso.forceAll);
	t += Fmt("chd.hunk=%u\nchd.codecs=%s\n", chd.hunkBytes, chd.CodecsString().c_str());
	t += Fmt("pbp.compress=%d\npbp.content_id=%s\npbp.startdat=%s\npbp.trim=%d\npbp.verify_macs=%d\npbp.layout=%s\n", pbp.compress,
		pbp.contentId.c_str(), pbp.startDatPng.c_str(), pbp.trimOnExtract, pbp.verifyMacs, pbp.layout == PbpLayout::File ? "file" : "folder");
	t += Fmt("out.dir=%s\nout.mirror=%d\nout.conflict=%s\nout.delete_source=%d\n", out.dir.c_str(), out.mirrorTree, ConflictName(out.conflict),
		out.deleteSource);
	t += Fmt("engine.verify=%d\nengine.ram_repack=%d\nengine.ram_budget=%llu\nengine.temp_dir=%s\n", engine.verify, engine.repackInRam,
		(unsigned long long)engine.ramBudget, engine.tempDir.c_str());
	t += Fmt("engine.jobs=%u\nengine.threads=%u\nengine.low_priority=%d\nrecursive=%d\n", engine.jobs, engine.threads, engine.lowPriority,
		recursive);
	File f;
	if (!f.Open(path, File::Write))
		return false;
	return f.WriteAt(0, t.data(), t.size());
}

}  // namespace isopk
