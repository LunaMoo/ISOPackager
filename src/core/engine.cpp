#include "core/engine.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <chdmodern/chdmodern.h>
#include <ipbp/ipbp.h>
#include <irvz/irvz.h>
#include <zarchive/zarchivecommon.h>
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
	s += Fmt("CHDmodern %s, iPBP %s, iRVZ %s, ZArchive %s", chdm_version(), ipbp_version(), irvz_version(), ZARCHIVE_VERSION);
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
	case Phase::Archiving: return "Archiving";
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

size_t ChdOutRead(void *user, uint64_t offset, void *buf, size_t len) {
	return ((MemoryOutput *)user)->ReadAt(offset, buf, len);
}

size_t ChdOutWrite(void *user, uint64_t offset, const void *buf, size_t len) {
	return ((MemoryOutput *)user)->WriteAt(offset, buf, len) ? len : (size_t)-1;
}

uint64_t ChdOutSize(void *user) {
	return ((MemoryOutput *)user)->Size();
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

bool CompressToMemory(ImageReader &iso, Format target, const Settings &s, MemoryOutput &out, OpContext &ctx) {
	ReaderBox box(&iso);
	// Room for about the ISO's size up front: a compressed image rarely needs more, and growing
	// a multi-gigabyte buffer means copying it.
	if (!out.Begin(iso.Size())) {
		SetError(ctx, out.Error());
		return false;
	}
	auto writeFailed = [&] {
		if (!out.Error().empty() && !ctx.Cancelled())
			SetError(ctx, out.Error());
	};
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
		maxcso_writer w{ (ImageWriter *)&out, &McsoBegin, &McsoWrite };
		char err[512] = "";
		int rc = maxcso_process_io(nullptr, &r, &w, &o, &McsoProgress, &ctx, err, sizeof(err));
		if (rc != MAXCSO_RESULT_SUCCESS)
			writeFailed();
		return McsoResult(rc, err, ctx) && out.Finish(UINT64_MAX);
	}
#endif
	case Format::CHD: {
		chdm_io io{ &box, &ChdIoSize, &ChdIoRead, nullptr };
		chdm_output co{ &out, &ChdOutRead, &ChdOutWrite, &ChdOutSize };
		uint32_t list[4];
		int rc = chdm_create_dvd_to_output(&io, &co, s.chd.hunkBytes, ChdCodecs(s, list), &ChdProgress, &ctx);
		return ChdResult(rc, ctx, &out) && out.Finish(UINT64_MAX);
	}
	case Format::PBP: {
		PbpJob job(ctx);
		ipbp_iso2pbp_options o;
		std::vector<uint8_t> png;
		if (!PbpOptions(s, &o, &png, ctx))
			return false;
		ipbp_reader r{ &box, iso.Size(), &PbpRead };
		ipbp_writer w{ (ImageWriter *)&out, nullptr, &PbpWrite, &PbpFinish };
		return job.Result(ipbp_iso_to_pbp_io(job.job, &r, &w, &o), &out);
	}
	case Format::WIA:
	case Format::RVZ: {
		irvz_compress_options o;
		IrvzOptions(target, s, &o);
		irvz_reader r{ &box, iso.Size(), &IrvzRead };
		irvz_writer w{ (ImageWriter *)&out, &IrvzBegin, &IrvzWrite, &IrvzFinish };
		return IrvzResult(irvz_compress(&r, &w, &o, &IrvzProgress, &ctx), ctx, &out);
	}
	default:
		SetError(ctx, Fmt("can't compress to %s", FormatName(target)));
		return false;
	}
}

