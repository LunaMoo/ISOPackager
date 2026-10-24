#include "core/zar.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

#include <chdmodern/chdmodern.h>
#include <zarchive/zarchivereader.h>
#include <zarchive/zarchivewriter.h>

#include "core/engine.h"
#include "core/fileio.h"
#include "core/image.h"
#include "core/settings.h"
#include "core/util.h"

namespace isopk {

// ---- Hash -------------------------------------------------------------------------------------

namespace {
inline uint64_t Rotl(uint64_t x, int r) {
	return (x << r) | (x >> (64 - r));
}
inline uint64_t Load64(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i)
		v = (v << 8) | p[i];
	return v;
}
}  // namespace

void StreamHash::Word(uint64_t w) {
	h_ ^= Rotl(w * 0xC2B2AE3D27D4EB4Full, 31) * 0x9E3779B97F4A7C15ull;
	h_ = Rotl(h_, 27) * 0x9E3779B97F4A7C15ull + 0x85EBCA77C2B2AE63ull;
}

void StreamHash::Update(const void *data, size_t len) {
	const uint8_t *p = (const uint8_t *)data;
	total_ += len;
	while (tailLen_ && len) {
		tail_[tailLen_++] = *p++;
		--len;
		if (tailLen_ == 8) {
			Word(Load64(tail_));
			tailLen_ = 0;
		}
	}
	for (; len >= 8; p += 8, len -= 8)
		Word(Load64(p));
	while (len--)
		tail_[tailLen_++] = *p++;
}

uint64_t StreamHash::Final() const {
	uint64_t h = h_;
	uint8_t last[8] = {};
	memcpy(last, tail_, tailLen_);
	h ^= Rotl(Load64(last) * 0xC2B2AE3D27D4EB4Full, 31) * 0x9E3779B97F4A7C15ull;
	h ^= total_ * 0x165667B19E3779F9ull;
	h ^= h >> 33;
	h *= 0xFF51AFD7ED558CCDull;
	h ^= h >> 33;
	h *= 0xC4CEB9FE1A85EC53ull;
	h ^= h >> 33;
	return h;
}

// ---- Spec -------------------------------------------------------------------------------------

std::string ZarSpec::SourceDir() const {
	const std::string &first = !rootDir.empty() ? rootDir : (extras.empty() ? std::string() : extras.front());
	if (first.empty())
		return {};
	std::error_code ec;
	fs::path p = fs::absolute(U8Path(first), ec).lexically_normal();
	// "D:/Games/Trails/" -> "D:/Games"
	if (!p.has_filename())
		p = p.parent_path();
	return PathU8(p.parent_path());
}

std::vector<std::string> ZarSpec::Paths() const {
	std::vector<std::string> v;
	if (!rootDir.empty())
		v.push_back(rootDir);
	v.insert(v.end(), extras.begin(), extras.end());
	return v;
}

namespace {

uint64_t TreeSize(const fs::path &p, uint64_t *files) {
	std::error_code ec;
	if (fs::is_regular_file(p, ec)) {
		++*files;
		return fs::file_size(p, ec);
	}
	uint64_t total = 0;
	if (fs::is_directory(p, ec))
		for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
			 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			std::error_code e2;
			if (it->is_regular_file(e2)) {
				++*files;
				total += it->file_size(e2);
			}
		}
	return total;
}

std::string KeyOf(const std::string &archivePath) {
	return ToLower(archivePath);  // ZArchive compares a-z case-insensitively
}

// "dir/Name.ext" not yet in used: "dir/Name (2).ext", ...
std::string Unique(const std::string &archivePath, std::set<std::string> &used) {
	if (used.insert(KeyOf(archivePath)).second)
		return archivePath;
	size_t slash = archivePath.rfind('/');
	std::string dir = slash == std::string::npos ? "" : archivePath.substr(0, slash + 1);
	std::string name = archivePath.substr(dir.size());
	size_t dot = name.rfind('.');
	std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
	std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
	for (int i = 2;; ++i) {
		std::string cand = dir + stem + Fmt(" (%d)", i) + ext;
		if (used.insert(KeyOf(cand)).second)
			return cand;
	}
}

std::string NormKey(const fs::path &p) {
	std::error_code ec;
	std::string k = PathU8(fs::absolute(p, ec).lexically_normal());
#ifdef _WIN32
	k = ToLower(k);
#endif
	return k;
}

