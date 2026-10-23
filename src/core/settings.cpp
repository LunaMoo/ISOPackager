#include "core/settings.h"

#include <algorithm>
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
	if (useDefault) {
		if (zso)
			flags = NO_ALL & ~(NO_LZ4_DEFAULT | NO_LZ4_HC);  // lz4 has no libdeflate: plain lz4 + lz4 HC
		else
			flags = NO_ALL & ~NO_LIBDEFLATE;
		return flags | (v2 ? FMT_CSO_2 : zso ? FMT_ZSO : f == Format::DAX ? FMT_DAX : 0);
	}
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
	if (f == Format::DAX)
		return 0xFFFFFFFFu;
	if (useDefault)
		return DEFAULT_BLOCK;
	if (blockSize == 0)
		return 0xFFFFFFFFu;  // automatic (DAX has a fixed frame size)
	return blockSize;
}

std::string CsoSettings::Describe(Format f) const {
	if (useDefault)
		return f == Format::ZSO ? "default (lz4 HC 64K)" : f == Format::DAX ? "default (libdeflate)" : "default (libdeflate 64K)";
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
	std::replace(l.begin(), l.end(), ',', '+');
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
	const uint32_t h = EffectiveHunk();
	return Fmt("%s hunk %s", CodecsString().c_str(), (h % 1024 ? Fmt("%u", h) : Fmt("%uK", h / 1024)).c_str());
}

std::string PsxSettings::Describe() const {
	return Fmt("deflate %d%s", level, basePbp.empty() ? "" : ", with a base PBP");
}

