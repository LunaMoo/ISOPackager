// The conversion engine: every compress / extract / repack goes through RunJob, which drives mCSO,
// CHDmodern and iPBP through their file or callback (RAM) interfaces.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "core/formats.h"
#include "core/image.h"
#include "core/scan.h"
#include "core/settings.h"

namespace isopk {

// ---- Logging ----------------------------------------------------------------------------------

enum class LogLevel { Debug, Info, Warning, Error };
using LogSink = std::function<void(LogLevel, const std::string &)>;
void SetLogSink(LogSink sink);
void Log(LogLevel level, const std::string &msg);

// Applies the process-wide parts of the settings (thread pool size, RAM budget, priority).
// Call before the first conversion, and again when the settings change.
void ApplyEngineSettings(const EngineSettings &e);

// Library versions, for the about box and the benchmark header.
std::string LibraryVersions();

// ---- Single library operations ----------------------------------------------------------------

struct OpContext {
	const std::atomic<bool> *cancel = nullptr;
	std::function<void(double)> progress;  // 0..1 within this operation
	std::string error;
	bool cancelled = false;

	bool Cancelled() const { return cancel && cancel->load(); }
};

// ISO file -> format (path interfaces of the libraries).
bool CompressFile(const std::string &isoPath, Format target, const Settings &s, const std::string &outPath, OpContext &ctx);
// ISO in RAM (or any thread-safe reader) -> format (callback interfaces).
bool CompressImage(ImageReader &iso, Format target, const Settings &s, const std::string &outPath, OpContext &ctx);
// Image -> ISO file.
bool DecompressToFile(const std::string &inPath, Format format, const Settings &s, const std::string &isoPath, bool trimPbp, OpContext &ctx);
// Image -> writer (RAM, comparison, ...). An ISO input is streamed through as it is.
bool DecompressToWriter(const std::string &inPath, Format format, const Settings &s, ImageWriter &out, bool trimPbp, OpContext &ctx);

// ---- Jobs -------------------------------------------------------------------------------------

enum class JobState { Queued, Running, Done, Skipped, Failed, Cancelled };
const char *JobStateName(JobState s);

enum class Phase { Idle, WaitingRam, WaitingCso, LoadingRam, Extracting, Compressing, Verifying, Finishing };
const char *PhaseName(Phase p);

struct JobResult {
	JobState state = JobState::Queued;
	std::string message;     // error or skip reason, or a short success note
	std::string outputPath;
	uint64_t inputSize = 0, outputSize = 0, isoSize = 0;
	double seconds = 0, extractSecs = 0, compressSecs = 0, verifySecs = 0;
	std::string verify = "-";  // "OK", "off", or what differed
	bool viaRam = false, viaTempFile = false;
};

struct Job {
	InputFile input;
	std::atomic<Format> target{ Format::CSO };  // may be changed by the UI while queued
	Settings settings;  // snapshot taken when the job starts

	std::atomic<bool> cancel{ false };
	std::atomic<bool> removeWhenDone{ false };  // JobQueue: removed while running
	std::atomic<int> phase{ (int)Phase::Idle };
	std::atomic<double> phaseProgress{ 0.0 }, overall{ 0.0 };
	std::atomic<int> state{ (int)JobState::Queued };

	mutable std::mutex resultMutex;
	JobResult result;  // guarded by resultMutex while running

	JobState State() const { return (JobState)state.load(); }
	Phase CurrentPhase() const { return (Phase)phase.load(); }
	JobResult Result() const {
		std::lock_guard<std::mutex> g(resultMutex);
		return result;
	}
};

// Where the output of converting input to target goes, before conflict handling.
std::string PlanOutputPath(const InputFile &input, Format target, const Settings &s);

// Runs one job to the end on the calling thread. Thread-safe: several jobs may run at once.
void RunJob(Job &job);

// Serialises mCSO calls: its global buffer pool can't serve two tasks with different block sizes.
std::mutex &McsoLane();

}  // namespace isopk
