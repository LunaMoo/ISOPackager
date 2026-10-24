// Round trips through every format and path the engine has, on a generated PSP image:
//   ISO -> every format (verified), every format -> ISO (compared with the original),
//   compressed -> compressed through RAM and through a temporary ISO, parallel jobs in the queue,
//   disc ID / title detection, and a small benchmark.
// Usage: isopk_selftest <scratch folder>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <chdmodern/chdmodern.h>

#include "core/bench.h"
#include "core/engine.h"
#include "core/fileio.h"
#include "core/queue.h"
#include "core/util.h"
#include "core/zar.h"
#include "test_iso.h"

#include <zarchive/zarchivereader.h>

using namespace isopk;

static int g_failures = 0;

#define CHECK(cond, ...)                                     \
	do {                                                     \
		if (!(cond)) {                                       \
			++g_failures;                                    \
			printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                             \
			printf("\n");                                    \
		}                                                    \
	} while (0)

static bool SameBytes(const std::string &a, const std::vector<uint8_t> &ref, std::string *why) {
	File f;
	if (!f.Open(a, File::Read)) {
		*why = "can't open " + a;
		return false;
	}
	if (f.Size() != ref.size()) {
		*why = Fmt("size %llu, expected %zu", (unsigned long long)f.Size(), ref.size());
		return false;
	}
	std::vector<uint8_t> buf(ref.size());
	if (!f.ReadExact(0, buf.data(), buf.size()) || memcmp(buf.data(), ref.data(), ref.size()) != 0) {
		*why = "content differs";
		return false;
	}
	return true;
}

static JobResult Convert(const std::string &input, Format target, Settings s, const std::string &outDir) {
	Job job;
	job.input.path = input;
	job.input.format = DetectFormat(input);
	job.input.size = fs::file_size(U8Path(input));
	job.target = target;
	s.out.dir = outDir;
	s.out.conflict = Conflict::Overwrite;
	job.settings = s;
	RunJob(job);
	return job.Result();
}

#ifdef ISOPK_CLI_EXE
#ifndef _WIN32
#include <sys/wait.h>
#endif
// Runs isopackager-cli with args (ASCII paths only: std::system takes the ANSI code page on
// Windows). Returns its exit code; its output goes to log.
static int Cli(const std::string &args, const std::string &log) {
	std::string cmd = std::string("\"") + ISOPK_CLI_EXE + "\" " + args + " > \"" + log + "\" 2>&1";
#ifdef _WIN32
	cmd = "\"" + cmd + "\"";  // cmd.exe drops the outer quotes
	return std::system(cmd.c_str());
#else
	int st = std::system(cmd.c_str());
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}
#endif

static JobResult MakeZar(const ZarSpec &specIn, Settings s, const std::string &outDir) {
	ZarSpec spec = specIn;
	std::string err;
	CHECK(MeasureZarSpec(&spec, &err), "measure: %s", err.c_str());
	Job job;
	job.zar = std::make_shared<const ZarSpec>(spec);
	job.input.path = spec.Paths().front();
	job.input.size = spec.totalSize;
	job.target = Format::ZAR;
	s.out.dir = outDir;
	s.out.conflict = Conflict::Overwrite;
	job.settings = s;
	RunJob(job);
	return job.Result();
}

// Reads one file out of an archive.
static std::vector<uint8_t> ZarFile(const std::string &zar, const std::string &name, bool *found) {
	std::unique_ptr<ZArchiveReader> r(ZArchiveReader::OpenFromFile(U8Path(zar)));
	*found = false;
	std::vector<uint8_t> out;
	if (!r)
		return out;
	ZArchiveNodeHandle h = r->LookUp(name, true, false);
	if (h == ZARCHIVE_INVALID_NODE)
		return out;
	*found = true;
	out.resize((size_t)r->GetFileSize(h));
	if (!out.empty() && r->ReadFromFile(h, 0, out.size(), out.data()) != out.size())
		out.clear();
	return out;
}

static std::vector<uint8_t> ReadAll(const std::string &path) {
	File f;
	std::vector<uint8_t> v;
	if (f.Open(path, File::Read)) {
		v.resize((size_t)f.Size());
		if (!v.empty() && !f.ReadExact(0, v.data(), v.size()))
			v.clear();
	}
	return v;
}

static void WriteAll(const std::string &path, const std::vector<uint8_t> &data) {
	File f;
	CHECK(f.Open(path, File::Write) && (data.empty() || f.WriteAt(0, data.data(), data.size())), "writing %s", path.c_str());
}

int main(int argc, char **argv) {
	fs::path root = argc > 1 ? U8Path(argv[1]) : fs::temp_directory_path() / "isopk_selftest";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root, ec);
	SetLogSink([](LogLevel l, const std::string &m) {
		if (l >= LogLevel::Warning)
			printf("  [%s] %s\n", l == LogLevel::Error ? "error" : "warn", m.c_str());
	});
	printf("ISOPackager self-test, %s\n", LibraryVersions().c_str());

	// A 12 MB "game" (non-ASCII folder name to exercise UTF-8 paths).
	std::vector<uint8_t> iso = testiso::MakeIso(12);
	fs::path srcDir = root / U8Path("źródło");
	fs::create_directories(srcDir, ec);
	const std::string isoPath = PathU8(srcDir / "Test Game.iso");
	{
		File f;
		CHECK(f.Open(isoPath, File::Write) && f.WriteAt(0, iso.data(), iso.size()), "writing the test ISO");
	}

	Settings base;
	base.engine.verify = true;
	base.engine.ramBudget = 512ull << 20;
	ApplyEngineSettings(base.engine);
	base.pbp.layout = PbpLayout::File;

	ImageInfo info = InspectImage(isoPath);
	CHECK(info.format == Format::ISO, "format %s", FormatName(info.format));
	CHECK(info.discId == "ULUS99999", "disc ID '%s'", info.discId.c_str());
	CHECK(info.title == "ISOPackager Test", "title '%s'", info.title.c_str());

	struct Target {
		Format f;
		const char *label;
		Settings s;
	};
	std::vector<Target> targets;
	auto add = [&](Format f, const char *label, auto tweak) {
		Settings s = base;
		tweak(s);
		targets.push_back({ f, label, s });
	};
#ifdef ISOPK_WITH_CSO
	add(Format::CSO, "cso default", [](Settings &) {});
	add(Format::CSO, "cso standard", [](Settings &s) { s.cso.useDefault = false; });
	add(Format::CSO, "cso custom 256K libdeflate", [](Settings &s) {
		s.cso.useDefault = false;
		s.cso.preset = CsoPreset::Custom;
		s.cso.zlib = s.cso.zlibBrute = s.cso.sevenZip = false;
		s.cso.libdeflate = true;
		s.cso.blockSize = 262144;
	});
	add(Format::ZSO, "zso", [](Settings &) {});
	add(Format::CSO2, "cso2 fast", [](Settings &s) {
		s.cso2.useDefault = false;
		s.cso2.preset = CsoPreset::Fast;
	});
	add(Format::DAX, "dax fast", [](Settings &s) {
		s.dax.useDefault = false;
		s.dax.preset = CsoPreset::Fast;
	});
