// Turning the files and folders a user picked into a list of images to work on.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/formats.h"

namespace isopk {

struct InputFile {
	std::string path;     // UTF-8
	std::string baseDir;  // the folder it was found under (for mirroring), empty when picked directly
	Format format = Format::Unknown;
	uint64_t size = 0;
};

// Files are taken as they are; folders are searched (recursively if asked) for disc images.
// Duplicates are dropped. Problems go to warnings.
std::vector<InputFile> ScanInputs(const std::vector<std::string> &paths, bool recursive, std::vector<std::string> *warnings,
	const std::atomic<bool> *cancel = nullptr);

// The game's name for output files: the file name without extension, or for .../Game/EBOOT.PBP
// the folder name.
std::string GameName(const std::string &path);

}  // namespace isopk
