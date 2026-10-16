// Small helpers shared by the core, the CLI and the GUI.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace isopk {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// UTF-8 std::string <-> filesystem path, on every platform (Windows paths are UTF-16 inside).
fs::path U8Path(const std::string &utf8);
std::string PathU8(const fs::path &p);

std::string ToLower(std::string s);
std::vector<std::string> Split(const std::string &s, char sep);
std::string Trim(const std::string &s);
bool ParseSize(const std::string &s, uint64_t *out);  // "4096", "16K", "2M", "1G"

// "1.23 GB", "512.0 KB", ...
std::string HumanBytes(uint64_t bytes);
// "1:02:03", "4:05", "12.3 s"
std::string HumanDuration(double seconds);
std::string Fmt(const char *fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

inline double SecondsSince(Clock::time_point t) {
	return std::chrono::duration<double>(Clock::now() - t).count();
}

// Local time "2026-10-17 01:23:45".
std::string Timestamp();

uint32_t FourCC(const char *s);  // "zstd" -> 'zstd'
std::string FourCCName(uint32_t v);

}  // namespace isopk