struct Planner {
	std::vector<ZarEntry> *out = nullptr;
	std::set<std::string> used;
	std::set<std::string> excluded;

	void Add(const fs::path &src, const std::string &archivePath) {
		std::error_code ec;
		if (excluded.count(NormKey(src)))
			return;
		if (fs::is_directory(src, ec)) {
			ZarEntry d;
			d.source = PathU8(src);
			d.archivePath = Unique(archivePath, used);
			d.isDir = true;
			out->push_back(d);
			std::vector<fs::path> children;
			for (auto it = fs::directory_iterator(src, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator();
				 it.increment(ec))
				children.push_back(it->path());
			std::sort(children.begin(), children.end(), [](const fs::path &a, const fs::path &b) {
				return ToLower(PathU8(a.filename())) < ToLower(PathU8(b.filename()));
			});
			const std::string base = d.archivePath;  // out->back() moves on as children are added
			for (const fs::path &c : children)
				Add(c, base + "/" + PathU8(c.filename()));
			return;
		}
		if (!fs::is_regular_file(src, ec))
			return;
		ZarEntry f;
		f.source = PathU8(src);
		f.archivePath = Unique(archivePath, used);
		f.size = fs::file_size(src, ec);
		if (IsImageExtension(ToLower(PathU8(src.extension()))))
			f.disc = DetectFormat(f.source);
		out->push_back(f);
	}
};

}  // namespace

bool MeasureZarSpec(ZarSpec *spec, std::string *error) {
	uint64_t files = 0, total = 0;
	for (const std::string &p : spec->Paths())
		total += TreeSize(U8Path(p), &files);
	spec->totalSize = total;
	if (files == 0) {
		if (error)
			*error = "no files to put into the archive";
		return false;
	}
	return true;
}

bool PlanZarEntries(const ZarSpec &spec, const std::string &exclude, std::vector<ZarEntry> *entries, std::string *error) {
	entries->clear();
	Planner pl;
	pl.out = entries;
	if (!exclude.empty()) {
		pl.excluded.insert(NormKey(U8Path(exclude)));
		pl.excluded.insert(NormKey(U8Path(exclude + ".part")));
	}
	std::error_code ec;
	if (!spec.rootDir.empty()) {
		fs::path root = U8Path(spec.rootDir);
		if (!fs::is_directory(root, ec)) {
			*error = spec.rootDir + " is not a folder";
			return false;
		}
		std::vector<fs::path> children;
		for (auto it = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator();
			 it.increment(ec))
			children.push_back(it->path());
		std::sort(children.begin(), children.end(),
			[](const fs::path &a, const fs::path &b) { return ToLower(PathU8(a.filename())) < ToLower(PathU8(b.filename())); });
		for (const fs::path &c : children)
			pl.Add(c, PathU8(c.filename()));
	}
	for (const std::string &x : spec.extras) {
		fs::path p = U8Path(x);
		if (!p.has_filename())
			p = p.parent_path();
		if (!fs::exists(p, ec)) {
			*error = x + " doesn't exist";
			return false;
		}
		pl.Add(p, PathU8(p.filename()));
	}
	bool anyFile = std::any_of(entries->begin(), entries->end(), [](const ZarEntry &e) { return !e.isDir; });
	if (!anyFile) {
		*error = "no files to put into the archive";
		return false;
	}
	return true;
}

std::string ZarReport::Summary() const {
	std::string s = Fmt("%llu file%s", (unsigned long long)files, files == 1 ? "" : "s");
	if (discsCompressed)
		s += Fmt(", %d disc%s compressed first", discsCompressed, discsCompressed == 1 ? "" : "s");
	if (discsConverted)
		s += Fmt(", %d disc%s converted first", discsConverted, discsConverted == 1 ? "" : "s");
	if (discsKept)
		s += Fmt(", %d disc%s already in that format", discsKept, discsKept == 1 ? "" : "s");
	if (discsStored)
		s += Fmt(", %d disc%s stored", discsStored, discsStored == 1 ? "" : "s");
	if (blocks)
		s += Fmt(", %.0f%% of blocks stored as they are", 100.0 * storedBlocks / blocks);
	return s;
}

// ---- Writing ----------------------------------------------------------------------------------

namespace {

struct OutputSink {
	File file;
	uint64_t offset = 0;
	bool failed = false;
	std::string error;