#endif
	add(Format::CHD, "chd default", [](Settings &) {});
	add(Format::CHD, "chd zstd 16K", [](Settings &s) {
		s.chd.SetCodecs("zstd");
		s.chd.hunkBytes = 16384;
	});
	add(Format::PBP, "pbp lzrc", [](Settings &) {});
	add(Format::PBP, "pbp none", [](Settings &s) { s.pbp.compress = 0; });

	std::vector<std::string> outputs;
	for (size_t i = 0; i < targets.size(); ++i) {
		const Target &t = targets[i];
		std::string dir = PathU8(root / Fmt("out%zu", i));
		JobResult r = Convert(isoPath, t.f, t.s, dir);
		CHECK(r.state == JobState::Done, "ISO -> %s: %s %s", t.label, JobStateName(r.state), r.message.c_str());
		CHECK(r.verify == "OK", "ISO -> %s: verify %s", t.label, r.verify.c_str());
		printf("  ISO -> %-28s %10llu bytes (%5.1f%%) %s\n", t.label, (unsigned long long)r.outputSize, 100.0 * r.outputSize / iso.size(),
			r.verify.c_str());
		outputs.push_back(r.outputPath);

		if (r.state != JobState::Done)
			continue;
		ImageInfo oi = InspectImage(r.outputPath);
		CHECK(oi.format == t.f, "%s detected as %s", t.label, FormatName(oi.format));
		CHECK(oi.isoSize >= iso.size(), "%s: image size %llu", t.label, (unsigned long long)oi.isoSize);
		if (t.f != Format::DAX)
			CHECK(oi.discId == "ULUS99999", "%s: disc ID '%s'", t.label, oi.discId.c_str());

		// Back to ISO: must equal the original exactly.
		std::string back = PathU8(root / Fmt("back%zu", i));
		JobResult x = Convert(r.outputPath, Format::ISO, t.s, back);
		CHECK(x.state == JobState::Done && x.verify == "OK", "%s -> ISO: %s %s / %s", t.label, JobStateName(x.state), x.message.c_str(),
			x.verify.c_str());
		std::string why;
		CHECK(SameBytes(x.outputPath, iso, &why), "%s -> ISO differs from the original: %s", t.label, why.c_str());
	}

	// Repack between formats: each output to the next target's format, through RAM and through a temp ISO.
	for (int viaRam = 1; viaRam >= 0; --viaRam) {
		for (size_t i = 0; i + 1 < targets.size(); ++i) {
			if (outputs[i].empty())
				continue;
			Settings s = targets[i + 1].s;
			s.engine.repackInRam = viaRam != 0;
			std::string dir = PathU8(root / Fmt("repack%d_%zu", viaRam, i));
			JobResult r = Convert(outputs[i], targets[i + 1].f, s, dir);
			CHECK(r.state == JobState::Done && r.verify == "OK", "repack %s -> %s (%s): %s %s / %s", targets[i].label, targets[i + 1].label,
				viaRam ? "RAM" : "temp ISO", JobStateName(r.state), r.message.c_str(), r.verify.c_str());
			CHECK(viaRam ? r.viaRam : r.viaTempFile, "repack %s -> %s didn't go through %s", targets[i].label, targets[i + 1].label,
				viaRam ? "RAM" : "a temp ISO");
			if (r.state == JobState::Done) {
				std::string back = PathU8(root / Fmt("repackback%d_%zu", viaRam, i));
				JobResult x = Convert(r.outputPath, Format::ISO, s, back);
				std::string why;
				CHECK(x.state == JobState::Done && SameBytes(x.outputPath, iso, &why), "repacked %s -> ISO: %s", targets[i + 1].label,
					why.c_str());
			}
		}
	}
	printf("  repacks done\n");

	// A RAM budget too small for the image: must fall back to a temporary ISO.
	{
		Settings s = base;
		s.engine.ramBudget = 1 << 20;
		ApplyEngineSettings(s.engine);
		JobResult r = Convert(outputs.back(), Format::CHD, s, PathU8(root / "smallram"));
		CHECK(r.state == JobState::Done && r.viaTempFile && !r.viaRam, "small RAM budget: %s %s", JobStateName(r.state), r.message.c_str());
		ApplyEngineSettings(base.engine);
	}

	// The queue: several jobs at once, two of them mCSO (serialised inside), folder scan + mirroring.
	{
		Settings s = base;
		s.engine.jobs = 4;
		s.out.dir = PathU8(root / "queue");
		s.out.conflict = Conflict::Rename;
		JobQueue q;
		q.SetSettings(s);
		std::vector<std::string> warnings;
		auto files = ScanInputs({ PathU8(srcDir) }, true, &warnings);
		CHECK(files.size() == 1, "scan found %zu files", files.size());
		std::vector<Format> fmts = { Format::CHD, Format::PBP };
#ifdef ISOPK_WITH_CSO
		fmts.push_back(Format::CSO);
		fmts.push_back(Format::ZSO);
#endif
		for (Format f : fmts)
			q.Add(files[0], f);
		q.Add(files[0], Format::CHD);  // same output again: renamed
		q.Start();
		q.WaitIdle();
		auto c = q.Count();
		CHECK(c.done == (int)fmts.size() + 1, "queue: %d done, %d failed", c.done, c.failed);
		for (auto &j : q.Jobs())
			if (j->State() != JobState::Done)
				printf("    %s -> %s: %s\n", j->input.path.c_str(), FormatName(j->target), j->Result().message.c_str());
		CHECK(fs::exists(root / "queue" / "Test Game (2).chd"), "renamed output missing");
	}

	// Cancelling a running job.
	{
		Job job;
		job.input.path = isoPath;
		job.input.format = Format::ISO;
		job.target = Format::PBP;
		job.settings = base;
		job.settings.pbp.compress = 2;
		job.settings.out.dir = PathU8(root / "cancel");
		std::thread t([&] { RunJob(job); });
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		job.cancel = true;
		t.join();
		CHECK(job.State() == JobState::Cancelled || job.State() == JobState::Done, "cancel: %s", JobStateName(job.State()));
		CHECK(!fs::exists(root / "cancel" / "Test Game.pbp.part"), "partial output left behind");
	}

	// GameCube disc: RVZ and WIA with several methods, back to ISO byte for byte, repacks
	// through RAM to and from CHD, and the platform rules.
	{
		std::vector<uint8_t> gc = testiso::MakeGameCubeDisc(8);
		const std::string gcPath = PathU8(srcDir / "GC Game.iso");
		{
			File f;
			CHECK(f.Open(gcPath, File::Write) && f.WriteAt(0, gc.data(), gc.size()), "writing the GameCube disc");
		}
		ImageInfo gi = InspectImage(gcPath);
		CHECK(gi.format == Format::ISO && gi.platform == Platform::GameCube && gi.discId == "GTST01" && gi.title == "ISOPackager GC Test",
			"GameCube detection: %s %s '%s' '%s'", FormatName(gi.format), PlatformLabel(gi.platform), gi.discId.c_str(), gi.title.c_str());

		struct R {
			Format f;
			const char *label;
			Settings s;
		};
		std::vector<R> rv;
		auto addr = [&](Format f, const char *label, auto tweak) {
			Settings s = base;
			tweak(s);
			rv.push_back({ f, label, s });
		};
		addr(Format::RVZ, "rvz default (lzma2)", [](Settings &) {});
		addr(Format::RVZ, "rvz zstd 19 256K", [](Settings &s) {
			s.rvz.useDefault = false;
			s.rvz.method = RVZ_ZSTD;
			s.rvz.level = 19;
			s.rvz.chunk = 256 << 10;
		});
		addr(Format::RVZ, "rvz bzip2 2M", [](Settings &s) {
			s.rvz.useDefault = false;
			s.rvz.method = RVZ_BZIP2;
			s.rvz.chunk = 2 << 20;
		});
		addr(Format::WIA, "wia default (lzma2)", [](Settings &) {});
		addr(Format::WIA, "wia purge", [](Settings &s) {
			s.wia.useDefault = false;
			s.wia.method = RVZ_PURGE;
		});
		addr(Format::WIA, "wia lzma 4M", [](Settings &s) {
			s.wia.useDefault = false;
			s.wia.method = RVZ_LZMA;
			s.wia.level = 9;
			s.wia.chunk = 4 << 20;
		});
		CHECK(base.rvz.EffectiveMethod(Format::RVZ) == RVZ_LZMA2 && base.rvz.EffectiveChunk(Format::RVZ) == (128u << 10) &&
				  base.rvz.EffectiveChunk(Format::WIA) == (2u << 20),
			"RVZ default isn't LZMA2 / 128K (WIA 2M)");
		std::vector<std::string> rvOut;
		for (size_t i = 0; i < rv.size(); ++i) {
			JobResult r = Convert(gcPath, rv[i].f, rv[i].s, PathU8(root / Fmt("rvz%zu", i)));
			CHECK(r.state == JobState::Done && r.verify == "OK", "GC -> %s: %s %s / %s", rv[i].label, JobStateName(r.state), r.message.c_str(),
				r.verify.c_str());
			printf("  GC  -> %-28s %10llu bytes (%5.1f%%) %s\n", rv[i].label, (unsigned long long)r.outputSize, 100.0 * r.outputSize / gc.size(),
				r.verify.c_str());
			rvOut.push_back(r.state == JobState::Done ? r.outputPath : "");
			if (r.state != JobState::Done)
				continue;
			ImageInfo oi = InspectImage(r.outputPath);
			CHECK(oi.format == rv[i].f && oi.platform == Platform::GameCube && oi.discId == "GTST01" && oi.isoSize == gc.size(),
				"%s info: %s %s %s %llu (%s)", rv[i].label, FormatName(oi.format), PlatformLabel(oi.platform), oi.discId.c_str(),
				(unsigned long long)oi.isoSize, oi.detail.c_str());
			JobResult x = Convert(r.outputPath, Format::ISO, rv[i].s, PathU8(root / Fmt("rvzback%zu", i)));
			std::string why;
			CHECK(x.state == JobState::Done && SameBytes(x.outputPath, gc, &why), "%s -> ISO: %s %s", rv[i].label, x.message.c_str(), why.c_str());
		}
		// RVZ -> CHD through RAM and through a temp ISO, then CHD -> WIA, back to ISO.
		for (int viaRam = 1; viaRam >= 0 && !rvOut[0].empty(); --viaRam) {
			Settings s = base;
			s.engine.repackInRam = viaRam != 0;
			JobResult c = Convert(rvOut[0], Format::CHD, s, PathU8(root / Fmt("rvzchd%d", viaRam)));
			CHECK(c.state == JobState::Done && c.verify == "OK" && (viaRam ? c.viaRam : c.viaTempFile), "RVZ -> CHD (%s): %s %s",
				viaRam ? "RAM" : "temp", c.message.c_str(), c.verify.c_str());
			if (c.state != JobState::Done)
				continue;
			JobResult w = Convert(c.outputPath, Format::WIA, s, PathU8(root / Fmt("chdwia%d", viaRam)));
			CHECK(w.state == JobState::Done && w.verify == "OK", "CHD -> WIA: %s %s", w.message.c_str(), w.verify.c_str());
			if (w.state == JobState::Done) {
				JobResult x = Convert(w.outputPath, Format::ISO, s, PathU8(root / Fmt("chdwiaback%d", viaRam)));
				std::string why;
				CHECK(x.state == JobState::Done && SameBytes(x.outputPath, gc, &why), "WIA (from CHD) -> ISO: %s", why.c_str());
			}
		}
		// Platform rules: a PSP game can't go to RVZ, a GameCube disc can't go to PBP.
		JobResult p1 = Convert(isoPath, Format::RVZ, base, PathU8(root / "pspRvz"));
		CHECK(p1.state == JobState::Skipped && p1.message.find("GameCube") != std::string::npos, "PSP -> RVZ: %s %s", JobStateName(p1.state),
			p1.message.c_str());
		JobResult p2 = Convert(gcPath, Format::PBP, base, PathU8(root / "gcPbp"));
		CHECK(p2.state == JobState::Skipped && p2.message.find("PSP") != std::string::npos, "GC -> PBP: %s %s", JobStateName(p2.state),
			p2.message.c_str());
		CHECK(!fs::exists(root / "pspRvz") && !fs::exists(root / "gcPbp"), "skipped jobs created output folders");
	}

	// Each container keeps its own settings through a save / load.
	{
		Settings s;
		s.cso.useDefault = false;
		s.cso.preset = CsoPreset::Max;
		s.zso.useDefault = false;
		s.zso.preset = CsoPreset::Custom;
		s.zso.lz4Brute = true;
		s.zso.blockSize = 16384;
		s.dax.useDefault = false;
		s.dax.preset = CsoPreset::Fast;
		s.rvz.useDefault = false;
		s.rvz.method = RVZ_ZSTD;
		s.rvz.level = 12;
		s.wia.useDefault = false;
		s.wia.method = RVZ_PURGE;
		s.wia.chunk = 8 << 20;
		std::string ini = PathU8(root / "containers.ini");
		CHECK(s.Save(ini), "saving per-container settings");
		Settings b;
		CHECK(b.Load(ini), "loading per-container settings");
		CHECK(b.cso.preset == CsoPreset::Max && !b.cso.useDefault && b.zso.lz4Brute && b.zso.blockSize == 16384 && b.cso2.useDefault &&
				  b.dax.preset == CsoPreset::Fast && b.rvz.method == RVZ_ZSTD && b.rvz.level == 12 && b.wia.method == RVZ_PURGE &&
				  b.wia.chunk == (8u << 20),
			"per-container settings didn't survive a save / load");
	}

	// Benchmark entries survive a save / load (the GUI's list lives in the settings file).
	{
		std::vector<BenchCase> cases;
		BenchCase c;
#ifdef ISOPK_WITH_CSO
		MakeCsoCase("D", &c);
		cases.push_back(c);
		CsoSettings z;
		z.useDefault = false;
		z.preset = CsoPreset::Custom;
		z.lz4Brute = true;
		z.blockSize = 16384;
		cases.push_back(MakeMcsoCase(Format::ZSO, z));
		cases.push_back(MakeMcsoCase(Format::CSO, CsoSettings()));
#endif
		cases.push_back(MakeChdCase("lzma+zstd", 8192));
		cases.push_back(MakePbpCase(2));
		cases.push_back(MakeRvzCase(Format::RVZ, RvzSettings()));
		{
			RvzSettings z;
			z.useDefault = false;
			z.method = RVZ_ZSTD;
			z.level = -3;
			z.chunk = 64 << 10;
			cases.push_back(MakeRvzCase(Format::RVZ, z));
			z.method = RVZ_PURGE;
			z.chunk = 4 << 20;
			cases.push_back(MakeRvzCase(Format::WIA, z));
		}
		Settings s;
		s.benchCases.clear();
		for (auto &bc : cases)
			s.benchCases.push_back(SerializeBenchCase(bc));
		std::string ini = PathU8(root / "bench_cases.ini");
		CHECK(s.Save(ini), "saving bench cases");
		Settings back;
		back.benchCases.clear();
		CHECK(back.Load(ini) && back.benchCases.size() == cases.size(), "loaded %zu bench cases", back.benchCases.size());
		for (size_t i = 0; i < cases.size() && i < back.benchCases.size(); ++i) {
			BenchCase p;
			CHECK(ParseBenchCase(back.benchCases[i], &p), "parse %s", back.benchCases[i].c_str());
			CHECK(p.format == cases[i].format && SerializeBenchCase(p) == SerializeBenchCase(cases[i]) &&
					  p.cso.Flags(p.format) == cases[i].cso.Flags(cases[i].format) && p.rvz.Describe(p.format) == cases[i].rvz.Describe(cases[i].format) && p.cso.EffectiveBlockSize(p.format) == cases[i].cso.EffectiveBlockSize(cases[i].format),
				"bench case %zu changed: %s", i, back.benchCases[i].c_str());
		}
		// Removing them all must stick (not come back as the defaults).
		s.benchCases.clear();
		CHECK(s.Save(ini), "saving empty list");
		Settings empty;
		CHECK(empty.Load(ini) && empty.benchCases.empty(), "empty bench list came back with %zu", empty.benchCases.size());
#ifdef ISOPK_WITH_CSO
		CsoSettings def;
		CHECK(def.useDefault && def.EffectiveBlockSize(Format::CSO) == 65536 && (def.Flags(Format::CSO) & mcso_flags::NO_LIBDEFLATE) == 0 &&
				  (def.Flags(Format::CSO) & mcso_flags::NO_ZLIB) == mcso_flags::NO_ZLIB,
			"CSO default isn't libdeflate 64K");
#endif
	}

	// ZArchive: a game folder with a plain ISO, an already compressed disc, a GameCube disc and DLC.
	{
		printf("ZArchive\n");
		fs::path game = root / U8Path("Trails in the Sky");
		fs::create_directories(game / "DLC" / "ULUS99999-DLC1", ec);
		fs::create_directories(game / "DLC" / "empty", ec);
		WriteAll(PathU8(game / "Test Game.iso"), iso);
		std::vector<uint8_t> csoBytes = outputs.empty() ? std::vector<uint8_t>() : ReadAll(outputs.front());
		WriteAll(PathU8(game / "Disc 2.cso"), csoBytes);
		// A PBP too: "compress the discs" converts it, through an ISO in RAM.
		std::vector<uint8_t> pbpBytes;
		for (const std::string &o : outputs)
			if (pbpBytes.empty() && ToLower(PathU8(U8Path(o).extension())) == ".pbp")
				pbpBytes = ReadAll(o);
		CHECK(!pbpBytes.empty(), "no PBP output to test with");
		WriteAll(PathU8(game / "Disc 3.pbp"), pbpBytes);
		std::vector<uint8_t> gc = testiso::MakeGameCubeDisc(8);
		WriteAll(PathU8(game / "Bonus GC.iso"), gc);
		std::vector<uint8_t> dlc;
		for (int i = 0; dlc.size() < 3000000; ++i) {
			std::string line = Fmt("DLC item %d texture data PSP_GAME/USRDIR\n", i % 977);
			dlc.insert(dlc.end(), line.begin(), line.end());
		}
		WriteAll(PathU8(game / "DLC" / "ULUS99999-DLC1" / "data.bin"), dlc);
		WriteAll(PathU8(game / "DLC" / "ULUS99999-DLC1" / "PARAM.SFO"), testiso::MakeSfo("ULUS99999", "DLC"));

		ZarSpec spec;
		spec.name = "Trails in the Sky";
		spec.rootDir = PathU8(game);
		Settings zs = base;
		zs.zar = ZarSettings();

		// disc2 / disc3: what Disc 2.cso and Disc 3.pbp are called in the archive (unchanged unless converted).
		auto check = [&](const JobResult &r, const char *label, const std::string &discName, const std::vector<uint8_t> *discBytes,
						 const char *gcName = "Bonus GC.iso", const std::string &disc2 = "Disc 2.cso", const std::string &disc3 = "Disc 3.pbp") {
			CHECK(r.state == JobState::Done, "%s: %s %s", label, JobStateName(r.state), r.message.c_str());
			if (r.state != JobState::Done)
				return;
			CHECK(r.verify == "OK", "%s: verify %s", label, r.verify.c_str());
			bool found;
			std::vector<uint8_t> d = ZarFile(r.outputPath, "DLC/ULUS99999-DLC1/data.bin", &found);
			CHECK(found && d == dlc, "%s: DLC data", label);
			d = ZarFile(r.outputPath, disc2, &found);
			CHECK(found && (disc2 != "Disc 2.cso" || d == csoBytes), "%s: %s", label, disc2.c_str());
			d = ZarFile(r.outputPath, disc3, &found);
			CHECK(found && (disc3 != "Disc 3.pbp" || d == pbpBytes), "%s: %s", label, disc3.c_str());
			d = ZarFile(r.outputPath, gcName, &found);
			CHECK(found && (strcmp(gcName, "Bonus GC.iso") != 0 || d == gc), "%s: GameCube disc %s", label, gcName);
			std::unique_ptr<ZArchiveReader> zr(ZArchiveReader::OpenFromFile(U8Path(r.outputPath)));
			CHECK(zr && zr->LookUp("DLC/empty", false, true) != ZARCHIVE_INVALID_NODE, "%s: empty folder", label);
			d = ZarFile(r.outputPath, discName, &found);
			CHECK(found, "%s: %s missing", label, discName.c_str());
			if (found && discBytes)
				CHECK(d == *discBytes, "%s: %s content", label, discName.c_str());
			printf("  %-28s %s -> %s (%.1f%%), %s\n", label, HumanBytes(r.inputSize).c_str(), HumanBytes(r.outputSize).c_str(),
				100.0 * r.outputSize / std::max<uint64_t>(r.inputSize, 1), r.message.c_str());
		};
		// A disc compressed first, taken out of the archive, must extract to the original ISO.
		auto checkDisc = [&](const JobResult &r, const char *label, const std::string &name, Format f) {
			if (r.state != JobState::Done)
				return;
			bool found;
			std::vector<uint8_t> d = ZarFile(r.outputPath, name, &found);
			CHECK(found && !d.empty(), "%s: %s missing", label, name.c_str());
			CHECK(ZarFile(r.outputPath, "Test Game.iso", &found).empty() && !found, "%s: the ISO is still in", label);
			const std::string tmp = PathU8(root / ("zar_disc" + std::string(FormatExtension(f))));
			WriteAll(tmp, d);
			CHECK(DetectFormat(tmp) == f, "%s: not a %s", label, FormatName(f));
			OpContext ctx;
			const std::string back = PathU8(root / "zar_back.iso");
			CHECK(DecompressToFile(tmp, f, base, back, true, ctx), "%s: extract %s", label, ctx.error.c_str());
			std::string why;
			CHECK(SameBytes(back, iso, &why), "%s: %s", label, why.c_str());
			fs::remove(U8Path(tmp), ec);
			fs::remove(U8Path(back), ec);
		};

		const std::string zdir = PathU8(root / "zar");
		JobResult r = MakeZar(spec, zs, zdir);
		check(r, "default (zstd 22, pre-check)", "Test Game.iso", &iso);
		CHECK(r.outputPath == PathU8(U8Path(zdir) / "Trails in the Sky.zar"), "output %s", r.outputPath.c_str());

		// Extracting it: recognised, scanned from a folder, unpacked next to it into "Trails in the Sky/".
		{
			const std::string zarPath = r.outputPath;
			CHECK(DetectFormat(zarPath) == Format::ZAR, "a .zar detected as %s", FormatName(DetectFormat(zarPath)));
			ImageInfo zi = InspectImage(zarPath);
			CHECK(zi.isoSize == r.inputSize, "ZAR contents %llu, expected %llu", (unsigned long long)zi.isoSize, (unsigned long long)r.inputSize);
			std::vector<std::string> w;
			auto found = ScanInputs({ zdir }, true, &w);
			CHECK(found.size() == 1 && found[0].format == Format::ZAR, "scanning found %zu", found.size());
			auto extract = [&](Conflict c) {
				Job job;
				job.input.path = zarPath;
				job.input.format = Format::ZAR;
				job.input.size = fs::file_size(U8Path(zarPath));
				job.target = Format::ISO;
				Settings sx = base;
				sx.out.dir = PathU8(root / "ignored");  // always next to the archive
				sx.out.conflict = c;
				job.settings = sx;
				RunJob(job);
				return job.Result();
			};
			JobResult x = extract(Conflict::Skip);
			const fs::path outFolder = U8Path(zdir) / "Trails in the Sky";
			CHECK(x.state == JobState::Done && x.verify == "OK", "extract: %s %s", JobStateName(x.state), x.message.c_str());
			CHECK(x.outputPath == PathU8(outFolder), "extracted to %s", x.outputPath.c_str());
			std::string why;
			CHECK(SameBytes(PathU8(outFolder / "Test Game.iso"), iso, &why), "extracted ISO: %s", why.c_str());
			CHECK(SameBytes(PathU8(outFolder / "DLC" / "ULUS99999-DLC1" / "data.bin"), dlc, &why), "extracted DLC: %s", why.c_str());
			CHECK(SameBytes(PathU8(outFolder / "Disc 2.cso"), csoBytes, &why), "extracted CSO: %s", why.c_str());
			CHECK(fs::is_directory(outFolder / "DLC" / "empty"), "empty folder not extracted");
			CHECK(!fs::exists(U8Path(PathU8(outFolder) + ".isopk-part")), "work folder left behind");
			JobResult again = extract(Conflict::Skip);
			CHECK(again.state == JobState::Skipped, "existing folder not skipped: %s", JobStateName(again.state));
			JobResult renamed = extract(Conflict::Rename);
			CHECK(renamed.state == JobState::Done && renamed.outputPath == PathU8(outFolder) + " (2)", "rename: %s", renamed.outputPath.c_str());
			// Overwrite goes into the existing folder: its files replaced, others kept.
			WriteAll(PathU8(outFolder / "mine.txt"), { 'h', 'i' });
			WriteAll(PathU8(outFolder / "Test Game.iso"), { 'x' });
			JobResult over = extract(Conflict::Overwrite);
			CHECK(over.state == JobState::Done && over.outputPath == PathU8(outFolder), "overwrite: %s", over.message.c_str());
			CHECK(SameBytes(PathU8(outFolder / "Test Game.iso"), iso, &why) && fs::exists(outFolder / "mine.txt"), "overwrite: %s", why.c_str());
			printf("  %-28s %s -> folder, %s\n", "extract", HumanBytes(x.inputSize).c_str(), x.message.c_str());
			fs::remove_all(outFolder, ec);
			fs::remove_all(U8Path(PathU8(outFolder) + " (2)"), ec);
		}

		Settings st = zs;
		st.zar.storeDiscs = true;
		st.zar.level = 3;
		r = MakeZar(spec, st, zdir);
		check(r, "discs stored, level 3", "Test Game.iso", &iso);
		CHECK(r.message.find("4 discs stored") != std::string::npos, "store-only: %s", r.message.c_str());

		// Every disc in the chosen format: the ISOs compressed, the CSO / PBP converted through RAM,
		// discs already in that format left alone, and each converted one must give the ISO back.
		std::vector<Format> firsts;
#ifdef ISOPK_WITH_CSO
		firsts.push_back(Format::CSO);
#endif
		firsts.push_back(Format::CHD);
		firsts.push_back(Format::PBP);
		for (Format f : firsts) {
			Settings sc = zs;
			sc.zar.compressDiscs = true;
			sc.zar.discFormat = f;
			const std::string ext = FormatExtension(f);
			std::string label = std::string("discs to ") + FormatName(f) + " in RAM";
			r = MakeZar(spec, sc, zdir);
			// PBP refuses the GameCube disc; the other formats compress it too.
			const std::string gcName = f == Format::PBP ? std::string("Bonus GC.iso") : "Bonus GC" + ext;
			const std::string disc2 = f == Format::CSO ? std::string("Disc 2.cso") : "Disc 2" + ext;
			const std::string disc3 = f == Format::PBP ? std::string("Disc 3.pbp") : "Disc 3" + ext;
			check(r, label.c_str(), "Test Game" + ext, nullptr, gcName.c_str(), disc2, disc3);
			checkDisc(r, label.c_str(), "Test Game" + ext, f);
			if (f != Format::CSO)
				checkDisc(r, (label + ", converted CSO").c_str(), disc2, f);
			if (f != Format::PBP)
				checkDisc(r, (label + ", converted PBP").c_str(), disc3, f);
			const char *want = f == Format::PBP ? "1 disc compressed first, 1 disc converted first"
				: f == Format::CHD             ? "2 discs compressed first, 2 discs converted first"
											   : "2 discs compressed first, 1 disc converted first";
			CHECK(r.message.find(want) != std::string::npos, "%s: %s", label.c_str(), r.message.c_str());
		}

		// Not enough RAM: temporary files instead (an ISO for the PBP / CSO, the CHD itself).
		{
			Settings sc = zs;
			sc.zar.compressDiscs = true;
			sc.zar.discFormat = Format::CHD;
			sc.engine.ramBudget = 1u << 20;
			ApplyEngineSettings(sc.engine);
			r = MakeZar(spec, sc, zdir);
			ApplyEngineSettings(base.engine);
			check(r, "CHD via temporary files", "Test Game.chd", nullptr, "Bonus GC.chd", "Disc 2.chd", "Disc 3.chd");
			checkDisc(r, "CHD via temporary files", "Test Game.chd", Format::CHD);
			checkDisc(r, "CHD via temporary files, converted PBP", "Disc 3.chd", Format::CHD);
			for (auto &it : fs::directory_iterator(U8Path(zdir), ec))
				CHECK(PathU8(it.path().filename()).find("isopk-temp") == std::string::npos, "temporary file left: %s", PathU8(it.path()).c_str());
		}

		// Picked files and folders: clashing names get renamed, the archive goes next to the first one.
		{
			ZarSpec picked;
			picked.name = "Picked";
			picked.extras = { PathU8(game / "Test Game.iso"), PathU8(srcDir / "Test Game.iso"), PathU8(game / "DLC") };
			Settings sp = zs;
			sp.zar.level = 1;
			sp.out.dir.clear();
			ZarSpec measured = picked;
			std::string err;
			MeasureZarSpec(&measured, &err);
			Job job;
			job.zar = std::make_shared<const ZarSpec>(measured);
			job.input.path = picked.extras[0];
			job.target = Format::ZAR;
			sp.out.conflict = Conflict::Overwrite;
			job.settings = sp;
			RunJob(job);
			JobResult pr = job.Result();
			CHECK(pr.state == JobState::Done, "picked: %s", pr.message.c_str());
			CHECK(pr.outputPath == PathU8(game / "Picked.zar"), "picked output %s", pr.outputPath.c_str());
			bool f1, f2, f3;
			CHECK(ZarFile(pr.outputPath, "Test Game.iso", &f1) == iso && ZarFile(pr.outputPath, "Test Game (2).iso", &f2) == iso && f1 && f2,
				"picked: renamed clash");
			CHECK(ZarFile(pr.outputPath, "DLC/ULUS99999-DLC1/data.bin", &f3) == dlc && f3, "picked: DLC folder under its name");
			fs::remove(U8Path(pr.outputPath), ec);
		}

		// The archive itself is never packed into itself, and an existing one is skipped by default.
		{
			Settings sk = zs;
			sk.out.dir = PathU8(game);
			ZarSpec m = spec;
			std::string err;
			MeasureZarSpec(&m, &err);
			Job job;
			job.zar = std::make_shared<const ZarSpec>(m);
			job.input.path = spec.rootDir;
			job.target = Format::ZAR;
			sk.out.conflict = Conflict::Overwrite;
			sk.zar.level = 1;
			job.settings = sk;
			RunJob(job);
			JobResult a = job.Result();
			CHECK(a.state == JobState::Done, "inside: %s", a.message.c_str());
			Job again;
			again.zar = job.zar;
			again.input.path = spec.rootDir;
			again.target = Format::ZAR;
			sk.out.conflict = Conflict::Overwrite;
			again.settings = sk;
			RunJob(again);
			bool found;
			ZarFile(again.Result().outputPath, "Trails in the Sky.zar", &found);
			CHECK(!found, "the archive packed itself");
			sk.out.conflict = Conflict::Skip;
			Job skip;
			skip.zar = job.zar;
			skip.input.path = spec.rootDir;
			skip.target = Format::ZAR;
			skip.settings = sk;
			RunJob(skip);
			CHECK(skip.State() == JobState::Skipped, "existing archive not skipped: %s", JobStateName(skip.State()));
			fs::remove(U8Path(a.outputPath), ec);
		}
		// A normal job can't target ZAR.
		JobResult bad = Convert(isoPath, Format::ZAR, base, zdir);
		CHECK(bad.state == JobState::Failed, "a plain job made a ZAR");
	}

