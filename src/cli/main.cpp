// isopackager-cli: the command line face of ISOPackager. Same engine and settings as the GUI.
//
//   isopackager-cli <command> [options] <files or folders...>
//
// The command is the format to make (cso, chd, pbp, ...), iso to extract, zar to pack an archive,
// or verify / info / bench / formats. Every format works without options (our recommended
// settings); --method, --level and --block tune whichever format is being made.
// docs/cli.md describes everything.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chdmodern/chdmodern.h>

#include "core/bench.h"
#include "core/engine.h"
#include "core/queue.h"
#include "core/scan.h"
#include "core/settings.h"
#include "core/sysinfo.h"
#include "core/util.h"
#include "core/zar.h"

using namespace isopk;

namespace {

std::atomic<bool> g_interrupted{ false };
bool g_quiet = false, g_tty = false;

void OnSignal(int) {
	if (g_interrupted.exchange(true)) {
		std::_Exit(130);  // second Ctrl+C: leave now
	}
}

void PrintLine(const std::string &s) {
	if (g_tty)
		printf("\r\033[K");
	printf("%s\n", s.c_str());
	fflush(stdout);
}

void Usage() {
	printf(
		"ISOPackager %s - compress, extract, repack, archive and benchmark disc images\n"
		"%s\n\n"
		"Usage: isopackager-cli <command> [options] <files or folders...>\n\n"
		"Commands (every format works without options, with the recommended settings):\n"
		"  cso  zso  cso2  dax  chd  pbp  rvz  wia\n"
		"               Make that format from any image: ISOs are compressed, compressed images\n"
		"               are converted through RAM. Folders are searched for images.\n"
		"  iso          Extract to ISO (a .zar is extracted to a folder next to it, a CD image to\n"
		"               cue/bin). Alias: extract\n"
		"  cue          CD images (cue/bin, CloneCD, Alcohol, GDI, Nero, raw .bin) and CD CHDs to\n"
		"               cue/bin; the chd command makes CD CHDs of them\n"
		"  zar          Pack a folder (its contents become the archive) and/or files into one .zar\n"
		"  convert      Same as the format commands, with --to FMT (default cso)\n"
		"  verify       Decode every image and report problems (.zar: check its SHA-256)\n"
		"  info         Format, sizes, disc ID and title of each image\n"
		"  bench        Benchmark settings on the given images\n"
		"  formats      List the formats and their default settings\n\n"
		"Format options (for the format being made; see 'formats' for the defaults):\n"
		"  --method M     cso/cso2/dax: libdeflate,zlib,zlib-brute,7zip,zopfli   zso/cso2: lz4,lz4hc,lz4hc-brute\n"
		"                 chd: up to 4 of lzma,zlib,zstd,huff,flac   pbp: lzrc, max, none\n"
		"                 rvz: lzma2, zstd, lzma, bzip2, none   wia: lzma2, lzma, bzip2, purge, none\n"
		"                 'default' for the recommended set\n"
		"  --level N      rvz/wia: bzip2/lzma/lzma2 1..9, zstd -22..22\n"
		"  --block SIZE   cso/zso/cso2: block 2K..256K (power of two) or auto   chd: hunk (x2048)\n"
		"                 rvz: chunk 32K..2M (power of two) or a multiple of 2M   wia: a multiple of 2M\n"
		"  --zopfli-splits N, --precheck PCT, --force-all     cso family (see docs/cli.md)\n"
		"  --content-id ID, --boot-png FILE, --pbp-file       pbp\n"
		"  --single-bin   cue: one Game.bin instead of one .bin per track (Redump style)\n\n"
		"Output and engine:\n"
		"  -o, --output DIR    Output folder (default: next to each source)\n"
		"  --flat              Don't recreate the scanned folders under --output\n"
		"  --no-recursive      Don't search subfolders\n"
		"  --skip | --overwrite | --rename   When the output exists (default: skip)\n"
		"  --delete-source     Delete each source after a successful (verified) conversion\n"
		"  --no-verify         Don't decode outputs again to compare them with the source\n"
		"  --no-ram            Repack through a temporary ISO instead of RAM\n"
		"  --ram-budget SIZE   RAM for in-memory images (e.g. 4G; default about 60%% of free RAM)\n"
		"  --temp DIR          Folder for temporary files (default: the output folder)\n"
		"  -j, --jobs N        Files converted at once (default 1)\n"
		"  -t, --threads N     Worker threads per job (default: all cores)\n"
		"  --normal-priority   Don't lower the process priority\n"
		"  --no-trim           Keep PBP padding when extracting   --no-mac-check  Skip PBP MAC checks\n"
		"  --gui-settings      Start from the GUI's saved settings   --config FILE  from an .ini\n"
		"  --save-config FILE  Save the resulting settings and exit\n"
		"  -q, --quiet         Only errors and the final summary\n\n"
		"zar:  --name NAME  --zar-level N (1-22, default 22)  --zar-precheck PCT (default 95)\n"
		"      --store-discs  --discs FMT (put every disc in FMT first, through RAM)  --no-root\n"
		"verify:  --against ISO     compare each image with this ISO byte for byte\n"
		"         (a CD image with a CD CHD, or a CD CHD with its cue/bin)\n"
		"bench:   --case \"FMT [format options]\" (repeatable; default: the GUI's benchmark list)\n"
		"         --reads N  --source-on-disk  --work DIR\n\n"
		"Examples:\n"
		"  isopackager-cli cso game.iso\n"
		"  isopackager-cli chd -o D:\\CHD D:\\ISO\n"
		"  isopackager-cli cso --method zopfli,libdeflate --block 16K game.chd\n"
		"  isopackager-cli rvz --method zstd --level 19 D:\\Wii\n"
		"  isopackager-cli zar --discs cso \"D:\\Games\\Trails in the Sky\"\n"
		"  isopackager-cli iso \"Trails in the Sky.zar\" EBOOT.PBP\n"
		"  isopackager-cli bench --case cso --case \"chd --block 16K\" game.iso\n\n"
		"Full guide: docs/cli.md (https://github.com/LunaMoo/ISOPackager/blob/main/docs/cli.md)\n",
		ISOPK_VERSION, LibraryVersions().c_str());
}

// ---- Arguments --------------------------------------------------------------------------------

// The format options, kept as given until the format they apply to is known.
struct FormatOpts {
	std::optional<std::string> method, level, block, splits, precheck;
	bool forceAll = false;
	bool Any() const { return method || level || block || splits || precheck || forceAll; }
};

struct Args {
	std::string command;
	std::vector<std::string> inputs;
	Settings s;
	Format target = Format::Unknown;
	FormatOpts fmt;
	// Older option names that named their format (--hunk, --rvz-chunk, ...): applied to that format.
	FormatOpts legacyChd, legacyRvz, legacyPbp;
	std::string saveConfig, against;
	// zar
	std::string zarName;
	bool noRoot = false;
	// bench
	std::vector<std::string> cases;
	int reads = 4000;
	bool sourceOnDisk = false;
	std::string work;
};

bool ParseU32Size(const std::string &v, uint32_t *out) {
	uint64_t x;
	if (!ParseSize(v, &x) || x > 0xFFFFFFFFull)
		return false;
	*out = (uint32_t)x;
	return true;
}

bool ParseInt(const std::string &v, int *out) {
	char *end = nullptr;
	long x = strtol(v.c_str(), &end, 10);
	if (v.empty() || !end || *end)
		return false;
	*out = (int)x;
	return true;
}

bool ParseDouble(const std::string &v, double *out) {
	char *end = nullptr;
	double x = strtod(v.c_str(), &end);
	if (v.empty() || !end || (*end && strcmp(end, "%") != 0))
		return false;
	*out = x;
	return true;
}

bool IsCompressedFormat(Format f) {
	return f != Format::Unknown && f != Format::ISO && f != Format::ZAR && !IsCdImageFormat(f) && f != Format::Count;
}

// Applies the format options to f's settings in s. Options that don't fit f are errors, so a typo
// or a misplaced option doesn't go unnoticed.
bool ApplyFormatOpts(Settings &s, Format f, const FormatOpts &o, const std::string &where, std::string *err) {
	auto notFor = [&](const char *opt, const std::string &hint = "") {
		*err = Fmt("%s doesn't apply to %s%s", opt, FormatName(f), where.c_str()) + (hint.empty() ? "" : ": " + hint);
		return false;
	};
	if (!o.Any())
		return true;
	if (f == Format::ISO || f == Format::ZAR)
		return notFor(o.method ? "--method" : o.level ? "--level" : o.block ? "--block" : "a format option");

	if (IsMcsoFormat(f)) {
		CsoSettings &c = s.Cso(f);
		const bool zso = f == Format::ZSO, v2 = f == Format::CSO2, dax = f == Format::DAX;
		if (o.level)
			return notFor("--level", "CSO-family formats choose methods with --method");
		if (o.method) {
			const std::string m = ToLower(*o.method);
			if (m == "default") {
				c = CsoSettings();
			} else {
				c.useDefault = false;
				c.preset = CsoPreset::Custom;
				c.zlib = c.zlibBrute = c.sevenZip = c.zopfli = c.libdeflate = c.lz4 = c.lz4hc = c.lz4Brute = false;
				std::string list = m;
				std::replace(list.begin(), list.end(), '+', ',');
				for (const std::string &name : Split(list, ',')) {
					const bool deflate = name == "zlib" || name == "zlib-brute" || name == "7zip" || name == "7z" || name == "zopfli" ||
						name == "libdeflate";
					const bool lz4 = name == "lz4" || name == "lz4hc" || name == "lz4hc-brute";
					if (!deflate && !lz4) {
						*err = "unknown method " + name + " (libdeflate, zlib, zlib-brute, 7zip, zopfli, lz4, lz4hc, lz4hc-brute)";
						return false;
					}
					if ((deflate && zso) || (lz4 && !zso && !v2)) {
						*err = Fmt("%s can't use %s: %s", FormatName(f), name.c_str(),
							zso ? "ZSO uses lz4, lz4hc, lz4hc-brute" : "CSO and DAX use libdeflate, zlib, zlib-brute, 7zip, zopfli (lz4 needs cso2)");
						return false;
					}
					if (name == "zlib") c.zlib = true;
					else if (name == "zlib-brute") c.zlibBrute = true;
					else if (name == "7zip" || name == "7z") c.sevenZip = true;
					else if (name == "zopfli") c.zopfli = true;
					else if (name == "libdeflate") c.libdeflate = true;
					else if (name == "lz4") c.lz4 = true;
					else if (name == "lz4hc") c.lz4hc = true;
					else c.lz4Brute = true;
				}
			}
		}
		if (o.block) {
			if (dax)
				return notFor("--block", "DAX always uses 8K frames");
			const std::string b = ToLower(*o.block);
			uint32_t v = 0;
			if (b != "auto" && (!ParseU32Size(b, &v) || v < 2048 || v > 262144 || (v & (v - 1)))) {
				*err = "--block for " + std::string(FormatName(f)) + " is a power of two from 2K to 256K, or auto";
				return false;
			}
			c.blockSize = v;
		}
		if (o.splits) {
			int n;
			if (zso)
				return notFor("--zopfli-splits");
			if (!ParseInt(*o.splits, &n) || n < 0 || n > 1000) {
				*err = "--zopfli-splits takes a number (0 = Zopfli's default, 15)";
				return false;
			}
			c.zopfliSplits = (uint32_t)n;
		}
		if (o.precheck) {
			double p;
			if (zso)
				return notFor("--precheck");
			if (!ParseDouble(*o.precheck, &p) || p < 0 || p > 100) {
				*err = "--precheck takes a percentage from 0 to 100";
				return false;
			}
			c.precheck = p;
		}
		if (o.forceAll)
			c.forceAll = true;
		// Tuning on top of the default keeps the default's methods (libdeflate, or lz4 + lz4 HC for
		// ZSO) and its 64K blocks, made explicit.
		if (c.useDefault && (o.block || o.splits || o.precheck || o.forceAll)) {
			const uint32_t block = c.blockSize;
			const bool force = c.forceAll;
			const uint32_t splits = c.zopfliSplits;
			const double pre = c.precheck;
			c.useDefault = false;
			c.preset = CsoPreset::Custom;
			c.zlib = c.zlibBrute = c.sevenZip = c.zopfli = c.lz4Brute = false;
			c.libdeflate = !zso;
			c.lz4 = c.lz4hc = zso;
			c.blockSize = o.block ? block : CsoSettings::DEFAULT_BLOCK;
			c.forceAll = force;
			c.zopfliSplits = splits;
			c.precheck = pre;
		}
		return true;
	}

	if (o.splits)
		return notFor("--zopfli-splits");
	if (o.precheck)
		return notFor("--precheck", "for archives: --zar-precheck");
	if (o.forceAll)
		return notFor("--force-all");

	if (f == Format::CHD) {
		if (o.level)
			return notFor("--level", "CHD chooses codecs with --method");
		if (o.method && !s.chd.SetCodecs(*o.method)) {
			*err = "--method for CHD: default, or up to 4 of lzma, zlib, zstd, huff, flac (e.g. lzma,zstd)";
			return false;
		}
		if (o.block) {
			uint32_t v;
			if (!ParseU32Size(*o.block, &v) || v == 0 || v % 2048 || v > (1u << 20)) {
				*err = "--block for CHD is the hunk size: a multiple of 2048 up to 1M (default 2K)";
				return false;
			}
			s.chd.hunkBytes = v;
		}
		return true;
	}

	if (f == Format::PBP) {
		if (o.level)
			return notFor("--level", "PBP: --method lzrc, max or none");
		if (o.block)
			return notFor("--block");
		if (o.method) {
			const std::string m = ToLower(*o.method);
			const int c = m == "none" ? 0 : (m == "lzrc" || m == "default") ? 1 : m == "max" ? 2 : -1;
			if (c < 0) {
				*err = "--method for PBP is lzrc (default), max or none";
				return false;
			}
			s.pbp.compress = c;
		}
		return true;
	}

	if (IsIrvzFormat(f)) {
		RvzSettings &r = s.Rvz(f);
		const bool wia = f == Format::WIA;
		if (o.method && ToLower(*o.method) == "default" && !o.level && !o.block) {
			r = RvzSettings();
			return true;
		}
		r.useDefault = false;
		if (o.method) {
			const std::string m = ToLower(*o.method);
			int found = -1;
			for (int k = RVZ_NONE; k <= RVZ_ZSTD; ++k)
				if (m == RvzMethodName(k) || (m == "default" && k == RVZ_LZMA2))
					found = k;
			if (found < 0 || !RvzMethodAllowed(f, found)) {
				*err = wia ? "--method for WIA is lzma2 (default), lzma, bzip2, purge or none"
						   : "--method for RVZ is lzma2 (default), zstd, lzma, bzip2 or none";
				return false;
			}
			r.method = found;
			int lo, hi;
			RvzMethodLevels(found, &lo, &hi);
			if (lo <= hi)
				r.level = std::clamp(r.level, lo, hi);
		}
		if (o.level) {
			int lo, hi, v;
			RvzMethodLevels(r.method, &lo, &hi);
			if (lo > hi)
				return notFor("--level", Fmt("the %s method has no levels", RvzMethodName(r.method)));
			if (!ParseInt(*o.level, &v) || v < lo || v > hi) {
				*err = Fmt("--level for %s is %d..%d", RvzMethodName(r.method), lo, hi);
				return false;
			}
			r.level = v;
		}
		if (o.block) {
			uint32_t v;
			const bool okSize = ParseU32Size(*o.block, &v) &&
				(wia ? (v >= (2u << 20) && v % (2u << 20) == 0)
					 : ((v >= (32u << 10) && v < (2u << 20) && !(v & (v - 1))) || (v >= (2u << 20) && v % (2u << 20) == 0)));
			if (!okSize || v > (32u << 20)) {
				*err = wia ? "--block for WIA is the chunk size: a multiple of 2M (default 2M)"
						   : "--block for RVZ is the chunk size: a power of two from 32K, or a multiple of 2M (default 128K)";
				return false;
			}
			r.chunk = v;
		}
		return true;
	}
	return true;
}

// Commands that are formats: what they make.
Format CommandFormat(const std::string &cmd) {
	if (cmd == "convert" || cmd == "compress" || cmd == "verify" || cmd == "info" || cmd == "bench" || cmd == "formats" || cmd == "zar")
		return Format::Unknown;
	if (cmd == "extract")
		return Format::ISO;
	return FormatFromName(cmd);
}

bool ParseArgs(int argc, char **argv, Args &a, std::string &err) {
	if (argc < 2)
		return false;
	a.command = ToLower(argv[1]);
	if (a.command == "-h" || a.command == "--help" || a.command == "help") {
		a.command = "help";
		return true;
	}
	if (a.command == "zarchive" || a.command == "pack")
		a.command = "zar";
	if (a.command == "compress")
		a.command = "convert";
	// Settings files first, so every other option overrides them.
	for (int i = 2; i < argc; ++i) {
		if (strcmp(argv[i], "--gui-settings") == 0 && !a.s.Load(Settings::DefaultPath())) {
			err = "no GUI settings at " + Settings::DefaultPath();
			return false;
		}
		std::string file;
		if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
			file = argv[i + 1];
		else if (strncmp(argv[i], "--config=", 9) == 0)
			file = argv[i] + 9;
		if (!file.empty() && !a.s.Load(file)) {
			err = "can't read " + file;
			return false;
		}
	}
	const Format cmdFormat = CommandFormat(a.command);
	if (cmdFormat != Format::Unknown) {
		if (!FormatAvailable(cmdFormat)) {
			err = std::string(FormatName(cmdFormat)) + " isn't supported by this build";
			return false;
		}
		a.target = cmdFormat;
		if (a.command != "iso" && a.command != "extract")
			a.command = "convert";
		else
			a.command = "extract";
	} else if (a.command == "convert") {
		a.target = Format::Unknown;  // --to, or the default below
	} else if (a.command != "verify" && a.command != "info" && a.command != "bench" && a.command != "formats" && a.command != "zar") {
		err = "unknown command " + a.command;
		return false;
	}

	for (int i = 2; i < argc; ++i) {
		std::string o = argv[i];
		// --opt=value works as well as --opt value.
		std::optional<std::string> inlineValue;
		if (o.size() > 2 && o[0] == '-' && o[1] == '-') {
			size_t eq = o.find('=');
			if (eq != std::string::npos) {
				inlineValue = o.substr(eq + 1);
				o = o.substr(0, eq);
			}
		}
		auto val = [&](std::string &out) {
			if (inlineValue) {
				out = *inlineValue;
				return true;
			}
			if (i + 1 >= argc) {
				err = o + " needs a value";
				return false;
			}
			out = argv[++i];
			return true;
		};
		auto opt = [&](std::optional<std::string> &out) {
			std::string v;
			if (!val(v))
				return false;
			out = v;
			return true;
		};
		std::string v;
		// ---- format options
		if (o == "--method" || o == "--methods") {
			if (!opt(a.fmt.method))
				return false;
		} else if (o == "--level") {
			if (!opt(a.fmt.level))
				return false;
		} else if (o == "--block") {
			if (!opt(a.fmt.block))
				return false;
		} else if (o == "--zopfli-splits" || o == "--splits") {
			if (!opt(a.fmt.splits))
				return false;
		} else if (o == "--precheck") {
			if (!opt(a.fmt.precheck))
				return false;
		} else if (o == "--force-all") {
			a.fmt.forceAll = true;
		} else if (o == "--content-id") {
			if (!val(a.s.pbp.contentId))
				return false;
		} else if (o == "--boot-png") {
			if (!val(a.s.pbp.startDatPng))
				return false;
		} else if (o == "--single-bin") {
			a.s.cue.splitBins = false;
		} else if (o == "--split-bins") {
			a.s.cue.splitBins = true;
		} else if (o == "--pbp-file") {
			a.s.pbp.layout = PbpLayout::File;
		} else if (o == "--pbp-folder") {
			a.s.pbp.layout = PbpLayout::Folder;
		} else if (o == "--no-trim") {
			a.s.pbp.trimOnExtract = false;
		} else if (o == "--no-mac-check") {
			a.s.pbp.verifyMacs = false;
			// ---- older names, kept so existing scripts work (docs/cli.md lists them)
		} else if (o == "--hunk") {
			if (!opt(a.legacyChd.block))
				return false;
		} else if (o == "--codecs") {
			if (!opt(a.legacyChd.method))
				return false;
		} else if (o == "--rvz-method") {
			if (!opt(a.legacyRvz.method))
				return false;
		} else if (o == "--rvz-level") {
			if (!opt(a.legacyRvz.level))
				return false;
		} else if (o == "--rvz-chunk") {
			if (!opt(a.legacyRvz.block))
				return false;
		} else if (o == "--pbp-mode") {
			if (!opt(a.legacyPbp.method))
				return false;
		} else if (o == "--preset") {
			err = "--preset was removed: the defaults are the recommended settings, and --method chooses methods (see docs/cli.md)";
			return false;
		} else if (o == "--cso-sets" || o == "--hunks" || o == "--pbp-modes" || o == "--extra" || o == "--no-cso" || o == "--no-chd" ||
			o == "--no-pbp") {
			err = o + " was removed: bench takes --case \"FMT [options]\" (see docs/cli.md)";
			return false;
			// ---- target
		} else if (o == "-f" || o == "--to" || o == "--format") {
			if (!val(v))
				return false;
			Format f = FormatFromName(v);
			if (f == Format::Unknown || f == Format::ZAR || !FormatAvailable(f)) {
				err = f == Format::ZAR ? "archives are made with the zar command" : "unknown or unsupported format: " + v;
				return false;
			}
			if (a.command == "extract" && f != Format::ISO) {
				err = "extract always writes ISO; use the format's command (e.g. 'chd') to convert";
				return false;
			}
			if (a.command != "convert" && a.command != "extract") {
				err = "--to belongs to convert (or use the format as the command, e.g. 'chd')";
				return false;
			}
			if (a.target != Format::Unknown && a.target != f && CommandFormat(ToLower(argv[1])) != Format::Unknown) {
				err = std::string("the command already says ") + FormatName(a.target);
				return false;
			}
			a.target = f;
			// ---- output
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
			// ---- engine
		} else if (o == "--no-verify") {
			a.s.engine.verify = false;
		} else if (o == "--verify") {
			a.s.engine.verify = true;
		} else if (o == "--no-ram") {
			a.s.engine.repackInRam = false;
		} else if (o == "--ram") {
			a.s.engine.repackInRam = true;
		} else if (o == "--ram-budget") {
			if (!val(v) || (ToLower(v) != "auto" && !ParseSize(v, &a.s.engine.ramBudget))) {
				err = "--ram-budget takes a size (e.g. 4G) or auto";
				return false;
			}
			if (ToLower(v) == "auto")
				a.s.engine.ramBudget = 0;
		} else if (o == "--temp") {
			if (!val(a.s.engine.tempDir))
				return false;
		} else if (o == "-j" || o == "--jobs") {
			int n;
			if (!val(v) || !ParseInt(v, &n) || n < 1 || n > 64) {
				err = "--jobs takes 1..64";
				return false;
			}
			a.s.engine.jobs = (unsigned)n;
		} else if (o == "-t" || o == "--threads") {
			int n;
			if (!val(v) || !ParseInt(v, &n) || n < 0 || n > 1024) {
				err = "--threads takes a number (0 = all cores)";
				return false;
			}
			a.s.engine.threads = (unsigned)n;
		} else if (o == "--normal-priority") {
			a.s.engine.lowPriority = false;
		} else if (o == "--low-priority") {
			a.s.engine.lowPriority = true;
		} else if (o == "--config") {
			if (!inlineValue)
				++i;  // loaded above
		} else if (o == "--gui-settings") {
			// loaded above
		} else if (o == "--save-config") {
			if (!val(a.saveConfig))
				return false;
		} else if (o == "-q" || o == "--quiet") {
			g_quiet = true;
			// ---- zar
		} else if (o == "--name") {
			if (!val(a.zarName))
				return false;
		} else if (o == "--zar-level") {
			int n;
			if (!val(v) || !ParseInt(v, &n) || n < 1 || n > 22) {
				err = "--zar-level takes 1..22";
				return false;
			}
			a.s.zar.level = n;
		} else if (o == "--zar-precheck") {
			double p;
			if (!val(v) || !ParseDouble(v, &p) || p < 0 || p > 100) {
				err = "--zar-precheck takes a percentage from 0 to 100 (0 = off)";
				return false;
			}
			a.s.zar.precheck = p;
		} else if (o == "--store-discs") {
			a.s.zar.storeDiscs = true;
		} else if (o == "--discs" || o == "--compress-discs") {
			if (!val(v))
				return false;
			Format f = FormatFromName(v);
			if (!IsCompressedFormat(f) || !FormatAvailable(f)) {
				err = o + " takes a compressed disc format: cso, zso, cso2, dax, chd, pbp, rvz, wia";
				return false;
			}
			a.s.zar.compressDiscs = true;
			a.s.zar.discFormat = f;
		} else if (o == "--no-root") {
			a.noRoot = true;
			// ---- verify
		} else if (o == "--against") {
			if (!val(a.against))
				return false;
			// ---- bench
		} else if (o == "--case") {
			if (!val(v))
				return false;
			a.cases.push_back(v);
		} else if (o == "--reads") {
			int n;
			if (!val(v) || !ParseInt(v, &n) || n < 0) {
				err = "--reads takes a number";
				return false;
			}
			a.reads = n;
		} else if (o == "--source-on-disk") {
			a.sourceOnDisk = true;
		} else if (o == "--work") {
			if (!val(a.work))
				return false;
		} else if (!o.empty() && o[0] == '-' && o.size() > 1) {
			err = "unknown option " + o;
			return false;
		} else {
			a.inputs.push_back(argv[i]);
		}
	}

	if (a.command == "convert" && a.target == Format::Unknown)
		a.target = Settings().target;
	if (a.command == "convert" && a.target == Format::ISO)
		a.command = "extract";

	// The format options go to the format being made.
	if (a.command == "convert") {
		a.s.target = a.target;
		if (!ApplyFormatOpts(a.s, a.target, a.fmt, "", &err))
			return false;
	} else if (a.command == "zar") {
		if (a.fmt.Any() && !a.s.zar.compressDiscs) {
			err = "in zar, --method / --level / --block tune the discs' format: give it with --discs FMT";
			return false;
		}
		if (!ApplyFormatOpts(a.s, a.s.zar.discFormat, a.fmt, " (the --discs format)", &err))
			return false;
	} else if (a.fmt.Any()) {
		err = a.command == "bench" ? "bench takes format options inside --case, e.g. --case \"chd --block 16K\""
								   : "format options (--method, --level, --block...) are for making a format";
		return false;
	}
	if (!ApplyFormatOpts(a.s, Format::CHD, a.legacyChd, " (--hunk / --codecs)", &err) ||
		!ApplyFormatOpts(a.s, Format::PBP, a.legacyPbp, " (--pbp-mode)", &err))
		return false;
	if (a.legacyRvz.Any() &&
		(!ApplyFormatOpts(a.s, Format::RVZ, a.legacyRvz, " (--rvz-*)", &err) || !ApplyFormatOpts(a.s, Format::WIA, a.legacyRvz, " (--rvz-*)", &err)))
		return false;
	if (a.command != "zar" && (!a.zarName.empty() || a.noRoot)) {
		err = "--name and --no-root are for zar";
		return false;
	}
	if (a.command != "bench" && !a.cases.empty()) {
		err = "--case is for bench";
		return false;
	}
	return true;
}

// What a format makes with s's settings, for the banner and 'formats'.
std::string SettingsFor(Format f, const Settings &s) {
	if (IsMcsoFormat(f))
		return s.Cso(f).Describe(f);
	if (IsIrvzFormat(f))
		return s.Rvz(f).Describe(f);
	switch (f) {
	case Format::CHD: return s.chd.Describe();
	case Format::PBP: return s.pbp.Describe() + (s.pbp.layout == PbpLayout::Folder ? ", Game/EBOOT.PBP" : ", Game.pbp");
	case Format::ZAR: return s.zar.Describe();
	case Format::CUE: return s.cue.splitBins ? "one .bin per track" : "one .bin";
	default: return "";
	}
}

// ---- Running a queue --------------------------------------------------------------------------

// Runs everything queued, with a progress line on a terminal. Returns the exit code.
int RunQueue(JobQueue &q, size_t total) {
	q.Start();
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
			auto c = q.Count();
			std::string line = Fmt("[%d/%zu] ", c.done + c.skipped + c.failed + c.cancelled, total);
			for (auto &j : q.Jobs()) {
				if (j->State() != JobState::Running)
					continue;
				const std::string name = j->zar ? j->zar->name : GameName(j->input.path);
				line += Fmt("%s: %s %.0f%%  ", name.c_str(), PhaseName(j->CurrentPhase()), j->overall.load() * 100);
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
				fprintf(stderr, "  FAILED %s: %s\n", j->zar ? j->zar->name.c_str() : j->input.path.c_str(), j->Result().message.c_str());
	}
	return c.failed ? 2 : (c.cancelled ? 130 : 0);
}

int RunConvert(Args &a) {
	std::vector<std::string> warnings;
	std::vector<InputFile> files = ScanInputs(a.inputs, a.s.recursive, &warnings, &g_interrupted);
	for (auto &w : warnings)
		fprintf(stderr, "warning: %s\n", w.c_str());
	if (files.empty()) {
		fprintf(stderr, "No disc images found.\n");
		return 1;
	}
	const Format t = a.command == "extract" ? Format::ISO : a.target;
	if (!g_quiet) {
		std::string what = t == Format::ISO ? std::string("ISO (archives to folders, CD images to cue/bin)") : Fmt("%s, %s", FormatName(t), SettingsFor(t, a.s).c_str());
		PrintLine(Fmt("%zu image(s) -> %s; %u at once, verify %s, RAM budget %s", files.size(), what.c_str(), a.s.engine.jobs,
			a.s.engine.verify ? "on" : "off", HumanBytes(a.s.engine.ramBudget ? a.s.engine.ramBudget : DefaultRamBudget()).c_str()));
	}
	JobQueue q;
	q.SetSettings(a.s);
	for (const InputFile &f : files)
		q.Add(f, t == Format::ISO && IsCdSource(f.path, f.format) ? Format::CUE : t);  // CD images extract to cue/bin
	return RunQueue(q, files.size());
}

int RunZar(Args &a) {
	ZarSpec spec;
	std::error_code ec;
	// The first path, when it's a folder, is the archive's root (its contents, like the zarchive
	// tool and the GUI); everything else goes in under its own name.
	for (std::string &in : a.inputs) {
		fs::path p = fs::absolute(U8Path(in), ec).lexically_normal();
		if (!p.has_filename())
			p = p.parent_path();  // "Trails/" -> "Trails"
		in = PathU8(p);
	}
	size_t first = 0;
	if (!a.noRoot && fs::is_directory(U8Path(a.inputs[0]), ec)) {
		spec.rootDir = a.inputs[0];
		first = 1;
	}
	spec.extras.assign(a.inputs.begin() + first, a.inputs.end());
	spec.name = a.zarName;
	if (spec.name.empty()) {
		fs::path p = U8Path(a.inputs[0]);
		if (!p.has_filename())
			p = p.parent_path();
		spec.name = fs::is_directory(p, ec) ? PathU8(p.filename()) : PathU8(p.stem());
	}
	std::string err;
	if (!MeasureZarSpec(&spec, &err)) {
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	if (!g_quiet) {
		std::string discs = a.s.zar.compressDiscs ? Fmt("; discs to %s, %s", FormatName(a.s.zar.discFormat), SettingsFor(a.s.zar.discFormat, a.s).c_str()) : "";
		PrintLine(Fmt("ZArchive %s: %s from %zu path(s); %s%s; verify %s -> %s", spec.name.c_str(), HumanBytes(spec.totalSize).c_str(),
			spec.Paths().size(), a.s.zar.Describe().c_str(), discs.c_str(), a.s.engine.verify ? "on" : "off",
			PlanZarOutputPath(spec, a.s).c_str()));
	}
	JobQueue q;
	q.SetSettings(a.s);
	auto job = q.AddZArchive(spec);
	int rc = RunQueue(q, 1);
	JobResult r = job->Result();
	if (r.state == JobState::Done)
		PrintLine(Fmt("%s: %s -> %s (%.1f%%), verify %s. %s", r.outputPath.c_str(), HumanBytes(r.inputSize).c_str(), HumanBytes(r.outputSize).c_str(),
			r.inputSize ? 100.0 * r.outputSize / r.inputSize : 0.0, r.verify.c_str(), r.message.c_str()));
	else if (r.state == JobState::Skipped)
		PrintLine("Skipped: " + r.message);
	return rc;
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
		if (f.format == Format::ZAR) {
			ok = TestZArchive(f.path, ctx, &verdict);
		} else if (IsCdSource(f.path, f.format)) {
			auto chdProgress = [](void *u, double p) -> int {
				OpContext *c = (OpContext *)u;
				if (c->progress)
					c->progress(p);
				return c->Cancelled() ? 1 : 0;
			};
			if (!a.against.empty()) {
				// A CD image against a CD CHD, either way round.
				const bool chd = f.format == Format::CHD;
				int rc = chdm_verify_cd(chd ? a.against.c_str() : f.path.c_str(), chd ? f.path.c_str() : a.against.c_str(), chdProgress, &ctx);
				ok = rc == CHDM_OK;
				verdict = ok ? "matches " + a.against : std::string(chdm_last_error_message());
			} else if (f.format == Format::CHD) {
				// Every hunk decodes and passes its CRC.
				chdm_file *file = nullptr;
				ok = chdm_open(f.path.c_str(), CHDM_OPEN_READ, nullptr, &file) == CHDM_OK;
				if (ok) {
					chdm_info ci{};
					chdm_get_info(file, &ci);
					std::vector<uint8_t> hunk(ci.hunk_bytes);
					for (uint32_t h = 0; ok && h < ci.hunk_count && !g_interrupted; ++h) {
						ok = chdm_read_hunk(file, h, hunk.data()) == CHDM_OK;
						if (h % 64 == 0)
							ctx.progress((double)h / ci.hunk_count);
					}
					chdm_close(file);
				}
				verdict = ok ? "decodes cleanly" : std::string(chdm_last_error_message());
			} else {
				chdm_cd_image_info ci{};
				ok = chdm_cd_inspect(f.path.c_str(), &ci) == CHDM_OK;
				verdict = ok ? Fmt("readable, %u tracks (--against GAME.chd compares it)", ci.tracks) : std::string(chdm_last_error_message());
			}
		} else if (!a.against.empty()) {
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
	return g_interrupted ? 130 : bad ? 2 : 0;
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
			printf("  %s %s (%llu bytes), %.2f%% of it\n", i.format == Format::ZAR ? "contents" : "image   ", HumanBytes(i.isoSize).c_str(),
				(unsigned long long)i.isoSize, 100.0 * i.fileSize / i.isoSize);
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

// "chd --block 16K --method zstd" -> a benchmark case, on top of base.
bool ParseCase(const std::string &text, const Settings &base, BenchCase *out, std::string *err) {
	std::vector<std::string> tok;
	for (const std::string &t : Split(text, ' '))
		if (!Trim(t).empty())
			tok.push_back(Trim(t));
	if (tok.empty()) {
		*err = "empty --case";
		return false;
	}
	const Format f = FormatFromName(tok[0]);
	if (!IsCompressedFormat(f) || !FormatAvailable(f)) {
		*err = "--case \"" + text + "\": starts with a compressed format (cso, zso, cso2, dax, chd, pbp, rvz, wia)";
		return false;
	}
	FormatOpts o;
	for (size_t i = 1; i < tok.size(); ++i) {
		std::string name = tok[i], value;
		size_t eq = name.find('=');
		if (eq != std::string::npos) {
			value = name.substr(eq + 1);
			name = name.substr(0, eq);
		}
		auto need = [&](std::optional<std::string> &dst) {
			if (eq == std::string::npos) {
				if (i + 1 >= tok.size()) {
					*err = "--case \"" + text + "\": " + name + " needs a value";
					return false;
				}
				value = tok[++i];
			}
			dst = value;
			return true;
		};
		bool ok = true;
		if (name == "--method" || name == "--methods") ok = need(o.method);
		else if (name == "--level") ok = need(o.level);
		else if (name == "--block") ok = need(o.block);
		else if (name == "--zopfli-splits" || name == "--splits") ok = need(o.splits);
		else if (name == "--precheck") ok = need(o.precheck);
		else if (name == "--force-all") o.forceAll = true;
		else {
			*err = "--case \"" + text + "\": " + name + " isn't a format option (--method, --level, --block, --zopfli-splits, --precheck, --force-all)";
			return false;
		}
		if (!ok)
			return false;
	}
	Settings s = base;
	if (!ApplyFormatOpts(s, f, o, " in --case \"" + text + "\"", err))
		return false;
	if (IsMcsoFormat(f))
		*out = MakeMcsoCase(f, s.Cso(f));
	else if (IsIrvzFormat(f))
		*out = MakeRvzCase(f, s.Rvz(f));
	else if (f == Format::CHD)
		*out = MakeChdCase(s.chd);
	else
		*out = MakePbpCase(s.pbp);
	return true;
}

int RunBench(Args &a) {
	BenchConfig cfg;
	cfg.verify = a.s.engine.verify;
	cfg.reads = a.reads;
	cfg.sourceInRam = !a.sourceOnDisk;
	cfg.workDir = a.work;
	cfg.threads = a.s.engine.threads;
	if (!a.cases.empty()) {
		for (const std::string &text : a.cases) {
			BenchCase c;
			std::string err;
			if (!ParseCase(text, a.s, &c, &err)) {
				fprintf(stderr, "error: %s\n", err.c_str());
				return 1;
			}
			cfg.cases.push_back(c);
		}
	} else {
		// The benchmark list: the GUI's with --gui-settings / --config, otherwise the default one.
		for (const std::string &line : a.s.benchCases) {
			BenchCase c;
			if (ParseBenchCase(line, &c) && FormatAvailable(c.format))
				cfg.cases.push_back(c);
		}
	}
	if (cfg.cases.empty()) {
		fprintf(stderr, "Nothing to benchmark: add --case \"FMT [options]\".\n");
		return 1;
	}
	std::vector<std::string> warnings;
	std::vector<InputFile> files = ScanInputs(a.inputs, a.s.recursive, &warnings);
	for (auto &w : warnings)
		fprintf(stderr, "warning: %s\n", w.c_str());
	files.erase(std::remove_if(files.begin(), files.end(), [](const InputFile &f) { return f.format == Format::ZAR; }), files.end());
	if (files.empty()) {
		fprintf(stderr, "No disc images found.\n");
		return 1;
	}
	std::sort(files.begin(), files.end(), [](const InputFile &x, const InputFile &y) { return x.size < y.size; });
	if (!g_quiet) {
		PrintLine(Fmt("Benchmark: %zu image(s) x %zu setting(s), %d random reads, verify %s, results in %s", files.size(), cfg.cases.size(),
			cfg.reads, cfg.verify ? "on" : "off", (cfg.workDir.empty() ? DefaultBenchDir() : cfg.workDir).c_str()));
		for (const BenchCase &c : cfg.cases)
			PrintLine(Fmt("  %-6s %s", FormatName(c.format), c.label.c_str()));
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
		failed += (r.failed && r.error.rfind("skipped", 0) != 0) ||
			(!r.failed && r.verify != "OK" && r.verify != "not compared" && r.verify.rfind("OK", 0) != 0);
	return g_interrupted ? 130 : failed ? 2 : 0;
}

int RunFormats(const Args &a) {
	printf("  %-8s %-6s %-46s %s\n", "command", "ext", "settings without options", "");
	for (int i = 1; i < (int)Format::Count; ++i) {
		Format f = (Format)i;
		if (f == Format::CDIMG)  // read only
			continue;
		std::string key = FormatKey(f);
		std::string settings = f == Format::ISO ? "extract (a .zar to a folder)" : SettingsFor(f, a.s);
		printf("  %-8s %-6s %-46s %s%s\n", key.c_str(), FormatExtension(f), settings.c_str(), FormatDescription(f),
			FormatAvailable(f) ? "" : " (not in this build)");
	}
	return 0;
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
		if (err.empty())
			Usage();
		else
			fprintf(stderr, "error: %s\n(isopackager-cli --help lists everything)\n", err.c_str());
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
	if (a.command == "formats")
		return RunFormats(a);
	if (a.inputs.empty()) {
		fprintf(stderr, "error: no input files or folders\n(isopackager-cli --help lists everything)\n");
		return 1;
	}
	if (a.command == "convert" || a.command == "extract")
		return RunConvert(a);
	if (a.command == "zar")
		return RunZar(a);
	if (a.command == "verify")
		return RunVerify(a);
	if (a.command == "info")
		return RunInfo(a);
	if (a.command == "bench")
		return RunBench(a);
	fprintf(stderr, "error: unknown command %s\n", a.command.c_str());
	return 1;
}
