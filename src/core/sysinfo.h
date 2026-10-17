// What the machine has, and the per-user folders, on every platform.
#pragma once

#include <cstdint>
#include <string>

namespace isopk {

uint64_t TotalRam();      // physical RAM in bytes, 0 if unknown
uint64_t AvailableRam();  // free RAM right now, 0 if unknown
unsigned CpuCount();
// Automatic RAM budget for in-RAM repacking: about 60% of what's free (at most 75% of the total).
uint64_t DefaultRamBudget();
// Free space on the volume holding path, or UINT64_MAX if unknown.
uint64_t FreeDiskSpace(const std::string &utf8Path);

void SetLowPriority(bool low);

// Folder for settings and benchmark results. Frontends that know better (SDL_GetPrefPath on
// mobile) call SetAppDataDir first.
// portable.ini next to the executable (Windows, Linux) switches to portable mode: an empty file
// keeps everything in the executable's folder, otherwise its first line names the folder.
std::string AppDataDir();
std::string ExecutableDir();  // empty where unknown
void SetAppDataDir(const std::string &utf8Path);

// The number of worker threads mCSO's pool (libuv) gets. Only takes effect before the first
// CSO conversion of the process.
void ConfigureCsoThreads(unsigned threads);

std::string PlatformName();  // "Windows x64", "Linux arm64", ...

}  // namespace isopk