	static void NewFile(int32_t, void *) {}
	static void Write(const void *data, size_t len, void *ctx) {
		OutputSink *o = (OutputSink *)ctx;
		if (o->failed)
			return;
		if (!o->file.WriteAt(o->offset, data, len)) {
			o->failed = true;
			o->error = o->file.LastError();
			return;
		}
		o->offset += len;
	}
};

// archivePath with the new format's name: "Game.iso" -> "Game.cso", or "Game/EBOOT.PBP".
std::string CompressedName(const std::string &archivePath, Format target, const Settings &s) {
	size_t slash = archivePath.rfind('/');
	std::string dir = slash == std::string::npos ? "" : archivePath.substr(0, slash + 1);
	std::string name = archivePath.substr(dir.size());
	size_t dot = name.rfind('.');
	std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
	if (target == Format::PBP && s.pbp.layout == PbpLayout::Folder)
		return dir + stem + "/EBOOT.PBP";
	return dir + stem + FormatExtension(target);
}

}  // namespace

bool WriteZArchive(const ZarSpec &spec, const std::vector<ZarEntry> &entries, const Settings &s, const std::string &outPath,
	OpContext &ctx, ZarProgress &progress, ZarReport *report) {
	const ZarSettings &z = s.zar;
	auto fail = [&](const std::string &msg) {
		if (ctx.error.empty())
			ctx.error = msg;
		return false;
	};
	auto cancelled = [&] {
		if (ctx.Cancelled()) {
			ctx.cancelled = true;
			fail("cancelled");
			return true;
		}
		return false;
	};

	OutputSink sink;
	if (!sink.file.Open(outPath, File::Write))
		return fail(outPath + ": " + sink.file.LastError());
	ZArchiveWriter w(&OutputSink::NewFile, &OutputSink::Write, &sink);
	w.SetCompressionLevel(z.level);
	w.SetPrecheck(z.precheck);
	w.SetThreads(s.engine.threads);

	// Names already used, for the compressed discs' new names.
	std::set<std::string> used;
	for (const ZarEntry &e : entries)
		used.insert(KeyOf(e.archivePath));

	uint64_t total = 0;
	for (const ZarEntry &e : entries)
		total += e.size;
	total = std::max<uint64_t>(total, 1);
	uint64_t done = 0;
	auto report01 = [&](uint64_t cur, double f) {
		if (ctx.progress)
			ctx.progress(std::min(1.0, ((double)done + (double)cur * f) / (double)total));
	};

	// 1 MB pieces: at zstd 22 a piece takes a fraction of a second, so progress keeps moving.
	std::vector<uint8_t> buf(1u << 20);
	// Appends a whole buffer in pieces (progress, cancel, write errors). f0..f1 = the part of
	// this file's weight it covers.
	auto appendMem = [&](const uint8_t *data, uint64_t size, StreamHash &h, uint64_t weight, double f0, double f1) {
		for (uint64_t pos = 0; pos < size;) {
			if (cancelled())
				return false;
			size_t n = (size_t)std::min<uint64_t>(buf.size(), size - pos);
			w.AppendData(data + pos, n);
			h.Update(data + pos, n);
			pos += n;
			if (sink.failed)
				return fail("writing the archive: " + sink.error);
			report01(weight, f0 + (f1 - f0) * (double)pos / (double)size);
		}
		return true;
	};
	auto appendFile = [&](const std::string &path, StreamHash &h, uint64_t weight, double f0, double f1, uint64_t *size) {
		File f;
		if (!f.Open(path, File::Read))
			return fail(path + ": " + f.LastError());
		const uint64_t n = f.Size();
		*size = n;
		for (uint64_t pos = 0; pos < n;) {
			if (cancelled())
				return false;
			size_t k = (size_t)std::min<uint64_t>(buf.size(), n - pos);
			if (!f.ReadExact(pos, buf.data(), k))
				return fail(path + ": " + f.LastError());
			w.AppendData(buf.data(), k);
			h.Update(buf.data(), k);
			pos += k;
			if (sink.failed)
				return fail("writing the archive: " + sink.error);
			report01(weight, f0 + (f1 - f0) * (double)pos / (double)std::max<uint64_t>(n, 1));
		}
		return true;
	};

	const Format discTarget = z.discFormat;

	// CD images (a cue sheet and its tracks, CloneCD, Alcohol, raw .bin) become one CD CHD each
	// when discs are converted, whichever format was chosen (CD images only go to CHD); their
	// track files go in with them.
	auto pathKey = [](const std::string &p) {
		std::error_code ec;
		std::string k = PathU8(fs::absolute(U8Path(p), ec).lexically_normal());
#ifdef _WIN32
		k = ToLower(k);
#endif
		return k;
	};
	std::set<std::string> cdConvert, cdParts;
	if (z.compressDiscs && discTarget != Format::ISO) {
		for (const ZarEntry &e : entries) {
			if (e.isDir || !IsCdImageFormat(e.disc) || !(IsCdDescriptor(e.source) || HasRawCdSync(e.source)))
				continue;
			ImageInfo info = InspectImage(e.source);
			if (info.platform != Platform::CD || !info.error.empty()) {
				Log(LogLevel::Warning, Fmt("%s: %s, adding it as it is", e.source.c_str(), info.error.c_str()));
				continue;
			}
			std::vector<std::string> files;
			chdm_cd_image_files(e.source.c_str(), [](void *u, const char *f) { ((std::vector<std::string> *)u)->push_back(f); }, &files);
			// A raw .bin read through a cue sheet is a track of that image, not one of its own.
			if (!IsCdDescriptor(e.source) && (files.empty() || pathKey(files.front()) != pathKey(e.source)))
				continue;
			cdConvert.insert(pathKey(e.source));
			for (const std::string &f : files)
				if (pathKey(f) != pathKey(e.source))
					cdParts.insert(pathKey(f));
		}
		for (const std::string &p : cdParts)
			cdConvert.erase(p);  // a track file of a cue sheet in the archive isn't an image of its own
	}
	uint32_t cdCodecList[4] = {};
	const uint32_t *cdCodecs = nullptr;
	{
		std::vector<uint32_t> cd = CdCodecsFor(s.chd);
		for (size_t i = 0; i < 4 && i < cd.size(); ++i)
			cdCodecList[i] = cd[i];
		if (!cd.empty())
			cdCodecs = cdCodecList;
	}

	for (const ZarEntry &e : entries) {
		if (cancelled())
			return false;
		if (e.isDir) {
			if (!w.MakeDir(e.archivePath.c_str(), true))
				return fail("can't create folder " + e.archivePath + " in the archive");
			continue;
		}
		ZarWritten written;
		StreamHash h;
		bool handled = false;

		if (!e.isDir && cdParts.count(pathKey(e.source))) {
			done += e.size;  // in the CD CHD of the image it belongs to
			report01(0, 0);
			continue;
		}
		if (!e.isDir && cdConvert.count(pathKey(e.source))) {
			const std::string name = Unique(CompressedName(e.archivePath, Format::CHD, s), used);
			if (progress.onPhase)
				progress.onPhase(true, name);
			Clock::time_point t0 = Clock::now();
			fs::path tdir = s.engine.tempDir.empty() ? U8Path(outPath).parent_path() : U8Path(s.engine.tempDir);
			std::error_code ec;
			fs::create_directories(tdir, ec);
			const std::string temp = PathU8(tdir / U8Path(GameName(e.source) + ".isopk-temp.chd"));
			struct TempCleanup {
				std::string path;
				~TempCleanup() {
					std::error_code e2;
					fs::remove(U8Path(path), e2);
				}
			} cleanup{ temp };
			struct Cb {
				OpContext *ctx;
				std::function<void(double)> f;
			} cb{ &ctx, [&](double f) { report01(e.size, 0.9 * f); } };
			auto chdProgress = [](void *u, double f) -> int {
				Cb *c = (Cb *)u;
				c->f(f);
				return c->ctx->Cancelled() ? 1 : 0;
			};
			int rc = chdm_create_cd(e.source.c_str(), temp.c_str(), 0, cdCodecs, chdProgress, &cb);
			if (rc == CHDM_OK && s.engine.verify)
				rc = chdm_verify_cd(e.source.c_str(), temp.c_str(), nullptr, nullptr);
			if (rc != CHDM_OK) {
				if (rc == CHDM_ERR_CANCELLED || ctx.Cancelled()) {
					ctx.cancelled = true;
					return fail("cancelled");
				}
				return fail(Fmt("compressing %s to a CD CHD: %s", e.source.c_str(), chdm_last_error_message()));
			}
			report->compressDiscSecs += SecondsSince(t0);
			if (progress.onPhase)
				progress.onPhase(false, name);
			if (!w.StartNewFile(name.c_str(), false))
				return fail("can't add " + name + " to the archive");
			uint64_t n = 0;
			if (!appendFile(temp, h, e.size, 0.9, 1.0, &n))
				return false;
			written.archivePath = name;
			written.size = n;
			report->discsConverted++;
			handled = true;
		}

		// Discs converted to the chosen format first: a plain ISO is compressed, any other format is
		// extracted to an ISO in RAM and that is compressed (also into RAM); the result goes into
		// the archive stored. A temporary file stands in for whichever step doesn't fit the RAM budget.
		const bool isDisc = !handled && e.disc != Format::Unknown && e.disc != Format::ZAR && !IsCdImageFormat(e.disc);
		// A DVD's image in a CD CHD isn't a DVD CHD yet.
		const bool alreadyTarget = z.compressDiscs && isDisc && e.disc == discTarget && !(e.disc == Format::CHD && CdChdHoldsDvd(e.source));
		if (z.compressDiscs && isDisc && !alreadyTarget && FormatAvailable(discTarget) && discTarget != Format::ISO && FormatAvailable(e.disc)) {
			ImageInfo info = InspectImage(e.source);
			if (std::string why = TargetRejects(discTarget, info.platform); !why.empty()) {
				Log(LogLevel::Warning, Fmt("%s: %s, adding it as it is", e.source.c_str(), why.c_str()));
			} else {
				const std::string name = Unique(CompressedName(e.archivePath, discTarget, s), used);
				if (progress.onPhase)
					progress.onPhase(true, name);
				Clock::time_point t0 = Clock::now();
				fs::path tdir = s.engine.tempDir.empty() ? U8Path(outPath).parent_path() : U8Path(s.engine.tempDir);
				std::error_code ec;
				std::vector<std::string> temps;  // removed whatever happens
				struct TempCleanup {
					std::vector<std::string> *list;
					~TempCleanup() {
						std::error_code e2;
						for (const std::string &t : *list)
							fs::remove(U8Path(t), e2);
					}
				} cleanup{ &temps };
				auto tempPath = [&](const char *ext) {
					fs::create_directories(tdir, ec);
					std::string t = PathU8(tdir / U8Path(GameName(e.source) + ".isopk-temp" + ext));
					temps.push_back(t);
					return t;
				};
				auto subFailed = [&](OpContext &sub, const char *what) {
					if (sub.Cancelled() || ctx.Cancelled()) {
						ctx.cancelled = true;
						return fail("cancelled");
					}
					return fail(Fmt("%s %s: %s", what, e.source.c_str(), sub.error.c_str()));
				};
				// Shares of this disc's progress: extract, compress, then storing in the archive.
				const bool extract = e.disc != Format::ISO;
				const double fx = extract ? 0.3 : 0.0, fc = 0.9;

				// 1. The plain image.
				FileImageReader isoFile;
				MemoryImage isoMem(ctx.cancel);
				ImageReader *iso = nullptr;
				if (!extract) {
					if (!isoFile.Open(e.source))
						return fail(isoFile.Error());
					iso = &isoFile;
				} else {
					OpContext sub;
					sub.cancel = ctx.cancel;
					sub.progress = [&](double f) { report01(e.size, fx * f); };
					if (DecompressToWriter(e.source, e.disc, s, isoMem, s.pbp.trimOnExtract, sub)) {
						iso = &isoMem;
					} else if (isoMem.OutOfMemory() && !ctx.Cancelled()) {
						Log(LogLevel::Warning, Fmt("%s: %s, extracting to a temporary ISO", e.source.c_str(), isoMem.Error().c_str()));
						isoMem.Free();
						const std::string tiso = tempPath(".iso");
						OpContext sub2;
						sub2.cancel = ctx.cancel;
						sub2.progress = sub.progress;
						if (!DecompressToFile(e.source, e.disc, s, tiso, s.pbp.trimOnExtract, sub2))
							return subFailed(sub2, "extracting");
						if (!isoFile.Open(tiso))
							return fail(isoFile.Error());
						iso = &isoFile;
						report->discsViaTemp++;
					} else {
						return subFailed(sub, "extracting");
					}
				}

				// 2. The new format, in RAM.
				OpContext sub;
				sub.cancel = ctx.cancel;
				sub.progress = [&](double f) { report01(e.size, fx + (fc - fx) * f); };
				MemoryOutput mem(ctx.cancel);
				bool inRam = CompressToMemory(*iso, discTarget, s, mem, sub);
				std::string tempOut;
				if (!inRam) {
					if (!mem.OutOfMemory() || ctx.Cancelled())
						return subFailed(sub, (std::string("compressing to ") + FormatName(discTarget)).c_str());
					mem.Free();
					Log(LogLevel::Warning, Fmt("%s: %s, compressing through a temporary file", e.source.c_str(), mem.Error().c_str()));
					tempOut = tempPath(FormatExtension(discTarget));
					OpContext sub2;
					sub2.cancel = ctx.cancel;
					sub2.progress = sub.progress;
					if (!CompressImage(*iso, discTarget, s, tempOut, sub2))
						return subFailed(sub2, (std::string("compressing to ") + FormatName(discTarget)).c_str());
					report->discsViaTemp++;
				}
				isoMem.Free();  // the plain image isn't needed any more: give its RAM back
				report->compressDiscSecs += SecondsSince(t0);

				// 3. Into the archive, stored.
				if (progress.onPhase)
					progress.onPhase(false, name);
				// Game/EBOOT.PBP needs its folder.
				if (size_t slash = name.rfind('/'); slash != std::string::npos && !w.MakeDir(name.substr(0, slash).c_str(), true))
					return fail("can't create folder " + name.substr(0, slash) + " in the archive");
				if (!w.StartNewFile(name.c_str(), false))
					return fail("can't add " + name + " to the archive");
				if (inRam) {
					if (!appendMem(mem.Data(), mem.Size(), h, e.size, fc, 1.0))
						return false;
					written.size = mem.Size();
				} else {
					uint64_t n = 0;
					if (!appendFile(tempOut, h, e.size, fc, 1.0, &n))
						return false;
					written.size = n;
				}
				written.archivePath = name;
				if (extract)
					report->discsConverted++;
				else
					report->discsCompressed++;
				handled = true;
			}
		}

		if (!handled) {
			// Already in the chosen format: it's compressed, so it's stored like the converted ones.
			const bool store = (z.storeDiscs && e.disc != Format::Unknown) || alreadyTarget;
			if (progress.onPhase)
				progress.onPhase(false, e.archivePath);
			if (!w.StartNewFile(e.archivePath.c_str(), !store))
				return fail("can't add " + e.archivePath + " to the archive");
			uint64_t n = 0;
			if (!appendFile(e.source, h, e.size, 0.0, 1.0, &n))
				return false;
			written.archivePath = e.archivePath;
			written.size = n;
			if (alreadyTarget)
				report->discsKept++;
			else if (store)
				report->discsStored++;
		}
		written.hash = h.Final();
		report->written.push_back(written);
		report->files++;
		report->inputBytes += e.size;
		done += e.size;
		report01(0, 0);
	}
	if (cancelled())
		return false;
	if (progress.onPhase)
		progress.onPhase(false, "");
	w.Finalize();
	if (sink.failed)
		return fail("writing the archive: " + sink.error);
	ZArchiveWriter::Stats st = w.GetStats();
	report->blocks = st.blocks;
	report->storedBlocks = st.storedBlocks;
	report->precheckSkipped = st.precheckSkipped;
	sink.file.Close();
	if (ctx.progress)
		ctx.progress(1.0);
	return true;
}

