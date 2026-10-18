// isopackager-cli: the command line face of ISOPackager. Same engine as the GUI.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/bench.h"
#include "core/engine.h"
#include "core/queue.h"
#include "core/scan.h"
#include "core/settings.h"
#include "core/sysinfo.h"
#include "core/util.h"

using namespace isopk;

namespace {

std::atomic<bool> g_interrupted{ false };
bool g_quiet = false, g_tty = false;

void OnSignal(int) {
	if (g_interrupted.exchange(true)) {
		std::_Exit(130);  // second Ctrl+C: leave now
	}
}

void Usage() {
	printf(
		"ISOPackager %s - compress, extract, repack and benchmark PSP disc images\n"
		"%s\n\n"
		"Usage: isopackager-cli <command> [options] <files or folders...>\n\n"
		"Commands:\n"
		"  convert   Convert to the format given with --to (default cso). Compressed sources are\n"
		"            repacked through RAM; ISO targets are extracted.\n"
		"  compress  Same as convert (kept for habit).\n"
		"  extract   Convert to ISO.\n"
		"  verify    Decode each image completely and report errors (CHD checksums, PBP MACs).\n"
		"            With --against ISO, also compare with that ISO byte for byte.\n"
		"  info      Show format, sizes, compression settings, disc ID and title.\n"
		"  bench     Benchmark settings on the given images (see Benchmark options).\n"
		"  zar       Pack a folder's contents (or several files and folders) into one ZArchive\n"
		"            (.zar): discs, DLC folders and the like. See ZArchive options.\n"
		"  formats   List the supported formats.\n\n"
		"Output:\n"
		"  -f, --to FMT           iso, cso, cso2, zso, dax, chd, pbp, rvz, wia\n"
		"  -o, --output DIR       Output folder (default: next to each source)\n"
		"      --flat             Don't recreate the scanned folders' structure under --output\n"
		"      --no-recursive     Don't search subfolders\n"
		"      --skip | --overwrite | --rename   When the output exists (default skip)\n"
		"      --delete-source    Delete each source after a successful, verified conversion\n"
		"      --no-verify        Don't decode outputs again to compare with the source\n"
		"      --no-ram           Repack through a temporary ISO instead of RAM\n"
		"      --ram-budget SIZE  RAM for in-memory images across jobs (e.g. 4G; default ~60%% of free)\n"
		"      --temp DIR         Folder for temporary ISOs (default: the output folder)\n"
		"  -j, --jobs N           Files converted at once (default 1; each is multithreaded)\n"
		"  -t, --threads N        Worker threads per job (default: all cores)\n"
		"      --normal-priority  Don't lower the process priority\n"
		"      --config FILE      Load settings from an ISOPackager .ini (the GUI's, by default none)\n"
		"      --save-config FILE Save the resulting settings and exit\n"
		"  -q, --quiet            Only errors and the final summary\n\n"
		"CSO / ZSO / CSO v2 / DAX (mCSO):\n"
		"      --preset P         default (libdeflate, 64K blocks), fast, standard, max\n"
		"      --methods LIST     Custom method set: zlib,zlib-brute,7z,zopfli,libdeflate,lz4,lz4hc,lz4hc-brute\n"
		"      --block SIZE       Block size, power of two 2K..256K (default auto)\n"
		"      --splits N         Zopfli block splits (default 15)\n"
		"      --precheck PCT     Skip slow trials on blocks libdeflate can't shrink below PCT%%\n"
		"      --force-all        Compress every block, even when it doesn't help\n\n"
		"CHD (CHDmodern):\n"
		"      --hunk SIZE        Hunk size, multiple of 2048 (default 4096)\n"
		"      --codecs LIST      default, or up to 4 of lzma,zlib,zstd,huff,flac joined by +\n\n"
		"RVZ / WIA (iRVZ, GameCube and Wii discs):\n"
		"      --rvz-method M     lzma2 (default), zstd, lzma, bzip2, none; WIA also purge, not zstd\n"
		"      --rvz-level N      bzip2/lzma/lzma2 1-9 (default 5), zstd -22..22\n"
		"      --rvz-chunk SIZE   RVZ: power of two from 32K or a multiple of 2M (default 128K);\n"
		"                         WIA: a multiple of 2M (default 2M)\n\n"
		"PBP (iPBP):\n"
		"      --pbp-mode M       none, lzrc (default), max\n"
		"      --content-id ID    Content ID (default derived from the disc ID)\n"
		"      --boot-png FILE    480x272 boot screen\n"
		"      --pbp-file         Write Game.pbp instead of Game/EBOOT.PBP\n"
		"      --no-trim          Keep PBP block padding when extracting\n"
		"      --no-mac-check     Don't verify PBP block MACs when reading\n\n"
		"ZArchive options (zar):\n"
		"      --name NAME        Archive name (default: the folder's name)\n"
		"      --zar-level N      zstd level 1-22 (default 22)\n"
		"      --zar-precheck PCT Store blocks zstd's fastest level can't shrink below PCT%% (default 95,\n"
		"                         0 = always compress)\n"
		"      --store-discs      Don't compress disc images (use when they're already CSO, CHD, ...)\n"
		"      --compress-discs FMT  Compress plain ISOs to FMT (cso, zso, chd, pbp, rvz...) in RAM first,\n"
		"                         and store them; the format's options above apply\n\n"
		"Benchmark options (bench):\n"
		"      --cso-sets LIST    A,B,C,D,L (img_bench sets, 256K) and F,S,X (presets); default B,D,L\n"
		"      --codecs LIST      CHD codec sets, comma separated, + inside a set; default default,lzma,zstd\n"
		"      --hunks LIST       CHD hunk sizes; default 2048,4096,16384,65536\n"
		"      --pbp-modes LIST   lzrc,max,none (default all)\n"
		"      --extra LIST       Also zso,cso2,dax (with the CSO options) and rvz,wia (with the --rvz-* options)\n"
		"      --no-cso | --no-chd | --no-pbp\n"
		"      --reads N          Random reads per image (default 4000)\n"
		"      --source-on-disk   Compress from the ISO file instead of RAM\n"
		"      --work DIR         Results folder (default %s)\n"
		"      --no-verify        Don't compare the decoded images\n\n"
		"Examples:\n"
		"  isopackager-cli convert --to chd --codecs lzma+zstd -o D:\\CHD D:\\ISO\n"
		"  isopackager-cli convert --to cso --preset max --block 16K game.chd\n"
		"  isopackager-cli extract EBOOT.PBP\n"
		"  isopackager-cli zar --compress-discs cso -o D:\\ZAR \"D:\\Games\\Trails in the Sky\"\n"
		"  isopackager-cli bench --no-cso --codecs default --hunks 8192,32768 D:\\ISO\n",
		ISOPK_VERSION, LibraryVersions().c_str(), DefaultBenchDir().c_str());
}

struct Args {
	std::string command;
	std::vector<std::string> inputs;
	Settings s;
	bool explicitTarget = false;
	std::string saveConfig, against;
	std::string zarName;
	// bench
	std::string csoSets = "B,D,L", codecs = "default,lzma,zstd", hunks = "2048,4096,16384,65536", pbpModes = "lzrc,max,none", extra;
	bool noCso = false, noChd = false, noPbp = false, sourceOnDisk = false;
	int reads = 4000;
	std::string work;
};

bool ParseU32Size(const std::string &v, uint32_t *out) {
	uint64_t x;
	if (!ParseSize(v, &x) || x > 0xFFFFFFFFull)
		return false;
	*out = (uint32_t)x;
	return true;
}

bool ParseArgs(int argc, char **argv, Args &a, std::string &err) {
	if (argc < 2) {
		err = "";
		return false;
	}
	a.command = ToLower(argv[1]);
	if (a.command == "-h" || a.command == "--help" || a.command == "help") {
		a.command = "help";
		return true;
	}
	// Load --config first, so the other options override it.
	for (int i = 2; i + 1 < argc; ++i)
		if (strcmp(argv[i], "--config") == 0 && !a.s.Load(argv[i + 1])) {
			err = std::string("can't read ") + argv[i + 1];
			return false;
		}
	if (a.command == "extract")
		a.s.target = Format::ISO;
	else if (a.command == "convert" || a.command == "compress")
		a.s.target = Settings().target;

	bool methodsGiven = false;
	bool csoTouched = false, rvzTouched = false;
	for (int i = 2; i < argc; ++i) {
		std::string o = argv[i];
		auto val = [&](std::string &out) {
			if (i + 1 >= argc) {
				err = o + " needs a value";
				return false;
			}
			out = argv[++i];
			return true;
		};
		std::string v;
		if (o == "-f" || o == "--to" || o == "--format") {
			if (!val(v))
				return false;
			Format f = FormatFromName(v);
			if (f == Format::Unknown || !FormatAvailable(f)) {
				err = "unknown or unsupported format: " + v;
				return false;
			}
			a.s.target = f;
			a.explicitTarget = true;
		} else if (o == "--name") {
			if (!val(a.zarName))
				return false;
		} else if (o == "--zar-level") {
			if (!val(v))
				return false;
			a.s.zar.level = std::clamp(atoi(v.c_str()), 1, 22);
		} else if (o == "--zar-precheck") {
			if (!val(v))
				return false;
			a.s.zar.precheck = std::clamp(atof(v.c_str()), 0.0, 100.0);
		} else if (o == "--store-discs") {
			a.s.zar.storeDiscs = true;
		} else if (o == "--compress-discs") {
			if (!val(v))
				return false;
			Format f = FormatFromName(v);
			if (f == Format::Unknown || f == Format::ISO || f == Format::ZAR || !FormatAvailable(f)) {
				err = "--compress-discs takes a compressed disc format: " + v;
				return false;
			}
			a.s.zar.compressDiscs = true;
			a.s.zar.discFormat = f;
		} else if (o == "-o" || o == "--output") {
			if (!val(a.s.out.dir))
				return false;
		} else if (o == "--flat") {
			a.s.out.mirrorTree = false;
		} else if (o == "--no-recursive") {
			a.s.recursive = false;
		} else if (o == "-r" || o == "--recursive") {
			a.s.recursive = true;
		} else if (o == "--skip") {
			a.s.out.conflict = Conflict::Skip;
		} else if (o == "--overwrite") {
			a.s.out.conflict = Conflict::Overwrite;
		} else if (o == "--rename") {
			a.s.out.conflict = Conflict::Rename;
		} else if (o == "--delete-source") {
			a.s.out.deleteSource = true;
		} else if (o == "--no-verify") {
			a.s.engine.verify = false;
		} else if (o == "--verify") {
			a.s.engine.verify = true;
		} else if (o == "--no-ram") {
			a.s.engine.repackInRam = false;
		} else if (o == "--ram-budget") {
			if (!val(v) || !ParseSize(v, &a.s.engine.ramBudget)) {
				err = "bad --ram-budget";
				return false;
			}
		} else if (o == "--temp") {
			if (!val(a.s.engine.tempDir))
				return false;
		} else if (o == "-j" || o == "--jobs") {
			if (!val(v))
				return false;
			a.s.engine.jobs = std::max(1, atoi(v.c_str()));
		} else if (o == "-t" || o == "--threads") {
			if (!val(v))
				return false;
			a.s.engine.threads = (unsigned)std::max(0, atoi(v.c_str()));
		} else if (o == "--normal-priority") {
			a.s.engine.lowPriority = false;
		} else if (o == "--config") {
			++i;  // handled above
		} else if (o == "--save-config") {
			if (!val(a.saveConfig))
				return false;
		} else if (o == "-q" || o == "--quiet") {
			g_quiet = true;
		} else if (o == "--preset") {
			csoTouched = true;
			if (!val(v))
				return false;
			v = ToLower(v);
			a.s.cso.useDefault = v == "default";
			if (v == "default") {
			} else if (v == "fast") a.s.cso.preset = CsoPreset::Fast;
			else if (v == "standard") a.s.cso.preset = CsoPreset::Standard;
			else if (v == "max" || v == "maximum") a.s.cso.preset = CsoPreset::Max;
			else if (v == "custom") a.s.cso.preset = CsoPreset::Custom;
			else {
				err = "unknown preset: " + v;
				return false;
			}
		} else if (o == "--methods") {
			csoTouched = true;
			if (!val(v))
				return false;
			CsoSettings &c = a.s.cso;
			c.useDefault = false;
			c.preset = CsoPreset::Custom;
			c.zlib = c.zlibBrute = c.sevenZip = c.zopfli = c.libdeflate = c.lz4 = c.lz4hc = c.lz4Brute = false;
			for (const std::string &m : Split(ToLower(v), ',')) {
				if (m == "zlib") c.zlib = true;
				else if (m == "zlib-brute") c.zlibBrute = true;
				else if (m == "7z" || m == "7zip" || m == "7zdeflate") c.sevenZip = true;
				else if (m == "zopfli") c.zopfli = true;
				else if (m == "libdeflate") c.libdeflate = true;
				else if (m == "lz4") c.lz4 = true;
				else if (m == "lz4hc") c.lz4hc = true;
				else if (m == "lz4hc-brute" || m == "lz4brute") c.lz4Brute = true;
				else {
					err = "unknown method: " + m;
					return false;
				}
			}
			methodsGiven = true;
		} else if (o == "--block") {
			csoTouched = true;
			if (!val(v) || !ParseU32Size(v, &a.s.cso.blockSize) ||
				(a.s.cso.blockSize && (a.s.cso.blockSize < 2048 || a.s.cso.blockSize > 262144 || (a.s.cso.blockSize & (a.s.cso.blockSize - 1))))) {
				err = "--block must be a power of two from 2K to 256K";
				return false;
			}
		} else if (o == "--splits") {
			csoTouched = true;
			if (!val(v))
				return false;
			a.s.cso.zopfliSplits = (uint32_t)std::max(0, atoi(v.c_str()));
		} else if (o == "--precheck") {
			csoTouched = true;
			if (!val(v))
				return false;
			a.s.cso.precheck = std::clamp(atof(v.c_str()), 0.0, 100.0);
		} else if (o == "--force-all") {
			csoTouched = true;
			a.s.cso.forceAll = true;
		} else if (o == "--hunk") {
			if (!val(v) || !ParseU32Size(v, &a.s.chd.hunkBytes) || a.s.chd.hunkBytes % 2048) {
				err = "--hunk must be a multiple of 2048";
				return false;
			}
		} else if (o == "--codecs") {
			if (!val(v))
				return false;
			a.codecs = v;  // bench: a comma list of sets
			if (a.command != "bench" && !a.s.chd.SetCodecs(v)) {
				err = "bad --codecs (default, or up to 4 of lzma,zlib,zstd,huff,flac joined by +)";
				return false;
			}
		} else if (o == "--rvz-method") {
			rvzTouched = true;
			if (!val(v))
				return false;
			v = ToLower(v);
			int m = -1;
			for (int k = RVZ_NONE; k <= RVZ_ZSTD; ++k)
				if (v == RvzMethodName(k))
					m = k;
			if (m < 0) {
				err = "--rvz-method must be none, purge, bzip2, lzma, lzma2 or zstd";
				return false;
			}
			a.s.rvz.useDefault = false;
			a.s.rvz.method = m;
		} else if (o == "--rvz-level") {
			rvzTouched = true;
			if (!val(v))
				return false;
			a.s.rvz.useDefault = false;
			a.s.rvz.level = atoi(v.c_str());
		} else if (o == "--rvz-chunk") {
			rvzTouched = true;
			if (!val(v) || !ParseU32Size(v, &a.s.rvz.chunk) || a.s.rvz.chunk < (32u << 10)) {
				err = "bad --rvz-chunk (e.g. 128K, 2M)";
				return false;
			}
			a.s.rvz.useDefault = false;
		} else if (o == "--pbp-mode") {
			if (!val(v))
				return false;
			v = ToLower(v);
			a.s.pbp.compress = v == "none" ? 0 : v == "max" ? 2 : v == "lzrc" ? 1 : -1;
			if (a.s.pbp.compress < 0) {
				err = "--pbp-mode must be none, lzrc or max";
				return false;
			}
		} else if (o == "--content-id") {
			if (!val(a.s.pbp.contentId))
				return false;
		} else if (o == "--boot-png") {
			if (!val(a.s.pbp.startDatPng))
				return false;
		} else if (o == "--pbp-file") {
			a.s.pbp.layout = PbpLayout::File;
		} else if (o == "--no-trim") {
			a.s.pbp.trimOnExtract = false;
		} else if (o == "--no-mac-check") {
			a.s.pbp.verifyMacs = false;
		} else if (o == "--against") {
			if (!val(a.against))
				return false;
		} else if (o == "--cso-sets") {
			if (!val(a.csoSets))
				return false;
		} else if (o == "--hunks") {
			if (!val(a.hunks))
				return false;
		} else if (o == "--pbp-modes") {
			if (!val(a.pbpModes))
				return false;
		} else if (o == "--extra") {
			if (!val(a.extra))
				return false;
		} else if (o == "--no-cso") {
			a.noCso = true;
		} else if (o == "--no-chd") {
			a.noChd = true;
		} else if (o == "--no-pbp") {
			a.noPbp = true;
		} else if (o == "--reads") {
			if (!val(v))
				return false;
			a.reads = std::max(0, atoi(v.c_str()));
		} else if (o == "--source-on-disk") {
			a.sourceOnDisk = true;
		} else if (o == "--work") {
			if (!val(a.work))
				return false;
		} else if (!o.empty() && o[0] == '-' && o.size() > 1) {
			err = "unknown option " + o;
			return false;
		} else {
			a.inputs.push_back(o);
		}
	}
	(void)methodsGiven;
	// Tuning options on top of the default: the same method set (libdeflate), made explicit.
	CsoSettings &c = a.s.cso;
	if (c.useDefault && (c.blockSize || c.zopfliSplits || c.precheck > 0 || c.forceAll)) {
		c.useDefault = false;
		c.preset = CsoPreset::Custom;
		c.zlib = c.zlibBrute = c.sevenZip = c.zopfli = c.lz4Brute = false;
		c.libdeflate = c.lz4 = c.lz4hc = true;
		if (!c.blockSize)
			c.blockSize = CsoSettings::DEFAULT_BLOCK;
	}
	// The CSO and RVZ options set up whichever container the conversion (or bench --extra) uses.
	if (csoTouched)
		a.s.zso = a.s.cso2 = a.s.dax = a.s.cso;
	if (rvzTouched)
		a.s.wia = a.s.rvz;
	if (a.command == "extract" && a.explicitTarget && a.s.target != Format::ISO) {
		err = "extract always writes ISO; use convert --to";
		return false;
	}
	return true;
}

void PrintLine(const std::string &s) {
	if (g_tty)
		printf("\r\033[K");
	printf("%s\n", s.c_str());
	fflush(stdout);
}

int RunConvert(Args &a) {
	if (a.s.target == Format::ZAR) {
		fprintf(stderr, "ZArchives are made with the zar command.\n");
		return 1;
	}
	std::vector<std::string> warnings;
	std::vector<InputFile> files = ScanInputs(a.inputs, a.s.recursive, &warnings, &g_interrupted);
	for (auto &w : warnings)
		fprintf(stderr, "warning: %s\n", w.c_str());
	if (files.empty()) {
		fprintf(stderr, "No disc images found.\n");
		return 1;
	}
	if (!g_quiet)
		PrintLine(Fmt("%zu image(s) -> %s, %u job(s) at once, verify %s, RAM budget %s", files.size(), FormatName(a.s.target),
			a.s.engine.jobs, a.s.engine.verify ? "on" : "off",
			HumanBytes(a.s.engine.ramBudget ? a.s.engine.ramBudget : DefaultRamBudget()).c_str()));

	JobQueue q;
	q.SetSettings(a.s);
	for (const InputFile &f : files)
		q.Add(f, a.s.target);
	q.Start();

	// Progress line while waiting.
	std::thread waiter([&] { q.WaitIdle(); });
	auto t0 = Clock::now();
	bool cancelledAll = false;
	while (q.Busy()) {
		if (g_interrupted && !cancelledAll) {
			PrintLine("Cancelling... (Ctrl+C again to quit at once)");
			q.CancelAll();
			cancelledAll = true;
		}
		if (g_tty && !g_quiet) {
			std::string line;
			auto c = q.Count();
			line = Fmt("[%d/%zu] ", c.done + c.skipped + c.failed + c.cancelled, files.size());
			for (auto &j : q.Jobs()) {
				if (j->State() != JobState::Running)
					continue;
				line += Fmt("%s: %s %.0f%%  ", GameName(j->input.path).c_str(), PhaseName(j->CurrentPhase()), j->phaseProgress.load() * 100);
			}
			if (line.size() > 150)
				line = line.substr(0, 147) + "...";
			printf("\r\033[K%s", line.c_str());
			fflush(stdout);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	waiter.join();
	if (g_tty)
		printf("\r\033[K");

	auto c = q.Count();
	PrintLine(Fmt("%d done, %d skipped, %d failed, %d cancelled in %s. %s -> %s", c.done, c.skipped, c.failed, c.cancelled,
		HumanDuration(SecondsSince(t0)).c_str(), HumanBytes(c.inBytes).c_str(), HumanBytes(c.outBytes).c_str()));
	if (c.failed) {
		for (auto &j : q.Jobs())
			if (j->State() == JobState::Failed)
				fprintf(stderr, "  FAILED %s: %s\n", j->input.path.c_str(), j->Result().message.c_str());
	}
	return c.failed ? 2 : (c.cancelled ? 130 : 0);
}

int RunZar(Args &a) {
	ZarSpec spec;
	std::error_code ec;
	if (a.inputs.size() == 1 && fs::is_directory(U8Path(a.inputs[0]), ec))
		spec.rootDir = a.inputs[0];
	else
		spec.extras = a.inputs;
	spec.name = a.zarName;
	if (spec.name.empty()) {
		fs::path p = U8Path(a.inputs[0]);
		if (!p.has_filename())
			p = p.parent_path();
		spec.name = spec.rootDir.empty() ? PathU8(p.stem()) : PathU8(p.filename());
	}
	std::string err;
	if (!MeasureZarSpec(&spec, &err)) {
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	if (!g_quiet)
		PrintLine(Fmt("ZArchive %s: %s from %zu path(s), %s, verify %s -> %s", spec.name.c_str(), HumanBytes(spec.totalSize).c_str(),
			spec.Paths().size(), a.s.zar.Describe().c_str(), a.s.engine.verify ? "on" : "off", PlanZarOutputPath(spec, a.s).c_str()));
	JobQueue q;
	q.SetSettings(a.s);
	auto job = q.AddZArchive(spec);
	q.Start();
	std::thread waiter([&] { q.WaitIdle(); });
	bool cancelled = false;
	while (q.Busy()) {
		if (g_interrupted && !cancelled) {
			PrintLine("Cancelling... (Ctrl+C again to quit at once)");
			q.CancelAll();
			cancelled = true;
		}
		if (g_tty && !g_quiet) {
			printf("\r\033[K%s: %s %.0f%%", spec.name.c_str(), PhaseName(job->CurrentPhase()), job->overall.load() * 100);
			fflush(stdout);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	waiter.join();
	if (g_tty)
		printf("\r\033[K");
	JobResult r = job->Result();
	switch (r.state) {
	case JobState::Done:
		PrintLine(Fmt("Done: %s, %s -> %s (%.1f%%) in %s, verify %s. %s", r.outputPath.c_str(), HumanBytes(r.inputSize).c_str(),
			HumanBytes(r.outputSize).c_str(), r.inputSize ? 100.0 * r.outputSize / r.inputSize : 0.0, HumanDuration(r.seconds).c_str(),
			r.verify.c_str(), r.message.c_str()));
		return 0;
	case JobState::Skipped:
		PrintLine("Skipped: " + r.message);
		return 0;
	case JobState::Cancelled:
		PrintLine("Cancelled");
		return 130;
	default:
		fprintf(stderr, "FAILED: %s\n", r.message.c_str());
		return 2;
	}
}

int RunVerify(Args &a) {
	std::vector<std::string> warnings;
	std::vector<InputFile> files = ScanInputs(a.inputs, a.s.recursive, &warnings, &g_interrupted);
	for (auto &w : warnings)
		fprintf(stderr, "warning: %s\n", w.c_str());
	if (files.empty()) {
		fprintf(stderr, "No disc images found.\n");
		return 1;
	}
	ApplyEngineSettings(a.s.engine);
	int bad = 0;
	FileImageReader ref;
	if (!a.against.empty() && !ref.Open(a.against)) {
		fprintf(stderr, "%s: %s\n", a.against.c_str(), ref.Error().c_str());
		return 1;
	}
	for (const InputFile &f : files) {
		if (g_interrupted)
			break;
		OpContext ctx;
		ctx.cancel = &g_interrupted;
		ctx.progress = [&](double p) {
			if (g_tty && !g_quiet) {
				printf("\r\033[K  %s %.0f%%", GameName(f.path).c_str(), p * 100);
				fflush(stdout);
			}
		};
		auto t0 = Clock::now();
		std::string verdict;
		bool ok;
		if (!a.against.empty()) {
			CompareWriter cmp(&ref);
			ok = DecompressToWriter(f.path, f.format, a.s, cmp, false, ctx);
			verdict = ok ? cmp.Result() : ctx.error;
			ok = ok && cmp.Matched();
		} else {
			NullWriter nw;
			ok = DecompressToWriter(f.path, f.format, a.s, nw, false, ctx);
			verdict = ok ? "decodes cleanly" : ctx.error;
			if (ok && f.format == Format::ISO) {
				FileImageReader r;
				uint64_t vol = r.Open(f.path) ? IsoVolumeSize(r) : 0;
				if (vol && vol != r.Size())
					verdict = Fmt("file is %s, ISO9660 volume is %s (%s)", HumanBytes(r.Size()).c_str(), HumanBytes(vol).c_str(),
						r.Size() > vol ? "padded" : "TRUNCATED");
				ok = !vol || r.Size() >= vol;
			}
		}
		if (!ok)
			++bad;
		PrintLine(Fmt("%s  %s: %s (%s)", ok ? "OK  " : "BAD ", f.path.c_str(), verdict.c_str(), HumanDuration(SecondsSince(t0)).c_str()));
	}
	return bad ? 2 : 0;
}

int RunInfo(Args &a) {
	std::vector<std::string> warnings;
	std::vector<InputFile> files = ScanInputs(a.inputs, a.s.recursive, &warnings);
	for (auto &w : warnings)
		fprintf(stderr, "warning: %s\n", w.c_str());
	for (const InputFile &f : files) {
		ImageInfo i = InspectImage(f.path);
		printf("%s\n", f.path.c_str());
		printf("  format   %s%s%s\n", FormatName(i.format), i.detail.empty() ? "" : ", ", i.detail.c_str());
		printf("  file     %s (%llu bytes)\n", HumanBytes(i.fileSize).c_str(), (unsigned long long)i.fileSize);
		if (i.isoSize)
			printf("  image    %s (%llu bytes), %.2f%% of it\n", HumanBytes(i.isoSize).c_str(), (unsigned long long)i.isoSize,
				100.0 * i.fileSize / i.isoSize);
		if (i.platform != Platform::Unknown)
			printf("  platform %s\n", PlatformLabel(i.platform));
		if (!i.discId.empty())
			printf("  disc ID  %s\n", i.discId.c_str());
		if (!i.title.empty())
			printf("  title    %s\n", i.title.c_str());
		if (!i.error.empty())
			printf("  problem  %s\n", i.error.c_str());
	}
	return files.empty() ? 1 : 0;
}

int RunBench(Args &a) {
	std::vector<std::string> warnings;
	std::vector<InputFile> files = ScanInputs(a.inputs.empty() ? std::vector<std::string>{ "." } : a.inputs, a.s.recursive, &warnings);
	for (auto &w : warnings)
		fprintf(stderr, "warning: %s\n", w.c_str());
	if (files.empty()) {
		fprintf(stderr, "No disc images found.\n");
		return 1;
	}
	std::sort(files.begin(), files.end(), [](const InputFile &x, const InputFile &y) { return x.size < y.size; });

	BenchConfig cfg;
	cfg.verify = a.s.engine.verify;
	cfg.reads = a.reads;
	cfg.sourceInRam = !a.sourceOnDisk;
	cfg.workDir = a.work;
	cfg.threads = a.s.engine.threads;
#ifdef ISOPK_WITH_CSO
	if (!a.noCso) {
		for (const std::string &k : Split(a.csoSets, ',')) {
			BenchCase c;
			if (!MakeCsoCase(k, &c)) {
				fprintf(stderr, "unknown CSO set %s (A,B,C,D,L,F,S,X)\n", k.c_str());
				return 1;
			}
			cfg.cases.push_back(c);
		}
	}
#endif
	for (const std::string &e : Split(a.extra, ',')) {
		Format f = FormatFromName(e);
		if (IsIrvzFormat(f)) {
			cfg.cases.push_back(MakeRvzCase(f, a.s.Rvz(f)));
			continue;
		}
		if (!IsMcsoFormat(f) || !FormatAvailable(f)) {
			fprintf(stderr, "--extra takes zso, cso2, dax, cso, rvz, wia\n");
			return 1;
		}
		cfg.cases.push_back(MakeMcsoCase(f, a.s.Cso(f)));
	}
	if (!a.noChd) {
		for (const std::string &codec : Split(a.codecs, ',')) {
			ChdSettings probe;
			if (!probe.SetCodecs(codec)) {
				fprintf(stderr, "bad CHD codec set %s\n", codec.c_str());
				return 1;
			}
			for (const std::string &h : Split(a.hunks, ',')) {
				uint32_t hb;
				if (!ParseU32Size(h, &hb) || hb == 0 || hb % 2048) {
					fprintf(stderr, "bad hunk size %s (multiples of 2048)\n", h.c_str());
					return 1;
				}
				cfg.cases.push_back(MakeChdCase(codec, hb));
			}
		}
	}
	if (!a.noPbp) {
		for (const std::string &m : Split(ToLower(a.pbpModes), ',')) {
			int c = m == "none" ? 0 : m == "lzrc" ? 1 : m == "max" ? 2 : -1;
			if (c < 0) {
				fprintf(stderr, "bad PBP mode %s (lzrc, max, none)\n", m.c_str());
				return 1;
			}
			cfg.cases.push_back(MakePbpCase(c));
		}
	}
	if (cfg.cases.empty()) {
		fprintf(stderr, "Nothing to benchmark.\n");
		return 1;
	}
	ApplyEngineSettings(a.s.engine);
	BenchCallbacks cb;
	cb.progress = [&](const std::string &what, double f, double) {
		if (g_tty && !what.empty()) {
			std::string w = what.size() > 100 ? what.substr(0, 97) + "..." : what;
			printf("\r\033[K  %s %5.1f%%", w.c_str(), f * 100);
			fflush(stdout);
		}
	};
	auto rows = RunBenchmark(files, cfg, cb, &g_interrupted);
	if (g_tty)
		printf("\r\033[K");
	int failed = 0;
	for (auto &r : rows)
		failed += (r.failed && r.error.rfind("skipped", 0) != 0) || (!r.failed && r.verify != "OK" && r.verify != "not compared" && r.verify.rfind("OK", 0) != 0);
	return g_interrupted ? 130 : failed ? 2 : 0;
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	DWORD mode = 0;
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	g_tty = GetConsoleMode(out, &mode) != 0;
	if (g_tty)
		SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
	g_tty = isatty(1) != 0;
#endif
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);

	Args a;
	std::string err;
	if (!ParseArgs(argc, argv, a, err) || a.command == "help") {
		Usage();
		if (!err.empty())
			fprintf(stderr, "\nerror: %s\n", err.c_str());
		return a.command == "help" ? 0 : 1;
	}

	SetLogSink([](LogLevel lvl, const std::string &msg) {
		if (lvl == LogLevel::Debug)
			return;
		// The benchmark's table lines are the output; job lines are progress.
		if (g_quiet && lvl == LogLevel::Info)
			return;
		if (lvl >= LogLevel::Warning) {
			if (g_tty)
				fprintf(stderr, "\r\033[K");
			fprintf(stderr, "%s%s\n", lvl == LogLevel::Error ? "error: " : "warning: ", msg.c_str());
		} else {
			PrintLine(msg);
		}
	});

	if (!a.saveConfig.empty()) {
		if (!a.s.Save(a.saveConfig)) {
			fprintf(stderr, "can't write %s\n", a.saveConfig.c_str());
			return 1;
		}
		printf("Saved %s\n", a.saveConfig.c_str());
		return 0;
	}

	if (a.command == "formats") {
		for (int i = 1; i < (int)Format::Count; ++i) {
			Format f = (Format)i;
			printf("  %-5s %-6s %s%s\n", FormatKey(f), FormatExtension(f), FormatDescription(f), FormatAvailable(f) ? "" : " (not in this build)");
		}
		return 0;
	}
	if (a.inputs.empty() && a.command != "bench") {
		Usage();
		fprintf(stderr, "\nerror: no input files or folders\n");
		return 1;
	}
	if (a.command == "convert" || a.command == "compress" || a.command == "extract")
		return RunConvert(a);
	if (a.command == "verify")
		return RunVerify(a);
	if (a.command == "zar")
		return RunZar(a);
	if (a.command == "info")
		return RunInfo(a);
	if (a.command == "bench")
		return RunBench(a);
	Usage();
	fprintf(stderr, "\nerror: unknown command %s\n", a.command.c_str());
	return 1;
}
