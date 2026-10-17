#include "core/engine.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <chdmodern/chdmodern.h>
#include <ipbp/ipbp.h>
#include <irvz/irvz.h>
#ifdef ISOPK_WITH_CSO
#include <maxcso_capi.h>
#endif

#include "core/sysinfo.h"
#include "core/util.h"

namespace isopk {

// ---- Logging ----------------------------------------------------------------------------------

static std::mutex g_logMutex;
static LogSink g_logSink;

void SetLogSink(LogSink sink) {
	std::lock_guard<std::mutex> g(g_logMutex);
	g_logSink = std::move(sink);
}

void Log(LogLevel level, const std::string &msg) {
	std::lock_guard<std::mutex> g(g_logMutex);
	if (g_logSink)
		g_logSink(level, msg);
}

std::mutex &McsoLane() {
	static std::mutex m;
	return m;
}

void ApplyEngineSettings(const EngineSettings &e) {
	static std::once_flag once;
	std::call_once(once, [&] { ConfigureCsoThreads(e.threads); });
	RamBudget::Get().SetCapacity(e.ramBudget ? e.ramBudget : DefaultRamBudget());
	SetLowPriority(e.lowPriority);
}

std::string LibraryVersions() {
	std::string s;
#ifdef ISOPK_WITH_CSO
	s += Fmt("mCSO %s, ", maxcso_version());
#endif
	s += Fmt("CHDmodern %s, iPBP %s, iRVZ %s", chdm_version(), ipbp_version(), irvz_version());
	return s;
}

const char *JobStateName(JobState s) {
	switch (s) {
	case JobState::Queued: return "Queued";
	case JobState::Running: return "Running";
	case JobState::Done: return "Done";
	case JobState::Skipped: return "Skipped";
	case JobState::Failed: return "Failed";
	case JobState::Cancelled: return "Cancelled";
	}
	return "?";
}

const char *PhaseName(Phase p) {
	switch (p) {
	case Phase::Idle: return "";
	case Phase::WaitingRam: return "Waiting for RAM";
	case Phase::WaitingCso: return "Waiting for mCSO";
	case Phase::LoadingRam: return "Reading into RAM";
	case Phase::Extracting: return "Extracting";
	case Phase::Compressing: return "Compressing";
	case Phase::Verifying: return "Verifying";
	case Phase::Finishing: return "Finishing";
	}
	return "";
}

// ---- Library adaptors -------------------------------------------------------------------------

namespace {

void SetError(OpContext &ctx, const std::string &msg) {
	if (ctx.error.empty())
		ctx.error = msg;
}

// Bytes of the ISO readable at offset (callers may ask past the end).
size_t Clamp(ImageReader &r, uint64_t offset, size_t len) {
	uint64_t size = r.Size();
	if (offset >= size)
		return 0;
	return (size_t)std::min<uint64_t>(len, size - offset);
}

struct ReaderBox {
	explicit ReaderBox(ImageReader *r) : reader(r) {}
	ImageReader *reader;
	std::mutex mutex;
	bool Read(uint64_t offset, void *buf, size_t len) {
		if (reader->ThreadSafe())
			return reader->ReadAt(offset, buf, len);
		std::lock_guard<std::mutex> g(mutex);
		return reader->ReadAt(offset, buf, len);
	}
};

bool LoadFileBytes(const std::string &path, std::vector<uint8_t> *out, std::string *err) {
	File f;
	if (!f.Open(path, File::Read)) {
		*err = path + ": " + f.LastError();
		return false;
	}
	out->resize((size_t)f.Size());
	if (!out->empty() && !f.ReadExact(0, out->data(), out->size())) {
		*err = path + ": " + f.LastError();
		return false;
	}
	return true;
}

// --- mCSO

#ifdef ISOPK_WITH_CSO
int MAXCSO_CAPI_CALL McsoProgress(void *user, int64_t pos, int64_t total, int64_t) {
	OpContext *ctx = (OpContext *)user;
	if (ctx->progress && total > 0)
		ctx->progress((double)pos / (double)total);
	return ctx->Cancelled() ? 1 : 0;
}

int64_t MAXCSO_CAPI_CALL McsoRead(void *user, uint64_t offset, void *buf, size_t len) {
	ReaderBox *box = (ReaderBox *)user;
	size_t n = Clamp(*box->reader, offset, len);
	if (n && !box->Read(offset, buf, n))
		return -1;
	return (int64_t)n;
}

int MAXCSO_CAPI_CALL McsoBegin(void *user, uint64_t size) {
	return ((ImageWriter *)user)->Begin(size) ? 0 : 1;
}

int64_t MAXCSO_CAPI_CALL McsoWrite(void *user, uint64_t offset, const void *buf, size_t len) {
	return ((ImageWriter *)user)->WriteAt(offset, buf, len) ? (int64_t)len : -1;
}

bool McsoResult(int rc, const char *err, OpContext &ctx) {
	if (rc == MAXCSO_RESULT_SUCCESS)
		return true;
	if (rc == MAXCSO_RESULT_CANCELLED || ctx.Cancelled()) {
		ctx.cancelled = true;
		SetError(ctx, "cancelled");
	} else {
		SetError(ctx, std::string("mCSO: ") + (err && *err ? err : "error"));
	}
	return false;
}

void McsoOptions(Format target, const Settings &s, maxcso_options *o) {
	maxcso_options_init(o, MAXCSO_MODE_STANDARD);
	const CsoSettings &c = s.Cso(target);
	o->flags = c.Flags(target);
	o->block_size = c.EffectiveBlockSize(target);
	o->zopfli_split_max = c.zopfliSplits;
	o->precheck_percent = c.precheck;
}
#endif

// --- CHDmodern

int ChdProgress(void *user, double f) {
	OpContext *ctx = (OpContext *)user;
	if (ctx->progress)
		ctx->progress(f);
	return ctx->Cancelled() ? 1 : 0;
}

uint64_t ChdIoSize(void *user) {
	return ((ReaderBox *)user)->reader->Size();
}

size_t ChdIoRead(void *user, uint64_t offset, void *buf, size_t len) {
	ReaderBox *box = (ReaderBox *)user;
	size_t n = Clamp(*box->reader, offset, len);
	if (n && !box->Read(offset, buf, n))
		return (size_t)-1;
	return n;
}

int ChdBegin(void *user, uint64_t size) {
	return ((ImageWriter *)user)->Begin(size) ? 0 : 1;
}

size_t ChdWrite(void *user, uint64_t offset, const void *buf, size_t len) {
	return ((ImageWriter *)user)->WriteAt(offset, buf, len) ? len : 0;
}

bool ChdResult(int rc, OpContext &ctx, ImageWriter *w = nullptr) {
	if (rc == CHDM_OK)
		return true;
	if (rc == CHDM_ERR_CANCELLED || ctx.Cancelled()) {
		ctx.cancelled = true;
		SetError(ctx, "cancelled");
	} else if (w && !w->Error().empty()) {
		SetError(ctx, w->Error());
	} else {
		SetError(ctx, std::string("CHD: ") + chdm_last_error_message());
	}
	return false;
}

const uint32_t *ChdCodecs(const Settings &s, uint32_t list[4]) {
	if (s.chd.codecs.empty())
		return nullptr;
	for (int i = 0; i < 4; ++i)
		list[i] = i < (int)s.chd.codecs.size() ? s.chd.codecs[i] : 0;
	return list;
}

// --- iPBP

struct PbpJob {
	ipbp_job *job = ipbp_job_create();
	OpContext *ctx;
	explicit PbpJob(OpContext &c) : ctx(&c) { ipbp_job_set_progress(job, &PbpJob::Progress, this); }
	~PbpJob() { ipbp_job_destroy(job); }
	static void Progress(void *user, uint64_t done, uint64_t total) {
		PbpJob *self = (PbpJob *)user;
		if (self->ctx->progress && total)
			self->ctx->progress((double)done / (double)total);
		if (self->ctx->Cancelled())
			ipbp_job_cancel(self->job);
	}
	bool Result(int rc, ImageWriter *w = nullptr) {
		if (rc == IPBP_OK)
			return true;
		if (rc == IPBP_CANCELLED || ctx->Cancelled()) {
			ctx->cancelled = true;
			SetError(*ctx, "cancelled");
		} else if (w && !w->Error().empty()) {
			SetError(*ctx, w->Error());
		} else {
			SetError(*ctx, std::string("iPBP: ") + ipbp_job_error(job));
		}
		return false;
	}
};

int PbpRead(void *user, uint64_t offset, void *buf, size_t len) {
	ReaderBox *box = (ReaderBox *)user;
	size_t n = Clamp(*box->reader, offset, len);
	if (n && !box->Read(offset, buf, n))
		return 1;
	if (n < len)
		memset((uint8_t *)buf + n, 0, len - n);
	return 0;
}

int PbpBegin(void *user, uint64_t size) {
	return ((ImageWriter *)user)->Begin(size) ? 0 : 1;
}

int PbpWrite(void *user, uint64_t offset, const void *buf, size_t len) {
	return ((ImageWriter *)user)->WriteAt(offset, buf, len) ? 0 : 1;
}

int PbpFinish(void *user, uint64_t finalSize) {
	return ((ImageWriter *)user)->Finish(finalSize) ? 0 : 1;
}

bool PbpOptions(const Settings &s, ipbp_iso2pbp_options *o, std::vector<uint8_t> *png, OpContext &ctx) {
	ipbp_iso2pbp_options_init(o);
	o->compress = s.pbp.compress;
	o->threads = s.engine.threads;
	if (!s.pbp.contentId.empty())
		o->content_id = s.pbp.contentId.c_str();
	if (!s.pbp.startDatPng.empty()) {
		std::string err;
		if (!LoadFileBytes(s.pbp.startDatPng, png, &err)) {
			SetError(ctx, "boot screen: " + err);
			return false;
		}
		o->startdat_png = png->data();
		o->startdat_png_size = png->size();
	}
	return true;
}

void PbpExtractOptions(const Settings &s, bool trim, ipbp_pbp2iso_options *o) {
	ipbp_pbp2iso_options_init(o);
	o->verify = s.pbp.verifyMacs ? 1 : 0;
	o->trim = trim ? 1 : 0;
	o->threads = s.engine.threads;
}

// --- iRVZ

int IrvzProgress(void *user, uint64_t done, uint64_t total) {
	OpContext *ctx = (OpContext *)user;
	if (ctx->progress && total)
		ctx->progress((double)done / (double)total);
	return ctx->Cancelled() ? 1 : 0;
}

int IrvzRead(void *user, uint64_t offset, void *buf, size_t len) {
	ReaderBox *box = (ReaderBox *)user;
	size_t n = Clamp(*box->reader, offset, len);
	if (n && !box->Read(offset, buf, n))
		return 1;
	if (n < len)
		memset((uint8_t *)buf + n, 0, len - n);
	return 0;
}

int IrvzBegin(void *user, uint64_t size) {
	return ((ImageWriter *)user)->Begin(size) ? 0 : 1;
}

int IrvzWrite(void *user, uint64_t offset, const void *buf, size_t len) {
	return ((ImageWriter *)user)->WriteAt(offset, buf, len) ? 0 : 1;
}

int IrvzFinish(void *user, uint64_t finalSize) {
	return ((ImageWriter *)user)->Finish(finalSize) ? 0 : 1;
}

bool IrvzResult(int rc, OpContext &ctx, ImageWriter *w = nullptr) {
	if (rc == IRVZ_OK)
		return true;
	if (rc == IRVZ_ERR_CANCELLED || ctx.Cancelled()) {
		ctx.cancelled = true;
		SetError(ctx, "cancelled");
	} else if (w && !w->Error().empty()) {
		SetError(ctx, w->Error());
	} else {
		const char *detail = irvz_last_error();
		SetError(ctx, std::string("iRVZ: ") + (detail && *detail ? detail : irvz_error_string(rc)));
	}
	return false;
}

void IrvzOptions(Format target, const Settings &s, irvz_compress_options *o) {
	const irvz_format fmt = target == Format::WIA ? IRVZ_FORMAT_WIA : IRVZ_FORMAT_RVZ;
	irvz_compress_options_init(o, fmt);
	o->format = fmt;
	const RvzSettings &r = s.Rvz(target);
	o->compression = (irvz_compression)r.EffectiveMethod(target);
	o->level = r.EffectiveLevel(target);
	o->chunk_size = r.EffectiveChunk(target);
	o->threads = (int)s.engine.threads;
}

}  // namespace

// ---- Operations -------------------------------------------------------------------------------

bool CompressFile(const std::string &iso, Format target, const Settings &s, const std::string &out, OpContext &ctx) {
	switch (target) {
#ifdef ISOPK_WITH_CSO
	case Format::CSO:
	case Format::CSO2:
	case Format::ZSO:
	case Format::DAX: {
		std::lock_guard<std::mutex> lane(McsoLane());
		maxcso_options o;
		McsoOptions(target, s, &o);
		char err[512] = "";
		int rc = maxcso_process_ex(iso.c_str(), out.c_str(), &o, &McsoProgress, &ctx, err, sizeof(err));
		return McsoResult(rc, err, ctx);
	}
#endif
	case Format::CHD: {
		uint32_t list[4];
		int rc = chdm_create_dvd(iso.c_str(), out.c_str(), s.chd.hunkBytes, ChdCodecs(s, list), &ChdProgress, &ctx);
		return ChdResult(rc, ctx);
	}
	case Format::PBP: {
		PbpJob job(ctx);
		ipbp_iso2pbp_options o;
		std::vector<uint8_t> png;
		if (!PbpOptions(s, &o, &png, ctx))
			return false;
		return job.Result(ipbp_iso_to_pbp(job.job, iso.c_str(), out.c_str(), &o));
	}
	case Format::WIA:
	case Format::RVZ: {
		irvz_compress_options o;
		IrvzOptions(target, s, &o);
		return IrvzResult(irvz_compress_file(iso.c_str(), out.c_str(), &o, &IrvzProgress, &ctx), ctx);
	}
	default:
		SetError(ctx, Fmt("can't compress to %s", FormatName(target)));
		return false;
	}
}

bool CompressImage(ImageReader &iso, Format target, const Settings &s, const std::string &out, OpContext &ctx) {
	ReaderBox box(&iso);
	switch (target) {
#ifdef ISOPK_WITH_CSO
	case Format::CSO:
	case Format::CSO2:
	case Format::ZSO:
	case Format::DAX: {
		std::lock_guard<std::mutex> lane(McsoLane());
		maxcso_options o;
		McsoOptions(target, s, &o);
		maxcso_reader r{ &box, iso.Size(), &McsoRead };
		char err[512] = "";
		int rc = maxcso_process_reader(&r, out.c_str(), &o, &McsoProgress, &ctx, err, sizeof(err));
		return McsoResult(rc, err, ctx);
	}
#endif
	case Format::CHD: {
		chdm_io io{ &box, &ChdIoSize, &ChdIoRead, nullptr };
		uint32_t list[4];
		int rc = chdm_create_dvd_io(&io, out.c_str(), s.chd.hunkBytes, ChdCodecs(s, list), &ChdProgress, &ctx);
		return ChdResult(rc, ctx);
	}
	case Format::PBP: {
		PbpJob job(ctx);
		ipbp_iso2pbp_options o;
		std::vector<uint8_t> png;
		if (!PbpOptions(s, &o, &png, ctx))
			return false;
		ipbp_reader r{ &box, iso.Size(), &PbpRead };
		return job.Result(ipbp_iso_to_pbp_reader(job.job, &r, out.c_str(), &o));
	}
	case Format::WIA:
	case Format::RVZ: {
		irvz_compress_options o;
		IrvzOptions(target, s, &o);
		irvz_reader r{ &box, iso.Size(), &IrvzRead };
		FileWriter fw(out);
		irvz_writer w{ &fw, &IrvzBegin, &IrvzWrite, &IrvzFinish };
		return IrvzResult(irvz_compress(&r, &w, &o, &IrvzProgress, &ctx), ctx, &fw);
	}
	default:
		SetError(ctx, Fmt("can't compress to %s", FormatName(target)));
		return false;
	}
}

bool DecompressToFile(const std::string &in, Format format, const Settings &s, const std::string &iso, bool trimPbp, OpContext &ctx) {
	switch (format) {
#ifdef ISOPK_WITH_CSO
	case Format::CSO:
	case Format::CSO2:
	case Format::ZSO:
	case Format::DAX: {
		std::lock_guard<std::mutex> lane(McsoLane());
		char err[512] = "";
		int rc = maxcso_process(in.c_str(), iso.c_str(), MAXCSO_MODE_DECOMPRESS, &McsoProgress, &ctx, err, sizeof(err));
		return McsoResult(rc, err, ctx);
	}
#endif
	case Format::CHD:
		return ChdResult(chdm_extract_raw(in.c_str(), iso.c_str(), &ChdProgress, &ctx), ctx);
	case Format::PBP: {
		PbpJob job(ctx);
		ipbp_pbp2iso_options o;
		PbpExtractOptions(s, trimPbp, &o);
		return job.Result(ipbp_pbp_to_iso(job.job, in.c_str(), iso.c_str(), &o));
	}
	case Format::ISO: {
		FileWriter w(iso);
		return DecompressToWriter(in, format, s, w, trimPbp, ctx);
	}
	case Format::WIA:
	case Format::RVZ:
		return IrvzResult(irvz_extract_file(in.c_str(), iso.c_str(), (int)s.engine.threads, &IrvzProgress, &ctx), ctx);
	default:
		SetError(ctx, Fmt("can't extract %s", FormatName(format)));
		return false;
	}
}

bool DecompressToWriter(const std::string &in, Format format, const Settings &s, ImageWriter &w, bool trimPbp, OpContext &ctx) {
	switch (format) {
#ifdef ISOPK_WITH_CSO
	case Format::CSO:
	case Format::CSO2:
	case Format::ZSO:
	case Format::DAX: {
		std::lock_guard<std::mutex> lane(McsoLane());
		maxcso_writer mw{ &w, &McsoBegin, &McsoWrite };
		char err[512] = "";
		int rc = maxcso_decompress_writer(in.c_str(), &mw, &McsoProgress, &ctx, err, sizeof(err));
		if (rc != MAXCSO_RESULT_SUCCESS && !w.Error().empty() && !ctx.Cancelled()) {
			SetError(ctx, w.Error());
			return false;
		}
		return McsoResult(rc, err, ctx) && w.Finish(UINT64_MAX);
	}
#endif
	case Format::CHD: {
		chdm_writer cw{ &w, &ChdBegin, &ChdWrite };
		return ChdResult(chdm_extract_raw_io(in.c_str(), &cw, &ChdProgress, &ctx), ctx, &w) && w.Finish(UINT64_MAX);
	}
	case Format::PBP: {
		PbpJob job(ctx);
		ipbp_pbp2iso_options o;
		PbpExtractOptions(s, trimPbp, &o);
		ipbp_writer pw{ &w, &PbpBegin, &PbpWrite, &PbpFinish };
		return job.Result(ipbp_pbp_to_iso_writer(job.job, in.c_str(), &pw, &o), &w);
	}
	case Format::ISO: {
		File f;
		if (!f.Open(in, File::Read)) {
			SetError(ctx, in + ": " + f.LastError());
			return false;
		}
		const uint64_t size = f.Size();
		if (!w.Begin(size)) {
			SetError(ctx, w.Error());
			return false;
		}
		std::vector<uint8_t> buf(8u << 20);
		for (uint64_t pos = 0; pos < size; pos += buf.size()) {
			if (ctx.Cancelled()) {
				ctx.cancelled = true;
				SetError(ctx, "cancelled");
				return false;
			}
			size_t n = (size_t)std::min<uint64_t>(buf.size(), size - pos);
			if (!f.ReadExact(pos, buf.data(), n)) {
				SetError(ctx, in + ": " + f.LastError());
				return false;
			}
			if (!w.WriteAt(pos, buf.data(), n)) {
				SetError(ctx, w.Error());
				return false;
			}
			if (ctx.progress)
				ctx.progress((double)(pos + n) / (double)size);
		}
		return w.Finish(size);
	}
	case Format::WIA:
	case Format::RVZ: {
		irvz_file *f = nullptr;
		if (irvz_open(in.c_str(), &f) != IRVZ_OK)
			return IrvzResult(IRVZ_ERR_OPEN, ctx);
		irvz_writer iw{ &w, &IrvzBegin, &IrvzWrite, &IrvzFinish };
		int rc = irvz_extract(f, &iw, (int)s.engine.threads, &IrvzProgress, &ctx);
		irvz_close(f);
		return IrvzResult(rc, ctx, &w);
	}
	default:
		SetError(ctx, Fmt("can't extract %s", FormatName(format)));
		return false;
	}
}

// ---- Jobs -------------------------------------------------------------------------------------

std::string PlanOutputPath(const InputFile &in, Format target, const Settings &s) {
	fs::path src = U8Path(in.path);
	fs::path dir;
	if (s.out.dir.empty()) {
		dir = src.parent_path();
		// .../Game/EBOOT.PBP -> next to the Game folder.
		if (ToLower(PathU8(src.filename())) == "eboot.pbp" && dir.has_parent_path())
			dir = dir.parent_path();
	} else {
		dir = U8Path(s.out.dir);
		if (s.out.mirrorTree && !in.baseDir.empty()) {
			fs::path srcDir = src.parent_path();
			if (ToLower(PathU8(src.filename())) == "eboot.pbp")
				srcDir = srcDir.parent_path();
			std::error_code ec;
			fs::path rel = fs::relative(srcDir, U8Path(in.baseDir), ec);
			if (!ec && !rel.empty() && PathU8(rel) != "." && PathU8(rel).rfind("..", 0) != 0)
				dir /= rel;
		}
	}
	std::string name = GameName(in.path);
	if (target == Format::PBP && s.pbp.layout == PbpLayout::Folder)
		return PathU8(dir / U8Path(name) / "EBOOT.PBP");
	return PathU8(dir / U8Path(name + FormatExtension(target)));
}

namespace {

bool SamePath(const std::string &a, const std::string &b) {
	std::error_code ec;
	if (fs::exists(U8Path(a), ec) && fs::exists(U8Path(b), ec))
		return fs::equivalent(U8Path(a), U8Path(b), ec);
	return fs::absolute(U8Path(a), ec).lexically_normal() == fs::absolute(U8Path(b), ec).lexically_normal();
}

std::mutex &ClaimMutex() {
	static std::mutex m;
	return m;
}

std::set<std::string> &Claimed() {
	static std::set<std::string> s;
	return s;
}

std::string ClaimKey(const std::string &p) {
	std::error_code ec;
	std::string k = PathU8(fs::absolute(U8Path(p), ec).lexically_normal());
#ifdef _WIN32
	k = ToLower(k);
#endif
	return k;
}

// Holds an output path for the lifetime of a job.
struct OutputClaim {
	std::string key;
	void Take(const std::string &p) {  // called with ClaimMutex held
		key = ClaimKey(p);
		Claimed().insert(key);
	}
	~OutputClaim() {
		if (!key.empty()) {
			std::lock_guard<std::mutex> g(ClaimMutex());
			Claimed().erase(key);
		}
	}
};

// "Game (2).cso" style alternative that isn't taken yet.
template <typename Taken>
std::string UniquePath(const std::string &path, bool pbpFolder, Taken taken) {
	fs::path p = U8Path(path);
	std::error_code ec;
	for (int i = 2; i < 10000; ++i) {
		fs::path cand;
		if (pbpFolder) {
			fs::path folder = p.parent_path();
			cand = folder.parent_path() / U8Path(PathU8(folder.filename()) + Fmt(" (%d)", i)) / p.filename();
		} else {
			cand = p.parent_path() / U8Path(PathU8(p.stem()) + Fmt(" (%d)", i) + PathU8(p.extension()));
		}
		if (!taken(PathU8(cand)))
			return PathU8(cand);
	}
	return path;
}

void RemoveFile(const std::string &path) {
	std::error_code ec;
	fs::remove(U8Path(path), ec);
}

// Overall progress across the phases this job will go through.
struct Plan {
	std::vector<std::pair<Phase, double>> phases;
	double Overall(Phase cur, double f) const {
		double total = 0, done = 0;
		bool found = false;
		for (auto &p : phases) {
			total += p.second;
			if (!found && p.first == cur) {
				done += p.second * f;
				found = true;
			} else if (!found) {
				done += p.second;
			}
		}
		return total > 0 ? std::min(1.0, done / total) : 0.0;
	}
};

}  // namespace

void RunJob(Job &job) {
	const Settings &s = job.settings;
	const InputFile &in = job.input;
	const auto t0 = Clock::now();
	JobResult res;
	res.inputSize = in.size;
	job.state = (int)JobState::Running;

	auto finish = [&](JobState st, const std::string &msg) {
		res.state = st;
		if (!msg.empty())
			res.message = msg;
		res.seconds = SecondsSince(t0);
		job.phase = (int)Phase::Idle;
		if (st == JobState::Done)
			job.overall = 1.0;
		{
			std::lock_guard<std::mutex> g(job.resultMutex);
			job.result = res;
		}
		job.state = (int)st;
		const char *name = in.path.c_str();
		switch (st) {
		case JobState::Done:
			Log(LogLevel::Info, Fmt("Done: %s -> %s (%s, %.1f%%, %s)%s", name, res.outputPath.c_str(), HumanBytes(res.outputSize).c_str(),
				res.isoSize ? 100.0 * res.outputSize / res.isoSize : 0.0, HumanDuration(res.seconds).c_str(),
				res.verify == "OK" ? ", verified" : ""));
			break;
		case JobState::Skipped: Log(LogLevel::Info, Fmt("Skipped: %s: %s", name, res.message.c_str())); break;
		case JobState::Cancelled: Log(LogLevel::Warning, Fmt("Cancelled: %s", name)); break;
		case JobState::Failed: Log(LogLevel::Error, Fmt("Failed: %s: %s", name, res.message.c_str())); break;
		default: break;
		}
	};

	Format S = in.format == Format::Unknown ? DetectFormat(in.path) : in.format;
	const Format T = job.target.load();
	if (S == Format::Unknown)
		return finish(JobState::Failed, "not a recognised disc image");
	if (!FormatAvailable(S) || !FormatAvailable(T))
		return finish(JobState::Failed, "format not supported by this build");
	if (S == Format::ISO && T == Format::ISO)
		return finish(JobState::Skipped, "already an ISO");
	// Formats that only hold some platforms' discs (RVZ/WIA: GameCube and Wii; PBP: PSP).
	ImageInfo info = InspectImage(in.path);
	if (std::string why = TargetRejects(T, info.platform); !why.empty())
		return finish(JobState::Skipped, why);

	// Where it goes.
	std::string final = PlanOutputPath(in, T, s);
	const bool pbpFolder = T == Format::PBP && s.pbp.layout == PbpLayout::Folder;
	const bool replacingSource = SamePath(final, in.path);
	std::error_code ec;
	// Claim the output path, so parallel jobs heading for the same file don't collide.
	OutputClaim claim;
	{
		std::lock_guard<std::mutex> g(ClaimMutex());
		auto taken = [&](const std::string &p) { return fs::exists(U8Path(p), ec) || Claimed().count(ClaimKey(p)) != 0; };
		if (taken(final)) {
			const bool busy = Claimed().count(ClaimKey(final)) != 0;
			if (replacingSource && s.out.conflict != Conflict::Overwrite)
				final = UniquePath(final, pbpFolder, taken);
			else if (s.out.conflict == Conflict::Skip)
				return finish(JobState::Skipped, "output exists: " + final);
			else if (s.out.conflict == Conflict::Rename || busy)
				final = UniquePath(final, pbpFolder, taken);
		}
		claim.Take(final);
	}
	res.outputPath = final;
	// Folders this job creates go again if it doesn't finish (e.g. Game/ for Game/EBOOT.PBP).
	std::vector<fs::path> createdDirs;
	for (fs::path d = U8Path(final).parent_path(); !d.empty() && !fs::exists(d, ec); d = d.parent_path()) {
		createdDirs.push_back(d);
		if (d == d.parent_path())
			break;
	}
	fs::create_directories(U8Path(final).parent_path(), ec);
	if (ec)
		return finish(JobState::Failed, "can't create " + PathU8(U8Path(final).parent_path()) + ": " + ec.message());
	const std::string part = final + ".part";
	RemoveFile(part);

	// Rough free space check (the output is at most about the ISO's size).
	res.isoSize = info.isoSize;
	uint64_t need = T == Format::ISO ? info.isoSize : info.isoSize / 2;
	uint64_t freeSpace = FreeDiskSpace(final);
	if (need && freeSpace != UINT64_MAX && freeSpace < need)
		Log(LogLevel::Warning, Fmt("%s: only %s free next to the output", in.path.c_str(), HumanBytes(freeSpace).c_str()));

	Plan plan;
	const bool needSource = T != Format::ISO;  // compressing needs the plain image
	const bool srcIsIso = S == Format::ISO;
	if (needSource && !srcIsIso)
		plan.phases.push_back({ Phase::Extracting, 0.25 });
	plan.phases.push_back({ T == Format::ISO ? Phase::Extracting : Phase::Compressing, T == Format::ISO ? 0.5 : 1.0 });
	if (s.engine.verify)
		plan.phases.push_back({ Phase::Verifying, T == Format::ISO ? 0.5 : 0.25 });

	OpContext ctx;
	ctx.cancel = &job.cancel;
	Phase cur = Phase::Idle;
	ctx.progress = [&](double f) {
		job.phaseProgress = f;
		job.overall = plan.Overall(cur, f);
	};
	auto enter = [&](Phase p) {
		cur = p;
		job.phase = (int)p;
		job.phaseProgress = 0.0;
		job.overall = plan.Overall(p, 0.0);
	};
	auto fail = [&](const std::string &what) {
		RemoveFile(part);
		for (const fs::path &d : createdDirs)  // deepest first
			if (fs::is_empty(d, ec))
				fs::remove(d, ec);
		if (ctx.cancelled || job.cancel.load())
			return finish(JobState::Cancelled, "cancelled");
		return finish(JobState::Failed, what.empty() ? ctx.error : what + ": " + ctx.error);
	};

	Log(LogLevel::Info, Fmt("Start: %s (%s) -> %s", in.path.c_str(), FormatName(S), FormatName(T)));

	std::unique_ptr<MemoryImage> mem;
	std::string tempIso;
	auto cleanupTemp = [&] {
		if (!tempIso.empty())
			RemoveFile(tempIso);
		tempIso.clear();
	};
	std::string srcIsoPath = srcIsIso ? in.path : std::string();

	// 1. The plain image, when compressing from a compressed source.
	if (needSource && !srcIsIso) {
		Clock::time_point tx = Clock::now();
		bool haveRam = false;
		if (s.engine.repackInRam && info.isoSize && info.isoSize <= RamBudget::Get().Capacity()) {
			// Other jobs may hold the RAM: wait here, so the UI can show it.
			if (RamBudget::Get().InUse() + info.isoSize > RamBudget::Get().Capacity()) {
				enter(Phase::WaitingRam);
				while (!job.cancel.load() && RamBudget::Get().InUse() + info.isoSize > RamBudget::Get().Capacity())
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (job.cancel.load())
					return finish(JobState::Cancelled, "cancelled");
			}
			mem = std::make_unique<MemoryImage>(&job.cancel);
			enter(Phase::Extracting);
			if (DecompressToWriter(in.path, S, s, *mem, s.pbp.trimOnExtract, ctx)) {
				haveRam = true;
				res.viaRam = true;
			} else if (mem->OutOfMemory() && !job.cancel.load()) {
				Log(LogLevel::Warning, Fmt("%s: %s, using a temporary ISO instead", in.path.c_str(), mem->Error().c_str()));
				ctx.error.clear();
			} else {
				return fail("extracting");
			}
			if (!haveRam)
				mem.reset();
		} else if (s.engine.repackInRam && info.isoSize) {
			Log(LogLevel::Warning, Fmt("%s: the %s image is bigger than the RAM budget (%s), using a temporary ISO", in.path.c_str(),
				HumanBytes(info.isoSize).c_str(), HumanBytes(RamBudget::Get().Capacity()).c_str()));
		}
		if (!haveRam) {
			fs::path tdir = s.engine.tempDir.empty() ? U8Path(final).parent_path() : U8Path(s.engine.tempDir);
			fs::create_directories(tdir, ec);
			tempIso = PathU8(tdir / U8Path(GameName(in.path) + ".isopk-temp.iso"));
			enter(Phase::Extracting);
			if (!DecompressToFile(in.path, S, s, tempIso, s.pbp.trimOnExtract, ctx)) {
				cleanupTemp();
				return fail("extracting");
			}
			res.viaTempFile = true;
			srcIsoPath = tempIso;
		}
		res.extractSecs = SecondsSince(tx);
	}

	// 2. Compress, or extract to ISO.
	{
		Clock::time_point tc = Clock::now();
		bool ok;
		if (T == Format::ISO) {
			enter(Phase::Extracting);
			ok = DecompressToFile(in.path, S, s, part, s.pbp.trimOnExtract, ctx);
		} else {
			// Show when another job holds mCSO (CompressFile / CompressImage take the lane themselves).
			if (IsMcsoFormat(T) && McsoLane().try_lock())
				McsoLane().unlock();
			else if (IsMcsoFormat(T))
				enter(Phase::WaitingCso);
			if (IsMcsoFormat(T))
				ctx.progress = [&, first = true](double f) mutable {
					if (first) {
						enter(Phase::Compressing);
						first = false;
					}
					job.phaseProgress = f;
					job.overall = plan.Overall(cur, f);
				};
			else
				enter(Phase::Compressing);
			ok = mem ? CompressImage(*mem, T, s, part, ctx) : CompressFile(srcIsoPath, T, s, part, ctx);
		}
		if (!ok) {
			cleanupTemp();
			return fail(T == Format::ISO ? "extracting" : "compressing");
		}
		if (T == Format::ISO)
			res.extractSecs = SecondsSince(tc);
		else
			res.compressSecs = SecondsSince(tc);
	}

	// 3. Verify: decode again and compare byte for byte, without writing anything.
	if (s.engine.verify) {
		Clock::time_point tv = Clock::now();
		enter(Phase::Verifying);
		bool ok;
		std::string verdict;
		if (T == Format::ISO) {
			// The written ISO against a second decode of the source.
			FileImageReader written;
			if (!written.Open(part)) {
				cleanupTemp();
				ctx.error = written.Error();
				return fail("verifying");
			}
			CompareWriter cmp(&written);
			ok = DecompressToWriter(in.path, S, s, cmp, false, ctx);
			verdict = ok ? cmp.Result() : "";
			ok = ok && cmp.Matched();
		} else {
			// The new image against the plain one it was made from.
			FileImageReader fileRef;
			ImageReader *ref = mem.get();
			if (!ref) {
				if (!fileRef.Open(srcIsoPath)) {
					cleanupTemp();
					ctx.error = fileRef.Error();
					return fail("verifying");
				}
				ref = &fileRef;
			}
			CompareWriter cmp(ref);
			ok = DecompressToWriter(part, T, s, cmp, false, ctx);
			verdict = ok ? cmp.Result() : "";
			ok = ok && cmp.Matched();
		}
		res.verifySecs = SecondsSince(tv);
		if (!ok) {
			cleanupTemp();
			if (!verdict.empty())
				ctx.error = verdict;
			res.verify = verdict.empty() ? ctx.error : verdict;
			return fail("verify failed");
		}
		res.verify = "OK";
	} else {
		res.verify = "off";
	}
	mem.reset();
	cleanupTemp();

	// 4. Put it in place.
	enter(Phase::Finishing);
	if (replacingSource || fs::exists(U8Path(final), ec))
		RemoveFile(final);
	fs::rename(U8Path(part), U8Path(final), ec);
	if (ec) {
		RemoveFile(part);
		return finish(JobState::Failed, "can't rename the output: " + ec.message());
	}
	res.outputSize = fs::file_size(U8Path(final), ec);
	if (!res.isoSize)
		res.isoSize = T == Format::ISO ? res.outputSize : 0;

	if (s.out.deleteSource && !replacingSource && (res.verify == "OK" || !s.engine.verify)) {
		RemoveFile(in.path);
		// A PBP's game folder goes too once it's empty.
		fs::path parent = U8Path(in.path).parent_path();
		if (ToLower(PathU8(U8Path(in.path).filename())) == "eboot.pbp" && fs::is_empty(parent, ec))
			fs::remove(parent, ec);
		res.message = "source deleted";
	}
	finish(JobState::Done, "");
}

}  // namespace isopk
