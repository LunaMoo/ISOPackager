// A queue of conversion jobs run by a pool of worker threads (several files at once, each of them
// also multithreaded inside its library).
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/engine.h"

namespace isopk {

class JobQueue {
public:
	JobQueue();
	~JobQueue();

	// Settings used by jobs from the moment they start (each keeps its own target).
	void SetSettings(const Settings &s);
	Settings CurrentSettings() const;

	std::shared_ptr<Job> Add(const InputFile &input, Format target);
	// A "Create ZArchive" job (spec.totalSize should be measured already).
	std::shared_ptr<Job> AddZArchive(const ZarSpec &spec);
	void Remove(const std::shared_ptr<Job> &job);  // cancels it if running
	void ClearFinished();
	void ClearAll();
	// Puts failed / cancelled jobs back in the queue.
	void Retry(const std::shared_ptr<Job> &job);

	void Start();   // runs queued jobs until the queue is empty
	void Pause();   // running jobs finish, no new ones start
	void CancelAll();
	bool Running() const { return running_.load(); }
	// True while jobs are running or (when started) queued.
	bool Busy() const;
	// Blocks until nothing is running or queued (or the queue is paused and idle).
	void WaitIdle();

	std::vector<std::shared_ptr<Job>> Jobs() const;
	struct Counts {
		int queued = 0, running = 0, done = 0, skipped = 0, failed = 0, cancelled = 0;
		uint64_t inBytes = 0, outBytes = 0;
	};
	Counts Count() const;

private:
	void Worker();
	void EnsureWorkers(unsigned n);

	mutable std::mutex mutex_;
	std::condition_variable cv_, idleCv_;
	std::vector<std::shared_ptr<Job>> jobs_;
	std::vector<std::thread> workers_;
	Settings settings_;
	std::atomic<bool> running_{ false }, stop_{ false };
	int active_ = 0;
};

}  // namespace isopk