std::vector<uint32_t> CdCodecsFor(const ChdSettings &c) {
	std::vector<uint32_t> out;
	for (uint32_t codec : c.codecs) {
		uint32_t cd = 0;
		if (codec == FourCC("lzma")) cd = FourCC("cdlz");
		else if (codec == FourCC("zlib")) cd = FourCC("cdzl");
		else if (codec == FourCC("zstd")) cd = FourCC("cdzs");
		else if (codec == FourCC("flac")) cd = FourCC("cdfl");
		if (cd && std::find(out.begin(), out.end(), cd) == out.end())
			out.push_back(cd);
	}
	return out;
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

const char *RvzMethodName(int m) {
	switch (m) {
	case RVZ_NONE: return "none";
	case RVZ_PURGE: return "purge";
	case RVZ_BZIP2: return "bzip2";
	case RVZ_LZMA: return "lzma";
	case RVZ_LZMA2: return "lzma2";
	case RVZ_ZSTD: return "zstd";
	default: return "?";
	}
}

void RvzMethodLevels(int m, int *min, int *max) {
	if (m == RVZ_BZIP2 || m == RVZ_LZMA || m == RVZ_LZMA2) {
		*min = 1;
		*max = 9;
	} else if (m == RVZ_ZSTD) {
		*min = -22;
		*max = 22;
	} else {
		*min = 1;
		*max = 0;
	}
}

bool RvzMethodAllowed(Format f, int m) {
	if (m < RVZ_NONE || m > RVZ_ZSTD)
		return false;
	if (f == Format::WIA)
		return m != RVZ_ZSTD;
	return m != RVZ_PURGE;  // RVZ
}

int RvzSettings::EffectiveMethod(Format f) const {
	int m = useDefault ? RVZ_LZMA2 : method;
	if (!RvzMethodAllowed(f, m))
		m = f == Format::WIA ? RVZ_LZMA2 : (m == RVZ_PURGE ? RVZ_NONE : RVZ_LZMA2);
	return m;
}

int RvzSettings::EffectiveLevel(Format f) const {
	int lo, hi;
	RvzMethodLevels(EffectiveMethod(f), &lo, &hi);
	if (lo > hi)
		return 0;
	return useDefault ? 5 : std::clamp(level, lo, hi);
}

uint32_t RvzSettings::EffectiveChunk(Format f) const {
	const uint32_t twoMB = 2u << 20;
	uint32_t c = useDefault ? 0 : chunk;
	if (c == 0)
		return f == Format::WIA ? twoMB : 128u << 10;
	if (f == Format::WIA)  // multiples of 2M only
		return std::max(twoMB, (c + twoMB - 1) / twoMB * twoMB);
	if (c < (32u << 10))
		return 32u << 10;
	if (c < twoMB) {  // power of two
		uint32_t p = 32u << 10;
		while (p < c)
			p <<= 1;
		return p;
	}
	return (c + twoMB - 1) / twoMB * twoMB;
}

std::string ZarSettings::Describe() const {
	std::string s = Fmt("zstd %d", level);
	if (precheck > 0)
		s += Fmt(", pre-check %g%%", precheck);
	if (compressDiscs)
		s += Fmt(", discs to %s first", FormatName(discFormat));
	if (storeDiscs)
		s += ", discs stored";
	return s;
}

std::string RvzSettings::Describe(Format f) const {
	const int m = EffectiveMethod(f);
	std::string s = useDefault ? "default (" : "";
	s += RvzMethodName(m);
	int lo, hi;
	RvzMethodLevels(m, &lo, &hi);
	if (lo <= hi)
		s += Fmt(" %d", EffectiveLevel(f));
	const uint32_t c = EffectiveChunk(f);
	s += c >= (1u << 20) ? Fmt(" %uM", c >> 20) : Fmt(" %uK", c >> 10);
	if (useDefault)
		s += ")";
	return s;
}

const char *ConflictName(Conflict c) {
	switch (c) {
	case Conflict::Overwrite: return "overwrite";
	case Conflict::Rename: return "rename";
	default: return "skip";
	}
}

CsoSettings &Settings::Cso(Format f) {
	return f == Format::ZSO ? zso : f == Format::CSO2 ? cso2 : f == Format::DAX ? dax : cso;
}

const CsoSettings &Settings::Cso(Format f) const {
	return const_cast<Settings *>(this)->Cso(f);
}

RvzSettings &Settings::Rvz(Format f) {
	return f == Format::WIA ? wia : rvz;
}

const RvzSettings &Settings::Rvz(Format f) const {
	return const_cast<Settings *>(this)->Rvz(f);
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
	for (Format f : { Format::CSO, Format::ZSO, Format::CSO2, Format::DAX }) {
		CsoSettings &c = Cso(f);
		const std::string k = std::string(FormatKey(f)) + ".";
		s.clear();
		str((k + "preset").c_str(), s);
		for (CsoPreset p : { CsoPreset::Fast, CsoPreset::Standard, CsoPreset::Max, CsoPreset::Custom })
			if (s == CsoPresetName(p))
				c.preset = p;
		boolean((k + "default").c_str(), c.useDefault);
		u32((k + "block").c_str(), c.blockSize);
		boolean((k + "zlib").c_str(), c.zlib);
		boolean((k + "zlib_brute").c_str(), c.zlibBrute);
		boolean((k + "7zip").c_str(), c.sevenZip);
		boolean((k + "zopfli").c_str(), c.zopfli);
		boolean((k + "libdeflate").c_str(), c.libdeflate);
		boolean((k + "lz4").c_str(), c.lz4);
		boolean((k + "lz4hc").c_str(), c.lz4hc);
		boolean((k + "lz4_brute").c_str(), c.lz4Brute);
		u32((k + "zopfli_splits").c_str(), c.zopfliSplits);
		dbl((k + "precheck").c_str(), c.precheck);
		boolean((k + "force_all").c_str(), c.forceAll);
	}

	u32("chd.hunk", chd.hunkBytes);
	// Settings written before version 2 saved chdman's default (0 = 4K) as the default hunk; the
	// default is 2K now.
	{
		uint32_t version = 1;
		u32("settings.version", version);
		if (version < 2 && chd.hunkBytes == 0)
			chd.hunkBytes = 2048;
	}
	s.clear();
	str("chd.codecs", s);
	if (!s.empty())
		chd.SetCodecs(s);

	boolean("cue.split", cue.splitBins);
	{
		uint32_t l = (uint32_t)psx.level;
		u32("psx.level", l);
		psx.level = l > 9 ? 9 : (int)l;
	}
	str("psx.base", psx.basePbp);
	str("psx.boot", psx.bootPng);
	boolean("psx.group_discs", psx.groupDiscs);

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

	for (Format f : { Format::RVZ, Format::WIA }) {
		RvzSettings &r = Rvz(f);
		const std::string k = std::string(FormatKey(f)) + ".";
		boolean((k + "default").c_str(), r.useDefault);
		s.clear();
		str((k + "method").c_str(), s);
		for (int m = RVZ_NONE; m <= RVZ_ZSTD; ++m)
			if (s == RvzMethodName(m) && RvzMethodAllowed(f, m))
				r.method = m;
		double lvl = r.level;
		dbl((k + "level").c_str(), lvl);
		r.level = (int)lvl;
		u32((k + "chunk").c_str(), r.chunk);
	}
	{
		double v = zar.level;
		dbl("zar.level", v);
		zar.level = std::clamp((int)v, 1, 22);
	}
	dbl("zar.precheck", zar.precheck);
	zar.precheck = std::clamp(zar.precheck, 0.0, 100.0);
	boolean("zar.store_discs", zar.storeDiscs);
	boolean("zar.compress_discs", zar.compressDiscs);
	s.clear();
	str("zar.disc_format", s);
	if (Format f = FormatFromName(s); f != Format::Unknown && f != Format::ISO && f != Format::ZAR && FormatAvailable(f))
		zar.discFormat = f;
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
	if (kv.count("bench.cases"))  // present even when empty: the user removed them all
		benchCases = Split(kv["bench.cases"], '|');
	{
		uint32_t t = (uint32_t)uiTheme;
		u32("ui.theme", t);
		uiTheme = t > 2 ? 0 : (int)t;
		double sc = uiScale;
		dbl("ui.scale", sc);
		uiScale = sc < 0.5 || sc > 4 ? 1.0f : (float)sc;
		uint32_t l = (uint32_t)uiLayout;
		u32("ui.layout", l);
		uiLayout = l > 2 ? 0 : (int)l;
	}
	if (engine.jobs == 0)
		engine.jobs = 1;
	return true;
}

bool Settings::Save(const std::string &path) const {
	std::error_code ec;
	fs::create_directories(U8Path(path).parent_path(), ec);
	std::string t;
	t += "# ISOPackager settings\n";
	t += "settings.version=2\n";
	t += Fmt("target=%s\n", FormatKey(target));
	for (Format f : { Format::CSO, Format::ZSO, Format::CSO2, Format::DAX }) {
		const CsoSettings &c = Cso(f);
		const char *k = FormatKey(f);
		t += Fmt("%s.default=%d\n%s.preset=%s\n%s.block=%u\n", k, c.useDefault, k, CsoPresetName(c.preset), k, c.blockSize);
		t += Fmt("%s.zlib=%d\n%s.zlib_brute=%d\n%s.7zip=%d\n%s.zopfli=%d\n%s.libdeflate=%d\n", k, c.zlib, k, c.zlibBrute, k, c.sevenZip, k,
			c.zopfli, k, c.libdeflate);
		t += Fmt("%s.lz4=%d\n%s.lz4hc=%d\n%s.lz4_brute=%d\n", k, c.lz4, k, c.lz4hc, k, c.lz4Brute);
		t += Fmt("%s.zopfli_splits=%u\n%s.precheck=%g\n%s.force_all=%d\n", k, c.zopfliSplits, k, c.precheck, k, c.forceAll);
	}
	t += Fmt("chd.hunk=%u\nchd.codecs=%s\n", chd.hunkBytes, chd.CodecsString().c_str());
	t += Fmt("cue.split=%d\n", cue.splitBins);
	t += Fmt("psx.level=%d\npsx.base=%s\npsx.boot=%s\npsx.group_discs=%d\n", psx.level, psx.basePbp.c_str(), psx.bootPng.c_str(), psx.groupDiscs);
	t += Fmt("pbp.compress=%d\npbp.content_id=%s\npbp.startdat=%s\npbp.trim=%d\npbp.verify_macs=%d\npbp.layout=%s\n", pbp.compress,
		pbp.contentId.c_str(), pbp.startDatPng.c_str(), pbp.trimOnExtract, pbp.verifyMacs, pbp.layout == PbpLayout::File ? "file" : "folder");
	for (Format f : { Format::RVZ, Format::WIA }) {
		const RvzSettings &r = Rvz(f);
		const char *k = FormatKey(f);
		t += Fmt("%s.default=%d\n%s.method=%s\n%s.level=%d\n%s.chunk=%u\n", k, r.useDefault, k, RvzMethodName(r.method), k, r.level, k, r.chunk);
	}
	t += Fmt("zar.level=%d\nzar.precheck=%g\nzar.store_discs=%d\nzar.compress_discs=%d\nzar.disc_format=%s\n", zar.level, zar.precheck,
		zar.storeDiscs, zar.compressDiscs, FormatKey(zar.discFormat));
	t += Fmt("out.dir=%s\nout.mirror=%d\nout.conflict=%s\nout.delete_source=%d\n", out.dir.c_str(), out.mirrorTree, ConflictName(out.conflict),
		out.deleteSource);
	t += Fmt("engine.verify=%d\nengine.ram_repack=%d\nengine.ram_budget=%llu\nengine.temp_dir=%s\n", engine.verify, engine.repackInRam,
		(unsigned long long)engine.ramBudget, engine.tempDir.c_str());
	t += Fmt("engine.jobs=%u\nengine.threads=%u\nengine.low_priority=%d\nrecursive=%d\n", engine.jobs, engine.threads, engine.lowPriority,
		recursive);
	t += Fmt("ui.theme=%d\nui.scale=%.2f\nui.layout=%d\n", uiTheme, uiScale, uiLayout);
	{
		std::string cases;
		for (const std::string &c : benchCases)
			cases += (cases.empty() ? "" : "|") + c;
		t += "bench.cases=" + cases + "\n";
	}
	File f;
	if (!f.Open(path, File::Write))
		return false;
	return f.WriteAt(0, t.data(), t.size());
}

}  // namespace isopk
