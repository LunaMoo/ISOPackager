#include "core/bench.h"

#include <algorithm>
#include <memory>
#include <random>

#include <chdmodern/chdmodern.h>

#include "core/engine.h"
#include "core/image.h"
#include "core/sysinfo.h"
#include "core/util.h"

namespace isopk {

using namespace mcso_flags;

const std::vector<CsoBenchSet> &CsoBenchSets() {
	static const std::vector<CsoBenchSet> sets = {
		{ "A", "zopfli5 256K" },
		{ "B", "zopfli5 splits20 256K" },
		{ "C", "precheck98 + zopfli5 256K" },
		{ "D", "precheck98 + zopfli5 splits20 256K" },
		{ "L", "libdeflate12 256K" },
		{ "F", "fast preset" },
		{ "S", "standard preset" },
		{ "X", "max preset" },
	};
	return sets;
}

bool MakeCsoCase(const std::string &keyIn, BenchCase *out) {
	std::string key = keyIn;
	for (char &c : key)
		c = (char)toupper((unsigned char)c);
	BenchCase c;
	c.format = Format::CSO;
	CsoSettings &o = c.cso;
	o.preset = CsoPreset::Custom;
	o.zlib = o.zlibBrute = o.sevenZip = o.zopfli = o.libdeflate = false;
	o.lz4 = o.lz4hc = o.lz4Brute = false;
	o.blockSize = 262144;
	if (key == "A") {
		o.zopfli = true;
	} else if (key == "B") {
		o.zopfli = true;
		o.zopfliSplits = 20;
	} else if (key == "C") {
		o.zopfli = o.libdeflate = true;
		o.precheck = 98.0;
	} else if (key == "D") {
		o.zopfli = o.libdeflate = true;
		o.precheck = 98.0;
		o.zopfliSplits = 20;
	} else if (key == "L") {
		o.libdeflate = true;
	} else if (key == "F" || key == "S" || key == "X") {
		c.cso = CsoSettings();
		c.cso.preset = key == "F" ? CsoPreset::Fast : key == "S" ? CsoPreset::Standard : CsoPreset::Max;
	} else {
		return false;
	}
	for (auto &s : CsoBenchSets())
		if (key == s.key)
			c.label = std::string(s.key) + " " + s.name;
	*out = c;
	return true;
}

BenchCase MakeChdCase(const std::string &codecs, uint32_t hunk) {
	BenchCase c;
	c.format = Format::CHD;
	c.chd.SetCodecs(codecs);
	c.chd.hunkBytes = hunk;
	c.label = Fmt("%s hunk %u", c.chd.CodecsString().c_str(), c.chd.EffectiveHunk());
	return c;
}

BenchCase MakePbpCase(int compress) {
	BenchCase c;
	c.format = Format::PBP;
	c.pbp.compress = compress;
	c.label = c.pbp.Describe();
	return c;
}

BenchCase MakeMcsoCase(Format f, const CsoSettings &cso) {
	BenchCase c;
	c.format = f;
	c.cso = cso;
	c.label = cso.Describe(f);
	return c;
}

std::string DefaultBenchDir() {
	return PathU8(U8Path(AppDataDir()) / "bench");
}

std::string BenchCsvHeader() {
	return "game,format,setting,input_bytes,output_bytes,compress_s,decompress_s,random_read_ms,verify";
}

static std::string CsvQuote(const std::string &s) {
	std::string o = "\"";
	for (char c : s) {
		if (c == '"')
			o += '"';
		o += c;
	}
	return o + "\"";
}

std::string BenchCsvLine(const BenchRow &r) {
	return Fmt("%s,%s,%s,%llu,%llu,%.3f,%.3f,%.4f,%s", CsvQuote(r.game).c_str(), r.format.c_str(), CsvQuote(r.setting).c_str(),
		(unsigned long long)r.input, (unsigned long long)r.output, r.compSecs, r.decompSecs, r.readMs,
		CsvQuote(r.failed ? "FAILED: " + r.error : r.verify).c_str());
}

std::string BenchTableHeader() {
	return Fmt("    %-44s %14s %9s %9s %8s %9s %9s  %s", "setting", "output bytes", "of ISO", "comp s", "MB/s", "decomp s", "read ms", "verify");
}

std::string BenchTableLine(const BenchRow &r) {
	std::string name = r.format + " " + r.setting;
	if (r.failed)
		return Fmt("    %-44s FAILED: %s", name.c_str(), r.error.c_str());
	std::string rd = r.readMs >= 0 ? Fmt("%.3f", r.readMs) : "-";
	std::string dc = r.decompSecs > 0 ? Fmt("%.1f", r.decompSecs) : "-";
	return Fmt("    %-44s %14llu %8.3f%% %9.1f %8.1f %9s %9s  %s", name.c_str(), (unsigned long long)r.output, r.Ratio(), r.compSecs,
		r.CompMBs(), dc.c_str(), rd.c_str(), r.verify.c_str());
}

namespace {

// Appends lines to a results file (never overwrites earlier runs).
class Appender {
public:
	bool Open(const std::string &path) {
		if (!file_.Open(path, File::Update))
			return false;
		pos_ = file_.Size();
		return true;
	}
	void Write(const std::string &s) {
		if (file_.IsOpen() && file_.WriteAt(pos_, s.data(), s.size()))
			pos_ += s.size();
	}

private:
	File file_;
	uint64_t pos_ = 0;
};

double CsoRandomReadMs(const std::string &path, int reads) {
	CsoImageReader r;
	if (!r.Open(path) || r.Blocks() == 0)
		return -1;
	std::vector<uint32_t> order(r.Blocks());
	for (uint32_t i = 0; i < r.Blocks(); ++i)
		order[i] = i;
	std::mt19937 rng(12345);
	std::shuffle(order.begin(), order.end(), rng);
	int n = std::min<int>(reads, (int)order.size());
	std::vector<uint8_t> out(r.BlockSize());
	auto t0 = Clock::now();
	for (int i = 0; i < n; ++i)
		if (!r.ReadBlock(order[i], out.data()))
			return -1;
	return SecondsSince(t0) * 1000.0 / std::max(1, n);
}

double ChdRandomReadMs(const std::string &path, int reads) {
	ChdImageReader r;
	if (!r.Open(path))
		return -1;
	chdm_info info{};
	chdm_get_info(r.Handle(), &info);
	if (!info.hunk_count || !info.unit_bytes)
		return -1;
	uint32_t perHunk = std::max<uint32_t>(1, info.hunk_bytes / info.unit_bytes);
	std::vector<uint32_t> hunks(info.hunk_count);
	for (uint32_t i = 0; i < info.hunk_count; ++i)
		hunks[i] = i;
	std::mt19937 rng(12345);
	std::shuffle(hunks.begin(), hunks.end(), rng);
	int n = std::min<int>(reads, (int)hunks.size());
	std::vector<uint8_t> buf(info.unit_bytes);
	auto t0 = Clock::now();
	for (int i = 0; i < n; ++i) {
		uint64_t unit = (uint64_t)hunks[i] * perHunk + rng() % perHunk;
		if (unit >= info.unit_count)
			unit = info.unit_count - 1;
		if (chdm_read_units(r.Handle(), unit, buf.data(), 1) != CHDM_OK)
			return -1;
	}
	return SecondsSince(t0) * 1000.0 / std::max(1, n);
}

}  // namespace

std::vector<BenchRow> RunBenchmark(const std::vector<InputFile> &inputs, const BenchConfig &cfg, const BenchCallbacks &cb,
	const std::atomic<bool> *cancel) {
	std::vector<BenchRow> rows;
	const fs::path work = U8Path(cfg.workDir.empty() ? DefaultBenchDir() : cfg.workDir);
	const fs::path tmp = work / "tmp";
	std::error_code ec;
	fs::create_directories(tmp, ec);
	if (ec) {
		Log(LogLevel::Error, "benchmark: can't create " + PathU8(tmp) + ": " + ec.message());
		return rows;
	}

	Appender summary, csv;
	const std::string csvPath = PathU8(work / "results.csv");
	bool newCsv = !fs::exists(U8Path(csvPath), ec) || fs::file_size(U8Path(csvPath), ec) == 0;
	summary.Open(PathU8(work / "summary.txt"));
	csv.Open(csvPath);
	if (newCsv)
		csv.Write(BenchCsvHeader() + "\n");
	auto log = [&](const std::string &line) {
		Log(LogLevel::Info, line);
		summary.Write(line + "\n");
	};

	log("");
	log(Fmt("=== ISOPackager %s benchmark, %s, %s ===", ISOPK_VERSION, Timestamp().c_str(), PlatformName().c_str()));
	log(Fmt("%s; %u cores, %s RAM; verify %s, %d random reads, source %s", LibraryVersions().c_str(), CpuCount(),
		HumanBytes(TotalRam()).c_str(), cfg.verify ? "on" : "off", cfg.reads, cfg.sourceInRam ? "in RAM" : "on disk"));
	log("Columns: output bytes, of ISO, compress s, compress MB/s, decode s, ms per random read of a new block or hunk, verify");

	const size_t totalSteps = std::max<size_t>(1, inputs.size() * cfg.cases.size());
	size_t step = 0;

	for (const InputFile &in : inputs) {
		if (cancel && cancel->load())
			break;
		const std::string game = GameName(in.path);
		Format S = in.format == Format::Unknown ? DetectFormat(in.path) : in.format;
		Settings base;
		base.engine.threads = cfg.threads;
		base.pbp.verifyMacs = true;

		// The plain image every setting starts from.
		std::unique_ptr<MemoryImage> mem;
		std::string srcIso = S == Format::ISO ? in.path : std::string();
		std::string tempIso;
		OpContext prep;
		prep.cancel = cancel;
		prep.progress = [&](double f) {
			if (cb.progress)
				cb.progress(game + ": preparing the source image", f, (double)step / totalSteps);
		};
		if (cfg.sourceInRam || S != Format::ISO) {
			mem = std::make_unique<MemoryImage>(cancel);
			bool ok = S == Format::ISO ? mem->LoadFile(in.path) : DecompressToWriter(in.path, S, base, *mem, true, prep);
			if (!ok) {
				if (cancel && cancel->load())
					break;
				Log(LogLevel::Warning, game + ": not in RAM (" + (mem->Error().empty() ? prep.error : mem->Error()) + ")");
				mem.reset();
				prep.error.clear();
			}
		}
		if (!mem && S != Format::ISO) {
			tempIso = PathU8(tmp / "source.iso");
			if (!DecompressToFile(in.path, S, base, tempIso, true, prep)) {
				Log(LogLevel::Error, game + ": can't extract the source: " + prep.error);
				fs::remove(U8Path(tempIso), ec);
				step += cfg.cases.size();
				continue;
			}
			srcIso = tempIso;
		}
		FileImageReader fileRef;
		ImageReader *ref = mem.get();
		if (!ref) {
			if (!fileRef.Open(srcIso)) {
				Log(LogLevel::Error, game + ": " + fileRef.Error());
				step += cfg.cases.size();
				continue;
			}
			ref = &fileRef;
		}
		const uint64_t size = ref->Size();

		uint64_t freeSpace = FreeDiskSpace(PathU8(tmp));
		if (freeSpace != UINT64_MAX && freeSpace < size + (256ull << 20))
			Log(LogLevel::Warning, Fmt("only %s free in %s; up to %s is needed", HumanBytes(freeSpace).c_str(), PathU8(tmp).c_str(),
				HumanBytes(size).c_str()));

		log("");
		log(Fmt("=== %s (%s, %.0f MB) ===", PathU8(U8Path(in.path).filename()).c_str(), FormatName(S), size / 1e6));
		log(BenchTableHeader());

		uint64_t lzrcSize = 0;
		for (const BenchCase &bc : cfg.cases) {
			if (cancel && cancel->load())
				break;
			BenchRow r;
			r.game = game;
			r.format = FormatName(bc.format);
			r.setting = bc.label;
			r.input = size;

			Settings s = base;
			s.cso = bc.cso;
			s.chd = bc.chd;
			s.pbp = bc.pbp;
			s.pbp.verifyMacs = true;
			const std::string out = PathU8(tmp / U8Path(std::string("test") + FormatExtension(bc.format)));
			fs::remove(U8Path(out), ec);

			const std::string what = game + ": " + r.format + " " + r.setting;
			OpContext ctx;
			ctx.cancel = cancel;
			ctx.progress = [&](double f) {
				if (cb.progress)
					cb.progress(what + " - compressing", f, (step + f * 0.7) / totalSteps);
			};
			auto t0 = Clock::now();
			bool ok = mem ? CompressImage(*mem, bc.format, s, out, ctx) : CompressFile(srcIso, bc.format, s, out, ctx);
			r.compSecs = SecondsSince(t0);
			if (!ok) {
				if (ctx.cancelled)
					break;
				r.failed = true;
				r.error = ctx.error;
			} else {
				r.output = fs::file_size(U8Path(out), ec);
				if (cb.progress)
					cb.progress(what + " - random reads", 0, (step + 0.7) / totalSteps);
				if (bc.format == Format::CHD)
					r.readMs = ChdRandomReadMs(out, cfg.reads);
				else if (bc.format == Format::CSO || bc.format == Format::CSO2 || bc.format == Format::ZSO)
					r.readMs = CsoRandomReadMs(out, cfg.reads);

				OpContext dctx;
				dctx.cancel = cancel;
				dctx.progress = [&](double f) {
					if (cb.progress)
						cb.progress(what + (cfg.verify ? " - decoding and comparing" : " - decoding"), f, (step + 0.75 + f * 0.25) / totalSteps);
				};
				t0 = Clock::now();
				if (cfg.verify) {
					CompareWriter cmp(ref);
					bool dok = DecompressToWriter(out, bc.format, s, cmp, false, dctx);
					r.decompSecs = SecondsSince(t0);
					r.verify = dok ? cmp.Result() : "decode failed: " + dctx.error;
				} else {
					NullWriter nw;
					bool dok = DecompressToWriter(out, bc.format, s, nw, false, dctx);
					r.decompSecs = SecondsSince(t0);
					r.verify = dok ? "not compared" : "decode failed: " + dctx.error;
				}
				if (dctx.cancelled) {
					fs::remove(U8Path(out), ec);
					break;
				}
			}
			fs::remove(U8Path(out), ec);
			if (bc.format == Format::PBP && bc.pbp.compress == 1)
				lzrcSize = r.output;
			if (bc.format == Format::PBP && bc.pbp.compress == 2 && !r.failed && lzrcSize && r.output == lzrcSize)
				r.verify += " (same size as LZRC)";
			log(BenchTableLine(r));
			csv.Write(BenchCsvLine(r) + "\n");
			rows.push_back(r);
			if (cb.row)
				cb.row(r);
			++step;
		}
		mem.reset();
		if (!tempIso.empty())
			fs::remove(U8Path(tempIso), ec);
	}
	if (cancel && cancel->load())
		log("Benchmark cancelled.");
	else
		log(Fmt("Done. Results: %s", PathU8(work).c_str()));
	fs::remove(tmp, ec);
	if (cb.progress)
		cb.progress("", 1.0, 1.0);
	return rows;
}

}  // namespace isopk
