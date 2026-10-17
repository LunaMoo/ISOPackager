// The benchmark: for every image and every setting, compress (timed), record the size, decode it
// again (timed, compared byte for byte with the original), and time random reads like an
// emulator does. The successor of img_bench in maxcso's bench/compression-trials branch.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/scan.h"
#include "core/settings.h"

namespace isopk {

struct BenchCase {
	Format format = Format::CSO;
	CsoSettings cso;
	ChdSettings chd;
	PbpSettings pbp;
	std::string label;  // "B zopfli5 splits20 256K", "default hunk 4096", ...
};

// img_bench's CSO sets (all at 256K): A zopfli5, B zopfli5 splits 20, C pre-check 98% + zopfli5,
// D pre-check 98% + zopfli5 splits 20, L libdeflate12 only. Plus F/S/X: the fast / standard / max
// presets at the automatic block size.
struct CsoBenchSet {
	const char *key, *name;
};
const std::vector<CsoBenchSet> &CsoBenchSets();
bool MakeCsoCase(const std::string &key, BenchCase *out);
BenchCase MakeChdCase(const std::string &codecs, uint32_t hunk);
BenchCase MakePbpCase(int compress);
BenchCase MakeMcsoCase(Format f, const CsoSettings &cso);  // any mCSO format with these settings
BenchCase MakeChdCase(const ChdSettings &chd);
BenchCase MakePbpCase(const PbpSettings &pbp);

// One line per case ("f=cso;d=0;p=3;b=262144;m=16;..."), for saving the GUI's benchmark list.
// The label is rebuilt from the settings when parsing.
std::string SerializeBenchCase(const BenchCase &c);
bool ParseBenchCase(const std::string &line, BenchCase *out);

struct BenchConfig {
	std::vector<BenchCase> cases;
	bool verify = true;
	int reads = 4000;           // random reads per image (CSO family and CHD)
	bool sourceInRam = true;    // compress from RAM (no disk reads in the timing)
	std::string workDir;        // results and temporary files; empty = AppDataDir()/bench
	unsigned threads = 0;
};

struct BenchRow {
	std::string game, format, setting;
	uint64_t input = 0, output = 0;
	double compSecs = 0, decompSecs = 0, readMs = -1;
	std::string verify = "-";
	bool failed = false;
	std::string error;

	double Ratio() const { return input ? 100.0 * (double)output / (double)input : 0; }
	double CompMBs() const { return compSecs > 0 ? input / 1e6 / compSecs : 0; }
};

struct BenchCallbacks {
	std::function<void(const BenchRow &)> row;
	// What's running: "Game: CHD default hunk 4096 - compressing", fraction of that step, of everything.
	std::function<void(const std::string &, double, double)> progress;
};

// Runs the whole matrix. Appends to <workDir>/summary.txt and <workDir>/results.csv.
// Returns the rows (failed ones included).
std::vector<BenchRow> RunBenchmark(const std::vector<InputFile> &inputs, const BenchConfig &cfg, const BenchCallbacks &cb,
	const std::atomic<bool> *cancel);

std::string BenchCsvHeader();
std::string BenchCsvLine(const BenchRow &r);
std::string BenchTableHeader();
std::string BenchTableLine(const BenchRow &r);
// Where results go when cfg.workDir is empty.
std::string DefaultBenchDir();

}  // namespace isopk
