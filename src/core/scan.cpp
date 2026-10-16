#include "core/scan.h"

#include <algorithm>
#include <set>

#include "core/util.h"

namespace isopk {

std::string GameName(const std::string &path) {
	fs::path p = U8Path(path);
	std::string stem = PathU8(p.stem());
	if (ToLower(PathU8(p.filename())) == "eboot.pbp" && p.has_parent_path()) {
		std::string parent = PathU8(p.parent_path().filename());
		if (!parent.empty())
			return parent;
	}
	return stem;
}

std::vector<InputFile> ScanInputs(const std::vector<std::string> &paths, bool recursive, std::vector<std::string> *warnings,
	const std::atomic<bool> *cancel) {
	std::vector<InputFile> out;
	std::set<std::string> seen;
	auto warn = [&](const std::string &w) {
		if (warnings)
			warnings->push_back(w);
	};
	auto add = [&](const fs::path &p, const std::string &base, bool explicitPick) {
		std::error_code ec;
		fs::path abs = fs::absolute(p, ec);
		if (ec)
			abs = p;
		std::string key = PathU8(abs.lexically_normal());
#ifdef _WIN32
		key = ToLower(key);
#endif
		if (!seen.insert(key).second)
			return;
		InputFile f;
		f.path = PathU8(abs.lexically_normal());
		f.baseDir = base;
		f.size = fs::file_size(abs, ec);
		f.format = DetectFormat(f.path);
		if (f.format == Format::Unknown) {
			if (explicitPick)
				warn(f.path + ": not a recognised disc image");
			return;
		}
		if (!FormatAvailable(f.format)) {
			warn(f.path + ": " + FormatName(f.format) + " isn't supported by this build");
			return;
		}
		out.push_back(std::move(f));
	};

	for (const std::string &in : paths) {
		fs::path p = U8Path(in);
		std::error_code ec;
		if (fs::is_directory(p, ec)) {
			fs::path base = fs::absolute(p, ec).lexically_normal();
			std::vector<fs::path> found;
			auto consider = [&](const fs::directory_entry &e) {
				std::error_code ec2;
				if (!e.is_regular_file(ec2))
					return;
				std::string ext = ToLower(PathU8(e.path().extension()));
				if (IsImageExtension(ext))
					found.push_back(e.path());
			};
			if (recursive) {
				for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
					 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
					if (cancel && cancel->load())
						return out;
					consider(*it);
				}
			} else {
				for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
					 !ec && it != fs::directory_iterator(); it.increment(ec))
					consider(*it);
			}
			if (ec)
				warn(in + ": " + ec.message());
			std::sort(found.begin(), found.end());
			for (const fs::path &f : found)
				add(f, PathU8(base), false);
		} else if (fs::is_regular_file(p, ec)) {
			add(p, "", true);
		} else {
			warn(in + ": not found");
		}
	}
	return out;
}

}  // namespace isopk
