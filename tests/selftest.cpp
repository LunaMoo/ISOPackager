// Round trips through every format and path the engine has, on a generated PSP image:
//   ISO -> every format (verified), every format -> ISO (compared with the original),
//   compressed -> compressed through RAM and through a temporary ISO, parallel jobs in the queue,
//   disc ID / title detection, and a small benchmark.
// Usage: isopk_selftest <scratch folder>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/bench.h"
#include "core/engine.h"
#include "core/fileio.h"
#include "core/queue.h"
#include "core/util.h"
#include "test_iso.h"

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
		s.cso.useDefault = false;
		s.cso.preset = CsoPreset::Fast;
	});
	add(Format::DAX, "dax fast", [](Settings &s) {
		s.cso.useDefault = false;
		s.cso.preset = CsoPreset::Fast;
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
			s.rvz.useDefault = false;
			s.rvz.method = RVZ_PURGE;
		});
		addr(Format::WIA, "wia lzma 4M", [](Settings &s) {
			s.rvz.useDefault = false;
			s.rvz.method = RVZ_LZMA;
			s.rvz.level = 9;
			s.rvz.chunk = 4 << 20;
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