bool VerifyZArchive(const std::string &path, const std::vector<ZarWritten> &written, OpContext &ctx, std::string *verdict) {
	std::unique_ptr<ZArchiveReader> r(ZArchiveReader::OpenFromFile(U8Path(path)));
	if (!r) {
		*verdict = "the archive doesn't open";
		return false;
	}
	uint64_t total = 0, done = 0;
	for (auto &x : written)
		total += x.size;
	total = std::max<uint64_t>(total, 1);
	std::vector<uint8_t> buf(4u << 20);
	for (const ZarWritten &x : written) {
		ZArchiveNodeHandle h = r->LookUp(x.archivePath, true, false);
		if (h == ZARCHIVE_INVALID_NODE) {
			*verdict = x.archivePath + " missing";
			return false;
		}
		if (r->GetFileSize(h) != x.size) {
			*verdict = x.archivePath + ": wrong size";
			return false;
		}
		StreamHash hash;
		for (uint64_t pos = 0; pos < x.size;) {
			if (ctx.Cancelled()) {
				ctx.cancelled = true;
				ctx.error = "cancelled";
				return false;
			}
			uint64_t n = r->ReadFromFile(h, pos, std::min<uint64_t>(buf.size(), x.size - pos), buf.data());
			if (n == 0) {
				*verdict = x.archivePath + ": read failed at " + std::to_string(pos);
				return false;
			}
			hash.Update(buf.data(), (size_t)n);
			pos += n;
			if (ctx.progress)
				ctx.progress((double)(done + pos) / (double)total);
		}
		if (hash.Final() != x.hash) {
			*verdict = x.archivePath + " differs";
			return false;
		}
		done += x.size;
	}
	*verdict = "OK";
	return true;
}