#ifdef ISOPK_CLI_EXE
	// The command line tool, end to end: every format without options gives the defaults.
	{
		printf("Command line\n");
		const fs::path cdir = root / "cli";
		fs::create_directories(cdir / "game" / "DLC", ec);
		const std::string q = "\"", d = PathU8(cdir), log = PathU8(cdir / "out.txt");
		WriteAll(PathU8(cdir / "game" / "Game.iso"), iso);
		WriteAll(PathU8(cdir / "game" / "DLC" / "dlc.bin"), std::vector<uint8_t>(300000, 'd'));
		auto run = [&](const std::string &args, int want) {
			int rc = Cli(args, log);
			std::string out;
			{
				std::vector<uint8_t> b = ReadAll(log);
				out.assign(b.begin(), b.end());
			}
			CHECK(rc == want, "isopackager-cli %s: exit %d, expected %d\n%s", args.c_str(), rc, want, out.c_str());
			return out;
		};
		const std::string isoArg = q + PathU8(cdir / "game" / "Game.iso") + q;
		run("cso -q -o " + q + d + "/a" + q + " " + isoArg, 0);
		run("chd -q -o " + q + d + "/a" + q + " " + isoArg, 0);
		ImageInfo ci = InspectImage(PathU8(cdir / "a" / "Game.cso"));
		CHECK(ci.format == Format::CSO && ci.blockSize == 65536, "cli cso: %s block %u", FormatName(ci.format), ci.blockSize);
		ImageInfo hi = InspectImage(PathU8(cdir / "a" / "Game.chd"));
		CHECK(hi.format == Format::CHD && hi.blockSize == 2048, "cli chd: %s hunk %u", FormatName(hi.format), hi.blockSize);
		run("cso -q --method zopfli --block 16K --precheck 98 -o " + q + d + "/b" + q + " " + q + d + "/a/Game.chd" + q, 0);
		CHECK(InspectImage(PathU8(cdir / "b" / "Game.cso")).blockSize == 16384, "cli cso --block 16K");
		run("chd --level 3 " + isoArg, 1);
		run("cso --method lz4 " + isoArg, 1);
		run("frobnicate " + isoArg, 1);
		run("zar -q --discs pbp -o " + q + d + "/z" + q + " " + q + d + "/game" + q, 0);
		bool found;
		CHECK(!ZarFile(PathU8(cdir / "z" / "game.zar"), "Game/EBOOT.PBP", &found).empty() && found, "cli zar --discs pbp");
		std::string v = run("verify " + q + d + "/z/game.zar" + q, 0);
		CHECK(v.find("SHA-256 matches") != std::string::npos, "cli verify zar: %s", v.c_str());
		run("iso -q " + q + d + "/z/game.zar" + q, 0);
		std::string why;
		CHECK(SameBytes(PathU8(cdir / "z" / "game" / "DLC" / "dlc.bin"), std::vector<uint8_t>(300000, 'd'), &why), "cli zar extract: %s", why.c_str());
		std::string b = run("bench --reads 10 --work " + q + d + "/bench" + q + " --case cso --case " + q + "chd --block 16K" + q + " " + isoArg, 0);
		CHECK(b.find("zstd") == std::string::npos && b.find("default hunk 16K") != std::string::npos, "cli bench output:\n%s", b.c_str());
		std::string f = run("formats", 0);
		CHECK(f.find("default hunk 2K") != std::string::npos && f.find("libdeflate 64K") != std::string::npos, "cli formats:\n%s", f.c_str());
		printf("  ok\n");
	}