// A whole reader into w, 8 MiB at a time (a plain ISO, or a DVD's image in a CD CHD).
static bool CopyImage(ImageReader &r, const std::string &name, ImageWriter &w, OpContext &ctx) {
	const uint64_t size = r.Size();
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
		if (!r.ReadAt(pos, buf.data(), n)) {
			SetError(ctx, name + ": " + r.Error());
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
	case Format::CHD: {
		if (CdChdHoldsDvd(in)) {
			FileWriter w(iso);
			return DecompressToWriter(in, format, s, w, trimPbp, ctx);
		}
		return ChdResult(chdm_extract_raw(in.c_str(), iso.c_str(), &ChdProgress, &ctx), ctx);
	}
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
		std::unique_ptr<CdDataReader> dvd;
		if (CdChdHoldsDvd(in, &dvd))  // a DVD's image in a CD CHD: its sectors' user data
			return CopyImage(*dvd, in, w, ctx);
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
		FileImageReader r;
		if (!r.Open(in)) {
			SetError(ctx, in + ": " + r.Error());
			return false;
		}
		return CopyImage(r, in, w, ctx);
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

// " (Disc 2)" (or "Disk", "CD", "Disc 2 of 3") in a name: the disc number, 0 when there's none,
// and where the digit is.
int DiscNumberOf(const std::string &name, size_t *digitAt) {
	std::string low = ToLower(name);
	for (const char *word : { "(disc ", "(disk ", "(cd " }) {
		size_t p = low.rfind(word);
		if (p == std::string::npos)
			continue;
		size_t d = p + strlen(word);
		if (d < low.size() && low[d] >= '1' && low[d] <= '9' && (d + 1 >= low.size() || low[d + 1] == ')' || low[d + 1] == ' ')) {
			if (digitAt)
				*digitAt = d;
			return low[d] - '0';
		}
	}
	return 0;
}

// The same name for disc n.
static std::string DiscName(const std::string &name, size_t digitAt, int n) {
	std::string s = name;
	s[digitAt] = (char)('0' + n);
	return s;
}

// The name without " (Disc 1)".
static std::string WithoutDisc(const std::string &name, size_t digitAt) {
	size_t open = name.rfind('(', digitAt);
	size_t close = name.find(')', digitAt);
	if (open == std::string::npos || close == std::string::npos)
		return name;
	size_t from = open > 0 && name[open - 1] == ' ' ? open - 1 : open;
	return Trim(name.substr(0, from) + name.substr(close + 1));
}

// Disc 2..5 of the game whose disc 1 is path, next to it (same extension).
static std::vector<std::string> OtherDiscs(const std::string &path) {
	std::vector<std::string> out;
	fs::path p = U8Path(path);
	std::string stem = PathU8(p.stem());
	size_t at = 0;
	if (DiscNumberOf(stem, &at) != 1)
		return out;
	std::error_code ec;
	for (int n = 2; n <= 5; ++n) {
		fs::path q = p.parent_path() / U8Path(DiscName(stem, at, n) + PathU8(p.extension()));
		if (!fs::is_regular_file(q, ec))
			break;
		out.push_back(PathU8(q));
	}
	return out;
}

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
	// A game's discs in one PSOne EBOOT: named without " (Disc 1)".
	size_t at = 0;
	if (target == Format::PSX && s.psx.groupDiscs && DiscNumberOf(name, &at) == 1 && !OtherDiscs(in.path).empty())
		name = WithoutDisc(name, at);
	if ((target == Format::PBP || target == Format::PSX) && s.pbp.layout == PbpLayout::Folder)
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

// ---- CD images ---------------------------------------------------------------------------------

std::string Msf(uint32_t f) {
	return Fmt("%02u:%02u:%02u", f / 4500, (f / 75) % 60, f % 75);
}

bool ParseMsf(const std::string &t, uint32_t *f) {
	unsigned m, sec, fr;
	if (sscanf(t.c_str(), "%u:%u:%u", &m, &sec, &fr) != 3)
		return false;
	*f = (m * 60 + sec) * 75 + fr;
	return true;
}

// The tracks of a single-.bin cue sheet as CHDmodern writes them. false (with *why) when a track
// isn't raw 2352-byte sectors.
bool TracksOfCue(const std::string &cuePath, std::vector<ipbp_psx_track> *tracks, std::string *why) {
	std::vector<uint8_t> raw;
	File f;
	if (!f.Open(cuePath, File::Read)) {
		*why = f.LastError();
		return false;
	}
	raw.resize((size_t)f.Size());
	f.ReadExact(0, raw.data(), raw.size());
	std::string text(raw.begin(), raw.end());
	size_t pos = 0;
	bool have00 = false;
	while (pos < text.size()) {
		size_t e = text.find('\n', pos);
		std::string line = Trim(text.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
		pos = e == std::string::npos ? text.size() : e + 1;
		char a[32] = {}, b[32] = {};
		unsigned n = 0;
		if (sscanf(line.c_str(), "TRACK %u %31s", &n, a) == 2) {
			std::string type = a;
			if (type != "AUDIO" && type != "MODE1/2352" && type != "MODE2/2352") {
				*why = Fmt("PSOne EBOOTs need raw 2352-byte sectors, track %u is %s", n, type.c_str());
				return false;
			}
			ipbp_psx_track t{};
			t.audio = type == "AUDIO";
			tracks->push_back(t);
			have00 = false;
		} else if (sscanf(line.c_str(), "INDEX %u %31s", &n, b) == 2 && !tracks->empty()) {
			uint32_t fr;
			if (!ParseMsf(b, &fr))
				continue;
			if (n == 0) {
				tracks->back().start = fr;
				have00 = true;
			} else if (n == 1) {
				tracks->back().index1 = fr;
				if (!have00)
					tracks->back().start = fr;
			}
		}
	}
	if (tracks->empty()) {
		*why = "no tracks in " + cuePath;
		return false;
	}
	return true;
}

// A CD image or CD CHD as one raw .bin with every track, in dir: stem.bin + stem.cue.
bool CdToRawBin(const std::string &src, Format fmt, const fs::path &dir, const std::string &stem, std::string *bin,
	std::vector<ipbp_psx_track> *tracks, OpContext &ctx) {
	std::string chd = src, tempChd;
	auto base = ctx.progress;
	OpContext sub;
	sub.cancel = ctx.cancel;
	if (fmt != Format::CHD) {
		tempChd = PathU8(dir / U8Path(stem + ".chd"));
		static const uint32_t none[4] = {};
		sub.progress = [&](double f) { if (base) base(0.5 * f); };
		if (!ChdResult(chdm_create_cd(src.c_str(), tempChd.c_str(), 0, none, ChdProgress, &sub), sub)) {
			ctx.cancelled = sub.cancelled;
			SetError(ctx, sub.error);
			RemoveFile(tempChd);
			return false;
		}
		chd = tempChd;
	}
	const std::string cue = PathU8(dir / U8Path(stem + ".cue"));
	sub.progress = [&](double f) { if (base) base(tempChd.empty() ? f : 0.5 + 0.5 * f); };
	const bool ok = ChdResult(chdm_extract_cd(chd.c_str(), cue.c_str(), 0, ChdProgress, &sub), sub);
	if (!tempChd.empty())
		RemoveFile(tempChd);
	if (!ok) {
		ctx.cancelled = sub.cancelled;
		SetError(ctx, sub.error);
		return false;
	}
	std::string why;
	if (!TracksOfCue(cue, tracks, &why)) {
		SetError(ctx, why);
		return false;
	}
	*bin = PathU8(dir / U8Path(stem + ".bin"));
	return true;
}

// Disc d of a PSOne EBOOT as stem.bin + stem.cue in dir.
bool PsxDiscToBin(const std::string &pbp, unsigned d, const ipbp_psx_disc_info &di, const fs::path &dir,
	const std::string &stem, std::string *cuePath, OpContext &ctx) {
	const std::string bin = PathU8(dir / U8Path(stem + ".bin"));
	FileWriter fw(bin);
	PbpJob pj(ctx);
	ipbp_writer w{ &fw, PbpBegin, PbpWrite, PbpFinish };
	if (!pj.Result(ipbp_pbp_to_psx(pj.job, pbp.c_str(), d, &w), &fw))
		return false;
	std::string text = "FILE \"" + stem + ".bin\" BINARY\n";
	for (uint32_t t = 0; t < di.tracks; ++t) {
		const ipbp_psx_track &tr = di.track[t];
		text += Fmt("  TRACK %02u %s\n", t + 1, tr.audio ? "AUDIO" : di.data_mode[t] == 1 ? "MODE1/2352" : "MODE2/2352");
		if (tr.start < tr.index1)
			text += "    INDEX 00 " + Msf(tr.start) + "\n";
		text += "    INDEX 01 " + Msf(tr.index1) + "\n";
	}
	*cuePath = PathU8(dir / U8Path(stem + ".cue"));
	File f;
	if (!f.Open(*cuePath, File::Write) || !f.WriteAt(0, text.data(), text.size())) {
		SetError(ctx, *cuePath + ": " + f.LastError());
		return false;
	}
	return true;
}

// CD images and CD CHDs, and PSOne EBOOTs: CD image -> CD CHD, CD CHD -> CD CHD (other codecs),
// any of them -> cue/bin, and to / from PSOne EBOOTs. They don't go through a plain 2048-byte
// image: CHDmodern and iPBP work file to file, with raw .bin files in a temporary folder between.
void RunCdJob(Job &job, Format S, Format T, const ImageInfo &info, const std::string &final, const std::string &part,
	bool replacingSource, const std::vector<fs::path> &createdDirs, JobResult &res,
	const std::function<void(JobState, const std::string &)> &finish) {
	const Settings &s = job.settings;
	const InputFile &in = job.input;
	std::error_code ec;
	res.isoSize = info.isoSize;
	const bool toCue = T == Format::CUE, toPsx = T == Format::PSX, fromPsx = S == Format::PSX;

	Plan plan;
	if (fromPsx || toPsx || toCue)
		plan.phases.push_back({ Phase::Extracting, toCue ? 1.0 : 0.4 });
	if (!toCue)
		plan.phases.push_back({ Phase::Compressing, 1.0 });
	if (s.engine.verify)
		plan.phases.push_back({ Phase::Verifying, 0.4 });
	OpContext ctx;
	ctx.cancel = &job.cancel;
	Phase cur = Phase::Idle;
	size_t discIndex = 0, discCount = 1;
	ctx.progress = [&](double f) {
		job.phaseProgress = f;
		job.overall = plan.Overall(cur, (discIndex + std::min(1.0, f)) / (double)discCount);
	};
	auto enter = [&](Phase p) {
		cur = p;
		job.phase = (int)p;
		job.phaseProgress = 0.0;
		job.overall = plan.Overall(p, (double)discIndex / (double)discCount);
	};

	// The source discs: the image itself, its other discs (into one PSOne EBOOT), or an EBOOT's discs.
	struct Disc {
		std::string path;  // CD image / CHD / temporary cue sheet
		Format format;
		std::string bin;   // raw .bin (for PSOne EBOOTs)
		std::vector<ipbp_psx_track> tracks;
	};
	std::vector<Disc> discs;
	std::vector<std::string> sources;  // for "delete source", listed before anything changes
	auto addSource = [&](const std::string &path, Format f) {
		discs.push_back({ path, f, {}, {} });
		std::vector<std::string> files;
		if (f != Format::PSX)
			chdm_cd_image_files(path.c_str(), [](void *u, const char *p) { ((std::vector<std::string> *)u)->push_back(p); }, &files);
		if (files.empty())
			files.push_back(path);
		sources.insert(sources.end(), files.begin(), files.end());
	};
	addSource(in.path, S);
	if (toPsx && !fromPsx && s.psx.groupDiscs)
		for (const std::string &o : OtherDiscs(in.path))
			addSource(o, DetectFormat(o));

	const fs::path dest = U8Path(final).parent_path();
	const fs::path tempDir = dest / U8Path("." + PathU8(U8Path(final).stem()) + ".isopk-temp");
	std::vector<std::string> outputs;
	auto cleanup = [&] {
		RemoveFile(part);
		fs::remove_all(tempDir, ec);
		for (const fs::path &d : createdDirs)
			if (fs::is_empty(d, ec))
				fs::remove(d, ec);
	};
	auto fail = [&](const std::string &what) {
		for (const std::string &o : outputs)
			RemoveFile(o);
		cleanup();
		if (ctx.cancelled || job.cancel.load())
			return finish(JobState::Cancelled, "cancelled");
		return finish(JobState::Failed, what + ": " + ctx.error);
	};
	fs::remove_all(tempDir, ec);
	fs::create_directories(tempDir, ec);
	if (ec) {
		ctx.error = ec.message();
		return fail("can't create " + PathU8(tempDir));
	}

	Log(LogLevel::Info, Fmt("Start: %s (%s, %s) -> %s%s", in.path.c_str(), FormatName(S), info.detail.c_str(), FormatName(T),
		discs.size() > 1 ? Fmt(", %zu discs", discs.size()).c_str() : ""));

	// 1. A PSOne EBOOT's discs, as cue/bin.
	if (fromPsx) {
		ipbp_job *pj = ipbp_job_create();
		ipbp_psx_info pi;
		const int rc = ipbp_read_psx_info(pj, in.path.c_str(), &pi);
		ctx.error = ipbp_job_error(pj);
		ipbp_job_destroy(pj);
		if (rc != IPBP_OK)
			return fail("reading the EBOOT");
		discs.clear();
		discCount = pi.discs;
		Clock::time_point tx = Clock::now();
		enter(Phase::Extracting);
		for (uint32_t d = 0; d < pi.discs; ++d) {
			discIndex = d;
			const std::string stem = Fmt("disc%u", d + 1);
			Disc disc{ {}, Format::CUE, PathU8(tempDir / U8Path(stem + ".bin")), {} };
			if (!PsxDiscToBin(in.path, d, pi.disc[d], tempDir, stem, &disc.path, ctx))
				return fail("extracting");
			for (uint32_t t = 0; t < pi.disc[d].tracks; ++t)
				disc.tracks.push_back(pi.disc[d].track[t]);
			discs.push_back(disc);
		}
		res.extractSecs = SecondsSince(tx);
		discIndex = 0;
	}
	discCount = discs.size();

	uint32_t list[4] = {};
	const uint32_t *codecs = nullptr;
	std::vector<uint32_t> cd = CdCodecsFor(s.chd);
	if (!cd.empty()) {
		for (size_t i = 0; i < 4 && i < cd.size(); ++i)
			list[i] = cd[i];
		codecs = list;
	}
	auto putInPlace = [&](const std::string &from, const std::string &to) {
		if (fs::exists(U8Path(to), ec))
			RemoveFile(to);
		fs::rename(U8Path(from), U8Path(to), ec);
		if (ec) {
			ctx.error = ec.message();
			return false;
		}
		outputs.push_back(to);
		return true;
	};

	if (toPsx) {
		// 2a. Raw .bin files of the discs, then the EBOOT.
		Clock::time_point tx = Clock::now();
		enter(Phase::Extracting);
		for (size_t i = 0; i < discs.size(); ++i) {
			discIndex = i;
			if (discs[i].bin.empty() || discs[i].tracks.empty()) {
				discs[i].tracks.clear();
				if (!CdToRawBin(discs[i].path, discs[i].format, tempDir, Fmt("raw%zu", i + 1), &discs[i].bin, &discs[i].tracks, ctx))
					return fail("reading " + PathU8(U8Path(discs[i].path).filename()));
			}
		}
		res.extractSecs += SecondsSince(tx);
		std::vector<std::unique_ptr<FileImageReader>> readers;
		std::vector<std::unique_ptr<ReaderBox>> boxes;
		std::vector<ipbp_psx_disc> pd(discs.size());
		for (size_t i = 0; i < discs.size(); ++i) {
			readers.emplace_back(new FileImageReader());
			if (!readers.back()->Open(discs[i].bin)) {
				ctx.error = readers.back()->Error();
				return fail("reading the disc image");
			}
			boxes.emplace_back(new ReaderBox(readers.back().get()));
			pd[i].image = ipbp_reader{ boxes.back().get(), readers.back()->Size(), PbpRead };
			pd[i].tracks = (uint32_t)discs[i].tracks.size();
			pd[i].track = discs[i].tracks.data();
		}
		ipbp_psx_options o;
		ipbp_psx_options_init(&o);
		o.level = s.psx.level;
		const std::string title = GameName(final);
		o.title = title.c_str();
		o.threads = s.engine.threads;
		if (!s.psx.basePbp.empty())
			o.base_pbp = s.psx.basePbp.c_str();
		std::vector<uint8_t> boot;
		if (!s.psx.bootPng.empty()) {
			std::string err;
			if (!LoadFileBytes(s.psx.bootPng, &boot, &err)) {
				ctx.error = err;
				return fail("the boot screen");
			}
			o.boot_png = boot.data();
			o.boot_png_size = boot.size();
		}
		Clock::time_point tc = Clock::now();
		discIndex = 0;
		discCount = 1;
		enter(Phase::Compressing);
		{
			PbpJob pj(ctx);
			if (!pj.Result(ipbp_psx_to_pbp(pj.job, pd.data(), (unsigned)pd.size(), part.c_str(), &o)))
				return fail("compressing");
		}
		res.compressSecs = SecondsSince(tc);
		if (s.engine.verify) {
			Clock::time_point tv = Clock::now();
			discCount = discs.size();
			enter(Phase::Verifying);
			for (size_t i = 0; i < discs.size(); ++i) {
				discIndex = i;
				CompareWriter cmp(readers[i].get());
				PbpJob pj(ctx);
				ipbp_writer w{ &cmp, PbpBegin, PbpWrite, PbpFinish };
				bool ok = pj.Result(ipbp_pbp_to_psx(pj.job, part.c_str(), (unsigned)i, &w), &cmp) && cmp.Matched();
				if (!ok) {
					if (ctx.error.empty())
						ctx.error = Fmt("disc %zu: %s", i + 1, cmp.Result().c_str());
					res.verify = ctx.error;
					return fail("verify failed");
				}
			}
			res.verifySecs = SecondsSince(tv);
		}
		readers.clear();
		enter(Phase::Finishing);
		if (replacingSource)
			RemoveFile(final);
		if (!putInPlace(part, final))
			return fail("can't rename the output");
	} else {
		// 2b. Each disc to a CD CHD or cue/bin: "Game.chd", or "Game (Disc 1).chd" ... for several.
		const std::string stem = PathU8(U8Path(final).stem()), ext = PathU8(U8Path(final).extension());
		for (size_t i = 0; i < discs.size(); ++i) {
			discIndex = i;
			const Disc &d = discs[i];
			const std::string out = discs.size() == 1 ? final : PathU8(dest / U8Path(Fmt("%s (Disc %zu)%s", stem.c_str(), i + 1, ext.c_str())));
			if (!toCue) {
				const std::string dpart = out + ".part";
				Clock::time_point tc = Clock::now();
				enter(Phase::Compressing);
				if (!ChdResult(chdm_create_cd(d.path.c_str(), dpart.c_str(), 0, codecs, ChdProgress, &ctx), ctx)) {
					RemoveFile(dpart);
					return fail("compressing");
				}
				res.compressSecs += SecondsSince(tc);
				if (s.engine.verify) {
					Clock::time_point tv = Clock::now();
					enter(Phase::Verifying);
					if (!ChdResult(chdm_verify_cd(d.path.c_str(), dpart.c_str(), ChdProgress, &ctx), ctx)) {
						RemoveFile(dpart);
						res.verify = ctx.error;
						return fail("verify failed");
					}
					res.verifySecs += SecondsSince(tv);
				}
				enter(Phase::Finishing);
				if (!putInPlace(dpart, out)) {
					RemoveFile(dpart);
					return fail("can't rename the output");
				}
				continue;
			}
			// cue/bin: CHDmodern writes it from a CHD, so other images go through an uncompressed one.
			std::string chd = d.path;
			const std::string tempChd = PathU8(tempDir / U8Path(Fmt("disc%zu.isopk-temp.chd", i + 1)));
			if (d.format != Format::CHD) {
				enter(Phase::Extracting);
				static const uint32_t none[4] = {};
				if (!ChdResult(chdm_create_cd(d.path.c_str(), tempChd.c_str(), 0, none, ChdProgress, &ctx), ctx))
					return fail("reading the CD image");
				chd = tempChd;
			}
			const fs::path cueDir = tempDir / U8Path(Fmt("out%zu", i + 1));
			fs::create_directories(cueDir, ec);
			const std::string cue = PathU8(cueDir / U8Path(out).filename());
			Clock::time_point tx = Clock::now();
			enter(Phase::Extracting);
			if (!ChdResult(chdm_extract_cd(chd.c_str(), cue.c_str(), s.cue.splitBins ? 1 : 0, ChdProgress, &ctx), ctx))
				return fail("extracting");
			res.extractSecs += SecondsSince(tx);
			if (s.engine.verify) {
				Clock::time_point tv = Clock::now();
				enter(Phase::Verifying);
				bool ok = ChdResult(chdm_verify_cd(cue.c_str(), chd.c_str(), ChdProgress, &ctx), ctx);
				if (ok && chd != d.path)
					ok = ChdResult(chdm_verify_cd(d.path.c_str(), chd.c_str(), ChdProgress, &ctx), ctx);
				if (!ok) {
					res.verify = ctx.error;
					return fail("verify failed");
				}
				res.verifySecs += SecondsSince(tv);
			}
			RemoveFile(tempChd);
			enter(Phase::Finishing);
			std::vector<fs::path> made;
			for (auto it = fs::directory_iterator(cueDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
				made.push_back(it->path());
			std::sort(made.begin(), made.end());
			for (const fs::path &f : made)
				if (!putInPlace(PathU8(f), PathU8(dest / f.filename())))
					return fail("can't move " + PathU8(f.filename()) + " into place");
		}
	}
	fs::remove_all(tempDir, ec);
	res.verify = s.engine.verify ? "OK" : "off";
	res.outputSize = 0;
	for (const std::string &o : outputs)
		res.outputSize += fs::file_size(U8Path(o), ec);
	if (discs.size() > 1)
		res.message = Fmt("%zu discs", discs.size());

	if (s.out.deleteSource && !replacingSource) {
		int removed = 0;
		for (const std::string &src : sources) {
			bool isOutput = false;
			for (const std::string &o : outputs)
				isOutput |= SamePath(src, o);
			if (!isOutput) {
				RemoveFile(src);
				++removed;
			}
		}
		// A PSOne EBOOT's game folder goes too once it's empty.
		fs::path parent = U8Path(in.path).parent_path();
		if (fromPsx && ToLower(PathU8(U8Path(in.path).filename())) == "eboot.pbp" && fs::is_empty(parent, ec))
			fs::remove(parent, ec);
		std::string msg = removed > 1 ? Fmt("source deleted (%d files)", removed) : "source deleted";
		res.message = res.message.empty() ? msg : res.message + ", " + msg;
	}
	finish(JobState::Done, "");
}

}  // namespace

std::string PlanZarOutputPath(const ZarSpec &spec, const Settings &s) {
	fs::path dir = s.out.dir.empty() ? U8Path(spec.SourceDir()) : U8Path(s.out.dir);
	std::string name = spec.name;
	// Characters no file system takes.
	for (char &c : name)
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			c = '_';
	if (name.empty())
		name = "archive";
	return PathU8(dir / U8Path(name + ".zar"));
}

namespace {

void RunZarJob(Job &job) {
	const Settings &s = job.settings;
	const ZarSpec &spec = *job.zar;
	const auto t0 = Clock::now();
	JobResult res;
	res.inputSize = spec.totalSize;
	res.isoSize = spec.totalSize;  // the ratio column: archive size against everything that went in
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
		const char *name = spec.name.c_str();
		switch (st) {
		case JobState::Done:
			Log(LogLevel::Info, Fmt("Done: %s -> %s (%s, %.1f%%, %s)%s; %s", name, res.outputPath.c_str(), HumanBytes(res.outputSize).c_str(),
				res.isoSize ? 100.0 * res.outputSize / res.isoSize : 0.0, HumanDuration(res.seconds).c_str(),
				res.verify == "OK" ? ", verified" : "", res.message.c_str()));
			break;
		case JobState::Skipped: Log(LogLevel::Info, Fmt("Skipped: %s: %s", name, res.message.c_str())); break;
		case JobState::Cancelled: Log(LogLevel::Warning, Fmt("Cancelled: %s", name)); break;
		case JobState::Failed: Log(LogLevel::Error, Fmt("Failed: %s: %s", name, res.message.c_str())); break;
		default: break;
		}
	};

	std::string final = PlanZarOutputPath(spec, s);
	std::error_code ec;
	OutputClaim claim;
	{
		std::lock_guard<std::mutex> g(ClaimMutex());
		auto taken = [&](const std::string &p) { return fs::exists(U8Path(p), ec) || Claimed().count(ClaimKey(p)) != 0; };
		if (taken(final)) {
			const bool busy = Claimed().count(ClaimKey(final)) != 0;
			if (s.out.conflict == Conflict::Skip && !busy)
				return finish(JobState::Skipped, "output exists: " + final);
			if (s.out.conflict == Conflict::Rename || busy)
				final = UniquePath(final, false, taken);
		}
		claim.Take(final);
	}
	res.outputPath = final;
	fs::create_directories(U8Path(final).parent_path(), ec);
	if (ec)
		return finish(JobState::Failed, "can't create " + PathU8(U8Path(final).parent_path()) + ": " + ec.message());
	const std::string part = final + ".part";
	RemoveFile(part);

	std::vector<ZarEntry> entries;
	std::string err;
	if (!PlanZarEntries(spec, final, &entries, &err))
		return finish(JobState::Failed, err);
	uint64_t freeSpace = FreeDiskSpace(final);
	if (freeSpace != UINT64_MAX && freeSpace < spec.totalSize / 2)
		Log(LogLevel::Warning, Fmt("%s: only %s free next to the output", spec.name.c_str(), HumanBytes(freeSpace).c_str()));

	Plan plan;
	plan.phases.push_back({ Phase::Archiving, 1.0 });
	if (s.engine.verify)
		plan.phases.push_back({ Phase::Verifying, 0.15 });
	OpContext ctx;
	ctx.cancel = &job.cancel;
	Phase cur = Phase::Archiving;
	auto enter = [&](Phase p) {
		cur = p;
		job.phase = (int)p;
		job.phaseProgress = 0.0;
		job.overall = plan.Overall(p, 0.0);
	};
	ctx.progress = [&](double f) {
		job.phaseProgress = f;
		job.overall = plan.Overall(cur, f);
	};
	auto fail = [&](const std::string &what) {
		RemoveFile(part);
		if (ctx.cancelled || job.cancel.load())
			return finish(JobState::Cancelled, "cancelled");
		return finish(JobState::Failed, what.empty() ? ctx.error : what + ": " + ctx.error);
	};

	Log(LogLevel::Info, Fmt("Start: ZArchive %s (%zu entries, %s) -> %s [%s]", spec.name.c_str(), entries.size(), HumanBytes(spec.totalSize).c_str(),
		final.c_str(), s.zar.Describe().c_str()));
	enter(Phase::Archiving);
	ZarProgress zp;
	// The phase label follows what's happening: a disc being compressed first, or archiving.
	zp.onPhase = [&](bool compressing, const std::string &) { job.phase = (int)(compressing ? Phase::Compressing : Phase::Archiving); };
	ZarReport report;
	Clock::time_point tc = Clock::now();
	if (!WriteZArchive(spec, entries, s, part, ctx, zp, &report))
		return fail("");
	res.compressSecs = SecondsSince(tc);
	res.extractSecs = report.compressDiscSecs;

	if (s.engine.verify) {
		Clock::time_point tv = Clock::now();
		enter(Phase::Verifying);
		std::string verdict;
		if (!VerifyZArchive(part, report.written, ctx, &verdict)) {
			res.verify = verdict.empty() ? ctx.error : verdict;
			if (!verdict.empty())
				ctx.error = verdict;
			return fail("verify failed");
		}
		res.verifySecs = SecondsSince(tv);
		res.verify = "OK";
	} else {
		res.verify = "off";
	}

	enter(Phase::Finishing);
	if (fs::exists(U8Path(final), ec))
		RemoveFile(final);
	fs::rename(U8Path(part), U8Path(final), ec);
	if (ec) {
		RemoveFile(part);
		return finish(JobState::Failed, "can't rename the output: " + ec.message());
	}
	res.outputSize = fs::file_size(U8Path(final), ec);
	res.message = report.Summary();
	finish(JobState::Done, "");
}

// A .zar in the queue: extracted into a folder named after it, next to it.
void RunZarExtractJob(Job &job) {
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
		switch (st) {
		case JobState::Done:
			Log(LogLevel::Info, Fmt("Done: %s -> %s (%s)%s", in.path.c_str(), res.outputPath.c_str(), res.message.c_str(),
				res.verify == "OK" ? ", verified" : ""));
			break;
		case JobState::Skipped: Log(LogLevel::Info, Fmt("Skipped: %s: %s", in.path.c_str(), res.message.c_str())); break;
		case JobState::Cancelled: Log(LogLevel::Warning, Fmt("Cancelled: %s", in.path.c_str())); break;
		case JobState::Failed: Log(LogLevel::Error, Fmt("Failed: %s: %s", in.path.c_str(), res.message.c_str())); break;
		default: break;
		}
	};

	uint64_t files = 0, bytes = 0;
	std::string err;
	if (!ZArchiveContents(in.path, &files, &bytes, &err))
		return finish(JobState::Failed, err);
	res.isoSize = bytes;

	// Always next to the archive: Game.zar -> Game/
	fs::path src = U8Path(in.path);
	std::string final = PathU8(src.parent_path() / src.stem());
	std::error_code ec;
	bool intoExisting = false;
	OutputClaim claim;
	{
		std::lock_guard<std::mutex> g(ClaimMutex());
		auto taken = [&](const std::string &p) { return fs::exists(U8Path(p), ec) || Claimed().count(ClaimKey(p)) != 0; };
		if (taken(final)) {
			const bool busy = Claimed().count(ClaimKey(final)) != 0;
			const bool isDir = fs::is_directory(U8Path(final), ec);
			if (s.out.conflict == Conflict::Skip && !busy)
				return finish(JobState::Skipped, "folder exists: " + final);
			if (s.out.conflict == Conflict::Overwrite && !busy && isDir) {
				intoExisting = true;  // files of the same name are replaced, others kept
			} else {
				const std::string stem = final;
				for (int i = 2; i < 10000 && taken(final); ++i)
					final = stem + Fmt(" (%d)", i);
			}
		}
		claim.Take(final);
	}
	res.outputPath = final;
	const std::string work = intoExisting ? final : final + ".isopk-part";
	auto removeWork = [&] {
		if (!intoExisting)
			fs::remove_all(U8Path(work), ec);
	};
	removeWork();
	uint64_t freeSpace = FreeDiskSpace(in.path);
	if (freeSpace != UINT64_MAX && freeSpace < bytes)
		Log(LogLevel::Warning, Fmt("%s: needs %s, only %s free", in.path.c_str(), HumanBytes(bytes).c_str(), HumanBytes(freeSpace).c_str()));

	Plan plan;
	plan.phases.push_back({ Phase::Extracting, 1.0 });
	if (s.engine.verify)
		plan.phases.push_back({ Phase::Verifying, 0.3 });
	OpContext ctx;
	ctx.cancel = &job.cancel;
	Phase cur = Phase::Extracting;
	auto enter = [&](Phase p) {
		cur = p;
		job.phase = (int)p;
		job.phaseProgress = 0.0;
		job.overall = plan.Overall(p, 0.0);
	};
	ctx.progress = [&](double f) {
		job.phaseProgress = f;
		job.overall = plan.Overall(cur, f);
	};
	auto fail = [&](const std::string &what) {
		removeWork();
		if (ctx.cancelled || job.cancel.load())
			return finish(JobState::Cancelled, "cancelled");
		return finish(JobState::Failed, what.empty() ? ctx.error : what + ": " + ctx.error);
	};

	Log(LogLevel::Info, Fmt("Start: %s (ZAR, %llu files, %s) -> %s", in.path.c_str(), (unsigned long long)files, HumanBytes(bytes).c_str(), final.c_str()));
	enter(Phase::Extracting);
	std::vector<ZarWritten> written;
	Clock::time_point tx = Clock::now();
	if (!ExtractZArchive(in.path, work, ctx, &written))
		return fail("extracting");
	res.extractSecs = SecondsSince(tx);
	if (s.engine.verify) {
		Clock::time_point tv = Clock::now();
		enter(Phase::Verifying);
		std::string verdict;
		if (!VerifyExtractedZArchive(work, written, ctx, &verdict)) {
			res.verify = verdict.empty() ? ctx.error : verdict;
			if (!verdict.empty())
				ctx.error = verdict;
			return fail("verify failed");
		}
		res.verifySecs = SecondsSince(tv);
		res.verify = "OK";
	} else {
		res.verify = "off";
	}
	enter(Phase::Finishing);
	if (!intoExisting) {
		fs::rename(U8Path(work), U8Path(final), ec);
		if (ec) {
			removeWork();
			return finish(JobState::Failed, "can't rename the output folder: " + ec.message());
		}
	}
	res.outputSize = bytes;
	res.message = Fmt("%llu file%s", (unsigned long long)files, files == 1 ? "" : "s");
	if (s.out.deleteSource && (res.verify == "OK" || !s.engine.verify)) {
		RemoveFile(in.path);
		res.message += ", source deleted";
	}
	finish(JobState::Done, "");
}

}  // namespace

void RunJob(Job &job) {
	if (job.zar)
		return RunZarJob(job);
	if (job.input.format == Format::ZAR || (job.input.format == Format::Unknown && DetectFormat(job.input.path) == Format::ZAR))
		return RunZarExtractJob(job);
	if (job.target.load() == Format::ZAR) {
		std::lock_guard<std::mutex> g(job.resultMutex);
		job.result.state = JobState::Failed;
		job.result.message = "ZArchives are made with Create ZArchive (CLI: the zar command)";
		job.state = (int)JobState::Failed;
		Log(LogLevel::Error, "Failed: " + job.input.path + ": " + job.result.message);
		return;
	}
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
	Format T = job.target.load();
	if (S == Format::Unknown)
		return finish(JobState::Failed, "not a recognised disc image");
	if (!FormatAvailable(S) || !FormatAvailable(T))
		return finish(JobState::Failed, "format not supported by this build");
	if (S == Format::ISO && T == Format::ISO)
		return finish(JobState::Skipped, "already an ISO");
	// Formats that only hold some platforms' discs (RVZ/WIA: GameCube and Wii; PBP: PSP).
	ImageInfo info = InspectImage(in.path);
	if (IsCdImageFormat(S) && !info.error.empty())
		return finish(JobState::Failed, info.error);
	// A batch for CSO / PBP / RVZ with CD images in it: those become CD CHDs (cue/bin when
	// extracting, PSOne EBOOTs in a PBP batch).
	if (info.platform == Platform::CD && T != Format::CHD && T != Format::CUE && T != Format::PSX) {
		const Format to = T == Format::ISO ? Format::CUE : T == Format::PBP ? Format::PSX : Format::CHD;
		Log(LogLevel::Info, Fmt("%s: a CD image, making %s instead of %s", in.path.c_str(), FormatName(to), FormatName(T)));
		T = to;
		job.target = T;
	}
	if (std::string why = TargetRejects(T, info.platform); !why.empty())
		return finish(JobState::Skipped, why);
	// Discs 2-5 of a game go into the EBOOT made from disc 1.
	if (T == Format::PSX && S != Format::PSX && s.psx.groupDiscs) {
		const std::string stem = PathU8(U8Path(in.path).stem());
		size_t at = 0;
		const int n = DiscNumberOf(stem, &at);
		const fs::path first = U8Path(in.path).parent_path() / U8Path(DiscName(stem, at, 1) + PathU8(U8Path(in.path).extension()));
		std::error_code ec1;
		if (n > 1 && fs::is_regular_file(first, ec1))
			return finish(JobState::Skipped, "goes into the EBOOT made from " + PathU8(first.filename()));
	}
	if (T == Format::CUE && info.platform != Platform::CD)
		return finish(JobState::Skipped, info.error.empty() ? "CUE/BIN only holds CD images" : info.error);

	// Where it goes.
	std::string final = PlanOutputPath(in, T, s);
	const bool pbpFolder = (T == Format::PBP || T == Format::PSX) && s.pbp.layout == PbpLayout::Folder;
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

	if (info.platform == Platform::CD)
		return RunCdJob(job, S, T, info, final, part, replacingSource, createdDirs, res, finish);

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