// ---- Extracting -------------------------------------------------------------------------------

namespace {

// One path component as a file system name: never "." / "..", nothing a Windows path can't hold.
std::string SafeName(std::string_view name) {
	std::string n(name);
	if (n.empty() || n == "." || n == "..")
		return "_";
#ifdef _WIN32
	for (char &c : n)
		if ((unsigned char)c < 32 || strchr("<>:\"|?*\\", c))
			c = '_';
	while (!n.empty() && (n.back() == '.' || n.back() == ' '))
		n.back() = '_';
	std::string stem = ToLower(n.substr(0, n.find('.')));
	static const char *reserved[] = { "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
		"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" };
	for (const char *r : reserved)
		if (stem == r)
			return "_" + n;
#else
	for (char &c : n)
		if (c == '/' || c == '\0')
			c = '_';
#endif
	return n;
}

struct TreeEntry {
	ZArchiveNodeHandle handle;
	std::string rel;  // relative path on disk, '/' separated
	bool dir;
	uint64_t size;
};

// Every node under the root, folders before what's in them.
bool WalkTree(ZArchiveReader &r, std::vector<TreeEntry> *out, std::string *error) {
	ZArchiveNodeHandle root = r.LookUp("", false, true);
	if (root == ZARCHIVE_INVALID_NODE) {
		*error = "the archive has no root folder";
		return false;
	}
	std::vector<std::pair<ZArchiveNodeHandle, std::string>> stack{ { root, "" } };
	while (!stack.empty()) {
		auto [dir, prefix] = stack.back();
		stack.pop_back();
		const uint32_t n = r.GetDirEntryCount(dir);
		for (uint32_t i = 0; i < n; ++i) {
			ZArchiveReader::DirEntry e;
			if (!r.GetDirEntry(dir, i, e)) {
				*error = "a folder in the archive is damaged";
				return false;
			}
			const std::string archivePath = prefix.empty() ? std::string(e.name) : prefix + "/" + std::string(e.name);
			ZArchiveNodeHandle h = r.LookUp(archivePath, true, true);
			if (h == ZARCHIVE_INVALID_NODE) {
				*error = archivePath + ": can't be looked up";
				return false;
			}
			TreeEntry t{ h, "", e.isDirectory, e.isDirectory ? 0 : r.GetFileSize(h) };
			out->push_back(t);
			out->back().rel = archivePath;  // the disk name is made in ExtractZArchive
			if (e.isDirectory)
				stack.push_back({ h, archivePath });
		}
	}
	return true;
}

std::string DiskRel(const std::string &archivePath) {
	std::string rel;
	size_t start = 0;
	while (start <= archivePath.size()) {
		size_t slash = archivePath.find('/', start);
		std::string_view part(archivePath.data() + start, (slash == std::string::npos ? archivePath.size() : slash) - start);
		if (!part.empty())
			rel += (rel.empty() ? "" : "/") + SafeName(part);
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return rel;
}

}  // namespace

bool ZArchiveContents(const std::string &zarPath, uint64_t *files, uint64_t *bytes, std::string *error) {
	std::unique_ptr<ZArchiveReader> r(ZArchiveReader::OpenFromFile(U8Path(zarPath)));
	if (!r) {
		*error = "not a valid ZArchive";
		return false;
	}
	std::vector<TreeEntry> tree;
	if (!WalkTree(*r, &tree, error))
		return false;
	*files = *bytes = 0;
	for (const TreeEntry &t : tree)
		if (!t.dir) {
			++*files;
			*bytes += t.size;
		}
	return true;
}

bool ExtractZArchive(const std::string &zarPath, const std::string &outDir, OpContext &ctx, std::vector<ZarWritten> *written) {
	auto fail = [&](const std::string &msg) {
		if (ctx.error.empty())
			ctx.error = msg;
		return false;
	};
	std::unique_ptr<ZArchiveReader> r(ZArchiveReader::OpenFromFile(U8Path(zarPath)));
	if (!r)
		return fail("not a valid ZArchive");
	std::vector<TreeEntry> tree;
	std::string err;
	if (!WalkTree(*r, &tree, &err))
		return fail(err);
	uint64_t total = 0, done = 0;
	for (const TreeEntry &t : tree)
		total += t.size;
	total = std::max<uint64_t>(total, 1);

	const fs::path base = U8Path(outDir);
	std::error_code ec;
	fs::create_directories(base, ec);
	if (ec)
		return fail("can't create " + outDir + ": " + ec.message());
	std::vector<uint8_t> buf(4u << 20);
	for (const TreeEntry &t : tree) {
		const std::string rel = DiskRel(t.rel);
		const fs::path dst = base / U8Path(rel);
		if (t.dir) {
			fs::create_directories(dst, ec);
			if (ec)
				return fail("can't create " + PathU8(dst) + ": " + ec.message());
			continue;
		}
		fs::create_directories(dst.parent_path(), ec);
		File f;
		if (!f.Open(PathU8(dst), File::Write))
			return fail(PathU8(dst) + ": " + f.LastError());
		StreamHash h;
		for (uint64_t pos = 0; pos < t.size;) {
			if (ctx.Cancelled()) {
				ctx.cancelled = true;
				return fail("cancelled");
			}
			uint64_t n = r->ReadFromFile(t.handle, pos, std::min<uint64_t>(buf.size(), t.size - pos), buf.data());
			if (n == 0)
				return fail(Fmt("%s: the archive can't be read at %llu (damaged?)", t.rel.c_str(), (unsigned long long)pos));
			if (!f.WriteAt(pos, buf.data(), (size_t)n))
				return fail(PathU8(dst) + ": " + f.LastError());
			h.Update(buf.data(), (size_t)n);
			pos += n;
			if (ctx.progress)
				ctx.progress((double)(done + pos) / (double)total);
		}
		f.Close();
		written->push_back({ rel, t.size, h.Final() });
		done += t.size;
	}
	if (ctx.progress)
		ctx.progress(1.0);
	return true;
}

bool TestZArchive(const std::string &zarPath, OpContext &ctx, std::string *verdict) {
	std::unique_ptr<ZArchiveReader> r(ZArchiveReader::OpenFromFile(U8Path(zarPath)));
	if (!r) {
		*verdict = "not a valid ZArchive";
		return false;
	}
	bool stopped = false;
	const bool ok = r->VerifyIntegrity([&](uint64_t done, uint64_t total) {
		if (ctx.Cancelled()) {
			stopped = true;
			return false;
		}
		if (ctx.progress && total)
			ctx.progress((double)done / (double)total);
		return true;
	});
	if (stopped) {
		ctx.cancelled = true;
		ctx.error = "cancelled";
		*verdict = "cancelled";
		return false;
	}
	*verdict = ok ? "OK, SHA-256 matches" : "SHA-256 mismatch: the archive is damaged";
	return ok;
}

bool VerifyExtractedZArchive(const std::string &outDir, const std::vector<ZarWritten> &written, OpContext &ctx, std::string *verdict) {
	uint64_t total = 0, done = 0;
	for (auto &x : written)
		total += x.size;
	total = std::max<uint64_t>(total, 1);
	std::vector<uint8_t> buf(4u << 20);
	for (const ZarWritten &x : written) {
		const std::string path = PathU8(U8Path(outDir) / U8Path(x.archivePath));
		File f;
		if (!f.Open(path, File::Read) || f.Size() != x.size) {
			*verdict = x.archivePath + (f.IsOpen() ? ": wrong size" : ": missing");
			return false;
		}
		StreamHash h;
		for (uint64_t pos = 0; pos < x.size;) {
			if (ctx.Cancelled()) {
				ctx.cancelled = true;
				ctx.error = "cancelled";
				return false;
			}
			size_t n = (size_t)std::min<uint64_t>(buf.size(), x.size - pos);
			if (!f.ReadExact(pos, buf.data(), n)) {
				*verdict = x.archivePath + ": " + f.LastError();
				return false;
			}
			h.Update(buf.data(), n);
			pos += n;
			if (ctx.progress)
				ctx.progress((double)(done + pos) / (double)total);
		}
		if (h.Final() != x.hash) {
			*verdict = x.archivePath + " differs";
			return false;
		}
		done += x.size;
	}
	*verdict = "OK";
	return true;
}

}  // namespace isopk