#endif

	// CD images: a Redump-style cue with three track files, a CloneCD image, CHD and back.
	{
		uint32_t seed = 99;
		auto noise = [&](size_t n) {
			std::vector<uint8_t> v(n);
			for (auto &b : v)
				b = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
			return v;
		};
		auto put = [&](const fs::path &p, const std::vector<uint8_t> &d) {
			File f;
			CHECK(f.Open(PathU8(p), File::Write) && f.WriteAt(0, d.data(), d.size()), "can't write %s", PathU8(p).c_str());
		};
		auto putText = [&](const fs::path &p, const std::string &t) { put(p, std::vector<uint8_t>(t.begin(), t.end())); };
		auto readText = [&](const fs::path &p) {
			std::vector<uint8_t> v;
			File f;
			if (f.Open(PathU8(p), File::Read)) {
				v.resize((size_t)f.Size());
				f.ReadExact(0, v.data(), v.size());
			}
			return std::string(v.begin(), v.end());
		};
		std::vector<uint8_t> t1 = noise(30 * 2352), t2 = noise(40 * 2352), t3 = noise(23 * 2352);
		for (int i = 0; i < 30; ++i) {  // raw MODE1 sectors
			uint8_t *s = &t1[i * 2352];
			memset(s, 0xff, 12);
			s[0] = s[11] = 0;
			s[15] = 1;
		}
		const std::string cue =
			"FILE \"CD Game (Track 1).bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
			"FILE \"CD Game (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:10\n"
			"FILE \"CD Game (Track 3).bin\" BINARY\n  TRACK 03 AUDIO\n    PREGAP 00:00:05\n    INDEX 01 00:00:00\n";
		fs::path cd = root / "cd";
		fs::create_directories(cd, ec);
		put(cd / "CD Game (Track 1).bin", t1);
		put(cd / "CD Game (Track 2).bin", t2);
		put(cd / "CD Game (Track 3).bin", t3);
		putText(cd / "CD Game.cue", cue);

		std::vector<std::string> w;
		auto found = ScanInputs({ PathU8(cd) }, false, &w);
		CHECK(found.size() == 1 && found[0].format == Format::CUE, "a cue and its tracks should scan as one image (%zu)", found.size());
		ImageInfo ci = InspectImage(PathU8(cd / "CD Game.cue"));
		CHECK(ci.error.empty() && ci.platform == Platform::CD && ci.cdTracks == 3 && ci.isoSize == 93 * 2352, "cue info: %s %s",
			ci.detail.c_str(), ci.error.c_str());

		Settings s;
		s.engine.verify = true;
		JobResult r = Convert(PathU8(cd / "CD Game.cue"), Format::CHD, s, PathU8(root / "cd_chd"));
		CHECK(r.state == JobState::Done && r.verify == "OK", "cue -> CHD: %s", r.message.c_str());
		const std::string chd = PathU8(root / "cd_chd" / "CD Game.chd");
		ImageInfo hi = InspectImage(chd);
		CHECK(hi.error.empty() && hi.platform == Platform::CD && hi.cdTracks == 3, "CD CHD info: %s %s", hi.detail.c_str(), hi.error.c_str());
		r = Convert(chd, Format::CSO, s, PathU8(root / "cd_x"));  // CD images in a CSO batch become CD CHDs
		CHECK(r.state == JobState::Done && U8Path(r.outputPath).extension() == ".chd", "CD CHD in a CSO batch: %s", r.message.c_str());
		CHECK(Convert(isoPath, Format::CUE, s, PathU8(root / "cd_x")).state == JobState::Skipped, "a PSP ISO can't become cue/bin");

		// DVD images in CD CHDs ("chdman createcd" on a PSP or PS2 DVD ISO): recognised as the DVD
		// they are and converted as that ISO, to a proper DVD CHD, an ISO, CSO or PBP. CD games stay CDs.
		{
			fs::path dv = root / "dvd_in_cd";
			fs::create_directories(dv, ec);
			std::vector<uint8_t> ps2 = testiso::MakePs2Iso(900, true), ps2cd = testiso::MakePs2Iso(900, false, "SLUS_200.01");
			put(dv / "PS2 Game.iso", ps2);
			put(dv / "PS2 CD Game.iso", ps2cd);
			put(dv / "PSP Raw.bin", testiso::RawSectors(iso, 1));
			putText(dv / "PSP Raw.cue", "FILE \"PSP Raw.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
			put(dv / "PS2 Mode2.bin", testiso::RawSectors(ps2, 2));
			putText(dv / "PS2 Mode2.cue", "FILE \"PS2 Mode2.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
			static const uint32_t none[4] = {};
			struct Case {
				std::string src, chd;
				const std::vector<uint8_t> *data;
				Platform platform;
				const char *discId;
			} cases[] = {
				{ isoPath, PathU8(dv / "PSP Cooked.chd"), &iso, Platform::PSP, "ULUS99999" },
				{ PathU8(dv / "PSP Raw.cue"), PathU8(dv / "PSP Raw.chd"), &iso, Platform::PSP, "ULUS99999" },
				{ PathU8(dv / "PS2 Game.iso"), PathU8(dv / "PS2 Game.chd"), &ps2, Platform::PS2, "SLUS20312" },
				{ PathU8(dv / "PS2 Mode2.cue"), PathU8(dv / "PS2 Mode2.chd"), &ps2, Platform::PS2, "SLUS20312" },
			};
			for (const Case &c : cases) {
				const std::string label = PathU8(U8Path(c.chd).filename());
				CHECK(chdm_create_cd(c.src.c_str(), c.chd.c_str(), 0, none, nullptr, nullptr) == CHDM_OK, "%s: createcd: %s", label.c_str(),
					chdm_last_error_message());
				ImageInfo di = InspectImage(c.chd);
				CHECK(di.error.empty() && di.dvdInCdChd && di.platform == c.platform && di.discId == c.discId && di.isoSize == c.data->size(),
					"%s info: %s, %s, %s, %llu %s", label.c_str(), PlatformLabel(di.platform), di.discId.c_str(), di.detail.c_str(),
					(unsigned long long)di.isoSize, di.error.c_str());
				CHECK(!IsCdSource(c.chd, Format::CHD), "%s counts as a CD", label.c_str());
				const std::string out = PathU8(root / "dvd_out" / label);
				JobResult r = Convert(c.chd, Format::CHD, s, PathU8(root / "dvd_out"));
				CHECK(r.state == JobState::Done && r.verify == "OK", "%s -> CHD: %s", label.c_str(), r.message.c_str());
				chdm_file *f = nullptr;
				chdm_info ci{};
				if (chdm_open(r.outputPath.c_str(), CHDM_OPEN_READ, nullptr, &f) == CHDM_OK) {
					chdm_get_info(f, &ci);
					chdm_close(f);
				}
				CHECK(ci.kind == CHDM_KIND_DVD && ci.logical_bytes == c.data->size(), "%s: the new CHD isn't a DVD CHD of the image (kind %d)",
					label.c_str(), (int)ci.kind);
				JobResult x = Convert(r.outputPath, Format::ISO, s, PathU8(root / "dvd_iso"));
				std::string why;
				CHECK(x.state == JobState::Done && SameBytes(x.outputPath, *c.data, &why), "%s -> DVD CHD -> ISO: %s %s", label.c_str(),
					x.message.c_str(), why.c_str());
				x = Convert(c.chd, Format::ISO, s, PathU8(root / "dvd_iso2"));
				CHECK(x.state == JobState::Done && U8Path(x.outputPath).extension() == ".iso" && SameBytes(x.outputPath, *c.data, &why),
					"%s -> ISO: %s %s %s", label.c_str(), x.outputPath.c_str(), x.message.c_str(), why.c_str());
				if (c.platform == Platform::PSP) {
					x = Convert(c.chd, Format::PBP, s, PathU8(root / "dvd_pbp"));
					CHECK(x.state == JobState::Done && x.verify == "OK" && ToLower(PathU8(U8Path(x.outputPath).filename())) == "eboot.pbp" &&
							  DetectFormat(x.outputPath) == Format::PBP,
						"%s -> PBP: %s %s", label.c_str(), x.outputPath.c_str(), x.message.c_str());
				} else {
					CHECK(Convert(c.chd, Format::PSX, s, PathU8(root / "dvd_x")).state == JobState::Skipped, "%s: a PS2 DVD can't be a PSOne EBOOT",
						label.c_str());
				}
			}
			// A PS2 CD game (no UDF, CD sized) in a CD CHD is a CD.
			const std::string cdChd = PathU8(dv / "PS2 CD Game.chd");
			CHECK(chdm_create_cd(PathU8(dv / "PS2 CD Game.iso").c_str(), cdChd.c_str(), 0, none, nullptr, nullptr) == CHDM_OK, "PS2 CD createcd");
			ImageInfo ci = InspectImage(cdChd);
			CHECK(ci.platform == Platform::CD && !ci.dvdInCdChd && IsCdSource(cdChd, Format::CHD), "a PS2 CD game is a CD: %s",
				PlatformLabel(ci.platform));
			// And the plain PS2 ISO reads as PS2.
			ImageInfo pi = InspectImage(PathU8(dv / "PS2 Game.iso"));
			CHECK(pi.platform == Platform::PS2 && pi.discId == "SLUS20312", "PS2 ISO info: %s %s", PlatformLabel(pi.platform), pi.discId.c_str());
		}

		r = Convert(chd, Format::CUE, s, PathU8(root / "cd_cue"));
		CHECK(r.state == JobState::Done && r.verify == "OK", "CHD -> cue: %s", r.message.c_str());
		std::string why;
		CHECK(readText(root / "cd_cue" / "CD Game.cue") == cue, "split cue sheet differs");
		CHECK(SameBytes(PathU8(root / "cd_cue" / "CD Game (Track 1).bin"), t1, &why), "track 1: %s", why.c_str());
		CHECK(SameBytes(PathU8(root / "cd_cue" / "CD Game (Track 2).bin"), t2, &why), "track 2: %s", why.c_str());
		CHECK(SameBytes(PathU8(root / "cd_cue" / "CD Game (Track 3).bin"), t3, &why), "track 3: %s", why.c_str());
		CHECK(r.outputSize == 93 * 2352 + cue.size(), "cue/bin output size %llu", (unsigned long long)r.outputSize);

		std::vector<uint8_t> joined = t1;
		joined.insert(joined.end(), t2.begin(), t2.end());
		joined.insert(joined.end(), t3.begin(), t3.end());
		Settings one = s;
		one.cue.splitBins = false;
		r = Convert(chd, Format::CUE, one, PathU8(root / "cd_one"));
		CHECK(r.state == JobState::Done, "CHD -> single bin: %s", r.message.c_str());
		CHECK(SameBytes(PathU8(root / "cd_one" / "CD Game.bin"), joined, &why), "single bin: %s", why.c_str());

		// CloneCD straight to cue/bin (through a temporary CHD) and to CHD.
		fs::path ccd = root / "ccd";
		fs::create_directories(ccd, ec);
		std::vector<uint8_t> img = joined;
		put(ccd / "Clone.img", img);
		putText(ccd / "Clone.ccd", "[CloneCD]\nVersion=3\n[Disc]\nSessions=1\n[TRACK 1]\nMODE=1\nINDEX 1=0\n"
			"[TRACK 2]\nMODE=0\nINDEX 0=30\nINDEX 1=40\n[TRACK 3]\nMODE=0\nINDEX 1=70\n");
		r = Convert(PathU8(ccd / "Clone.ccd"), Format::CUE, one, PathU8(root / "ccd_cue"));
		CHECK(r.state == JobState::Done && r.verify == "OK", "ccd -> cue: %s", r.message.c_str());
		CHECK(SameBytes(PathU8(root / "ccd_cue" / "Clone.bin"), img, &why), "ccd -> bin: %s", why.c_str());
		CHECK(!fs::exists(root / "ccd_cue" / "Clone.cue.isopk-temp.chd"), "temporary CHD left behind");
		r = Convert(PathU8(ccd / "Clone.img"), Format::CHD, s, PathU8(root / "ccd_chd"));
		CHECK(r.state == JobState::Done && r.verify == "OK", "img (through its ccd) -> CHD: %s", r.message.c_str());

		// Into a ZArchive with the discs as CHDs: the cue/bin set becomes one CD CHD, even when the
		// discs are going to CSO, and a data .bin next to it stays as it is.
		{
			put(cd / "notes.bin", std::vector<uint8_t>(2352 * 3, 'n'));
			for (Format discs : { Format::CHD, Format::PBP }) {
				ZarSpec spec;
				spec.name = "CD Archive";
				spec.rootDir = PathU8(cd);
				Settings zs = s;
				zs.zar = ZarSettings();
				zs.zar.level = 1;
				zs.zar.compressDiscs = true;
				zs.zar.discFormat = discs;
				r = MakeZar(spec, zs, PathU8(root / "cd_zar"));
				CHECK(r.state == JobState::Done && r.verify == "OK", "cue in a ZAR (%s): %s", FormatName(discs), r.message.c_str());
				bool found = false;
				std::vector<uint8_t> inner = ZarFile(r.outputPath, "CD Game.chd", &found);
				CHECK(found && !inner.empty(), "no CD Game.chd in the archive");
				ZarFile(r.outputPath, "CD Game (Track 2).bin", &found);
				CHECK(!found, "a track file went into the archive next to its CHD");
				std::vector<uint8_t> notes = ZarFile(r.outputPath, "notes.bin", &found);
				CHECK(found && notes.size() == 2352 * 3, "notes.bin should be stored as it is");
				const std::string out = PathU8(root / "cd_zar" / "inner.chd");
				put(out, inner);
				CHECK(chdm_verify_cd(PathU8(cd / "CD Game.cue").c_str(), out.c_str(), nullptr, nullptr) == CHDM_OK, "the CHD in the archive: %s",
					chdm_last_error_message());
			}
			fs::remove(cd / "notes.bin", ec);
		}

		// PSOne EBOOTs: a PlayStation disc (MODE2 sectors, ISO9660, SYSTEM.CNF) with an audio track.
		{
			auto psxDisc = [&](const std::string &boot, uint32_t dataSectors) {
				std::vector<uint8_t> raw((size_t)dataSectors * 2352, 0);
				std::vector<uint8_t> sec(2048 * dataSectors, 0);
				auto both = [](uint8_t *q, uint32_t v) {
					for (int i = 0; i < 4; ++i) {
						q[i] = (uint8_t)(v >> (8 * i));
						q[7 - i] = (uint8_t)(v >> (8 * i));
					}
				};
				auto rec = [&](uint8_t *q, const std::string &name, uint32_t lba, uint32_t size, bool dir) {
					size_t nl = name.size(), len = 33 + nl + ((33 + nl) & 1);
					q[0] = (uint8_t)len;
					both(q + 2, lba);
					both(q + 10, size);
					q[25] = dir ? 2 : 0;
					q[28] = 1;
					q[32] = (uint8_t)nl;
					memcpy(q + 33, name.data(), nl);
					return len;
				};
				uint8_t *pvd = &sec[16 * 2048];
				pvd[0] = 1;
				memcpy(pvd + 1, "CD001", 5);
				pvd[6] = 1;
				both(pvd + 80, dataSectors);
				rec(pvd + 156, std::string(1, '\0'), 18, 2048, true);
				sec[17 * 2048] = 255;
				memcpy(&sec[17 * 2048 + 1], "CD001", 5);
				const std::string cnf = "BOOT = " + boot + "\r\n";
				uint8_t *r = &sec[18 * 2048];
				r += rec(r, std::string(1, '\0'), 18, 2048, true);
				r += rec(r, std::string(1, '\1'), 18, 2048, true);
				rec(r, "SYSTEM.CNF;1", 19, (uint32_t)cnf.size(), false);
				memcpy(&sec[19 * 2048], cnf.data(), cnf.size());
				std::vector<uint8_t> fill = noise(2048 * (dataSectors - 24));
				memcpy(&sec[24 * 2048], fill.data(), fill.size());
				for (uint32_t i = 0; i < dataSectors; ++i) {
					uint8_t *q = &raw[(size_t)i * 2352];
					memset(q + 1, 0xff, 10);
					q[15] = 2;
					q[18] = q[22] = 8;
					memcpy(q + 24, &sec[(size_t)i * 2048], 2048);
				}
				return raw;
			};
			fs::path psx = root / "psx";
			fs::create_directories(psx, ec);
			std::vector<uint8_t> d1 = psxDisc("cdrom:\\SLUS_012.34;1", 64), audio = noise(150 * 2352 + 80 * 2352);
			std::vector<uint8_t> d1bin = d1;
			d1bin.insert(d1bin.end(), audio.begin(), audio.end());
			put(psx / "PS Game (Disc 1).bin", d1bin);
			putText(psx / "PS Game (Disc 1).cue", "FILE \"PS Game (Disc 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
				"  TRACK 02 AUDIO\n    INDEX 00 00:00:64\n    INDEX 01 00:02:64\n");
			std::vector<uint8_t> d2 = psxDisc("cdrom:\\SLUS_012.35;1", 40);
			put(psx / "PS Game (Disc 2).bin", d2);
			putText(psx / "PS Game (Disc 2).cue", "FILE \"PS Game (Disc 2).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");

			// A PBP job on CD images makes a PSOne EBOOT, both discs in it; disc 2's own job is skipped.
			Settings ps = s;
			ps.pbp.layout = PbpLayout::File;
			r = Convert(PathU8(psx / "PS Game (Disc 1).cue"), Format::PBP, ps, PathU8(root / "psx_out"));
			CHECK(r.state == JobState::Done && r.verify == "OK", "cue -> PSOne EBOOT: %s", r.message.c_str());
			const std::string eboot = PathU8(root / "psx_out" / "PS Game.pbp");
			CHECK(r.outputPath == eboot, "EBOOT named %s", r.outputPath.c_str());
			CHECK(DetectFormat(eboot) == Format::PSX, "the EBOOT is detected as %s", FormatName(DetectFormat(eboot)));
			ImageInfo pi = InspectImage(eboot);
			CHECK(pi.error.empty() && pi.platform == Platform::CD && pi.discId == "SLUS01234" && pi.title == "PS Game" &&
				pi.isoSize == d1bin.size() + d2.size(), "EBOOT info: %s %s %s", pi.discId.c_str(), pi.detail.c_str(), pi.error.c_str());
			JobResult skipped = Convert(PathU8(psx / "PS Game (Disc 2).cue"), Format::PSX, ps, PathU8(root / "psx_out"));
			CHECK(skipped.state == JobState::Skipped, "disc 2 should go with disc 1: %s", JobStateName(skipped.state));

			// Back: cue/bin and CHDs, one per disc.
			Settings one2 = ps;
			one2.cue.splitBins = false;
			r = Convert(eboot, Format::CUE, one2, PathU8(root / "psx_cue"));
			CHECK(r.state == JobState::Done && r.verify == "OK", "EBOOT -> cue: %s", r.message.c_str());
			CHECK(SameBytes(PathU8(root / "psx_cue" / "PS Game (Disc 1).bin"), d1bin, &why), "disc 1 back: %s", why.c_str());
			CHECK(SameBytes(PathU8(root / "psx_cue" / "PS Game (Disc 2).bin"), d2, &why), "disc 2 back: %s", why.c_str());
			CHECK(readText(root / "psx_cue" / "PS Game (Disc 1).cue") ==
					"FILE \"PS Game (Disc 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
					"  TRACK 02 AUDIO\n    INDEX 00 00:00:64\n    INDEX 01 00:02:64\n",
				"disc 1 cue sheet: %s", readText(root / "psx_cue" / "PS Game (Disc 1).cue").c_str());
			r = Convert(eboot, Format::CHD, ps, PathU8(root / "psx_chd"));
			CHECK(r.state == JobState::Done && r.verify == "OK", "EBOOT -> CHD: %s", r.message.c_str());
			CHECK(chdm_verify_cd(PathU8(psx / "PS Game (Disc 1).cue").c_str(), PathU8(root / "psx_chd" / "PS Game (Disc 1).chd").c_str(),
					  nullptr, nullptr) == CHDM_OK, "disc 1 CHD: %s", chdm_last_error_message());
			// And a CD CHD to an EBOOT again (one disc, stored), into Game/EBOOT.PBP.
			Settings stored = s;
			stored.psx.level = 0;
			stored.psx.groupDiscs = false;  // disc 1 is next to it
			r = Convert(PathU8(root / "psx_chd" / "PS Game (Disc 2).chd"), Format::PSX, stored, PathU8(root / "psx_out2"));
			CHECK(r.state == JobState::Done && r.verify == "OK" && r.outputPath == PathU8(root / "psx_out2" / "PS Game (Disc 2)" / "EBOOT.PBP"),
				"CHD -> EBOOT: %s %s", r.outputPath.c_str(), r.message.c_str());
			CHECK(Convert(isoPath, Format::PSX, s, PathU8(root / "psx_x")).state == JobState::Skipped, "a PSP ISO can't be a PSOne EBOOT");
			for (const char *d : { "psx_out", "psx_cue", "psx_chd", "psx_out2" })
				for (auto &it : fs::directory_iterator(root / d, ec))
					CHECK(PathU8(it.path().filename()).find("isopk-temp") == std::string::npos, "temporary left: %s", PathU8(it.path()).c_str());
		}

		// Deleting the source takes every file of the image.
		Settings del = s;
		del.out.deleteSource = true;
		r = Convert(PathU8(cd / "CD Game.cue"), Format::CHD, del, PathU8(root / "cd_chd2"));
		CHECK(r.state == JobState::Done, "cue -> CHD, delete source: %s", r.message.c_str());
		CHECK(fs::is_empty(cd, ec), "the cue's track files should be deleted with it");
	}

	// A tiny benchmark.
	{
		BenchConfig cfg;
		cfg.reads = 50;
		cfg.workDir = PathU8(root / "bench");
		BenchCase c;
#ifdef ISOPK_WITH_CSO
		MakeCsoCase("L", &c);
		cfg.cases.push_back(c);
		cfg.cases.push_back(MakeMcsoCase(Format::ZSO, CsoSettings()));
#endif
		cfg.cases.push_back(MakeChdCase("default", 4096));
		cfg.cases.push_back(MakePbpCase(1));
		std::vector<std::string> w;
		auto files = ScanInputs({ isoPath, outputs.back() }, false, &w);
		auto rows = RunBenchmark(files, cfg, {}, nullptr);
		CHECK(rows.size() == cfg.cases.size() * 2, "bench rows %zu", rows.size());
		for (auto &r : rows) {
			CHECK(!r.failed && r.verify.rfind("OK", 0) == 0, "bench %s %s: %s %s", r.format.c_str(), r.setting.c_str(), r.error.c_str(),
				r.verify.c_str());
			if (r.format == "CHD" || r.format == "CSO" || r.format == "ZSO")
				CHECK(r.readMs >= 0, "bench %s: no random read time", r.format.c_str());
		}
		CHECK(fs::exists(root / "bench" / "results.csv"), "results.csv missing");
	}

	if (g_failures == 0) {
		printf("All tests passed.\n");
		fs::remove_all(root, ec);
		return 0;
	}
	printf("%d failure(s). Files left in %s\n", g_failures, PathU8(root).c_str());
	return 1;
}
