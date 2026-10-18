#include "core/queue.h"

#include <algorithm>

namespace isopk {

JobQueue::JobQueue() = default;

JobQueue::~JobQueue() {
	CancelAll();
	{
		std::lock_guard<std::mutex> g(mutex_);
		stop_ = true;
	}
	cv_.notify_all();
	for (std::thread &t : workers_)
		if (t.joinable())
			t.join();
}

void JobQueue::SetSettings(const Settings &s) {
	{
		std::lock_guard<std::mutex> g(mutex_);
		settings_ = s;
	}
	ApplyEngineSettings(s.engine);
	EnsureWorkers(std::max(1u, s.engine.jobs));
	cv_.notify_all();
}

Settings JobQueue::CurrentSettings() const {
	std::lock_guard<std::mutex> g(mutex_);
	return settings_;
}

std::shared_ptr<Job> JobQueue::Add(const InputFile &input, Format target) {
	auto job = std::make_shared<Job>();
	job->input = input;
	job->target = target;
	{
		std::lock_guard<std::mutex> g(mutex_);
		job->settings = settings_;
		jobs_.push_back(job);
	}
	cv_.notify_all();
	return job;
}

std::shared_ptr<Job> JobQueue::AddZArchive(const ZarSpec &spec) {
	auto job = std::make_shared<Job>();
	job->zar = std::make_shared<const ZarSpec>(spec);
	std::vector<std::string> paths = spec.Paths();
	job->input.path = paths.empty() ? spec.name : paths.front();
	job->input.size = spec.totalSize;
	job->input.format = Format::Unknown;
	job->target = Format::ZAR;
	{
		std::lock_guard<std::mutex> g(mutex_);
		job->settings = settings_;
		jobs_.push_back(job);
	}
	cv_.notify_all();
	return job;
}

void JobQueue::Remove(const std::shared_ptr<Job> &job) {
	job->cancel = true;
	std::lock_guard<std::mutex> g(mutex_);
	// A running job stays listed until its worker is done with it.
	if (job->State() == JobState::Running)
		job->removeWhenDone = true;
	else
		jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), job), jobs_.end());
}

void JobQueue::ClearFinished() {
	std::lock_guard<std::mutex> g(mutex_);
	jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
					[](const std::shared_ptr<Job> &j) {
						JobState s = j->State();
						return s == JobState::Done || s == JobState::Skipped || s == JobState::Failed || s == JobState::Cancelled;
					}),
		jobs_.end());
}

void JobQueue::ClearAll() {
	std::lock_guard<std::mutex> g(mutex_);
	for (auto &j : jobs_) {
		j->cancel = true;
		if (j->State() == JobState::Running)
			j->removeWhenDone = true;
	}
	jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const std::shared_ptr<Job> &j) { return j->State() != JobState::Running; }),
		jobs_.end());
}

void JobQueue::Retry(const std::shared_ptr<Job> &job) {
	{
		std::lock_guard<std::mutex> g(mutex_);
		JobState s = job->State();
		if (s == JobState::Running || s == JobState::Queued)
			return;
		job->cancel = false;
		job->overall = 0.0;
		job->phaseProgress = 0.0;
		{
			std::lock_guard<std::mutex> rg(job->resultMutex);
			job->result = JobResult();
		}
		job->state = (int)JobState::Queued;
	}
	cv_.notify_all();
}

void JobQueue::Start() {
	{
		std::lock_guard<std::mutex> g(mutex_);
		for (auto &j : jobs_)
			if (j->State() == JobState::Queued)
				j->cancel = false;
		running_ = true;
		EnsureWorkers(std::max(1u, settings_.engine.jobs));
	}
	cv_.notify_all();
}

void JobQueue::Pause() {
	running_ = false;
	idleCv_.notify_all();
}

void JobQueue::CancelAll() {
	std::lock_guard<std::mutex> g(mutex_);
	running_ = false;
	for (auto &j : jobs_) {
		JobState s = j->State();
		if (s == JobState::Running)
			j->cancel = true;
		else if (s == JobState::Queued) {
			j->cancel = true;
			{
				std::lock_guard<std::mutex> rg(j->resultMutex);
				j->result.state = JobState::Cancelled;
				j->result.message = "cancelled";
			}
			j->state = (int)JobState::Cancelled;
		}
	}
	idleCv_.notify_all();
}

bool JobQueue::Busy() const {
	std::lock_guard<std::mutex> g(mutex_);
	if (active_ > 0)
		return true;
	if (!running_)
		return false;
	for (auto &j : jobs_)
		if (j->State() == JobState::Queued)
			return true;
	return false;
}

void JobQueue::WaitIdle() {
	std::unique_lock<std::mutex> lock(mutex_);
	idleCv_.wait(lock, [&] {
		if (active_ > 0)
			return false;
		if (!running_)
			return true;
		for (auto &j : jobs_)
			if (j->State() == JobState::Queued)
				return false;
		return true;
	});
}

std::vector<std::shared_ptr<Job>> JobQueue::Jobs() const {
	std::lock_guard<std::mutex> g(mutex_);
	return jobs_;
}

JobQueue::Counts JobQueue::Count() const {
	Counts c;
	std::lock_guard<std::mutex> g(mutex_);
	for (auto &j : jobs_) {
		switch (j->State()) {
		case JobState::Queued: ++c.queued; break;
		case JobState::Running: ++c.running; break;
		case JobState::Done: {
			++c.done;
			JobResult r = j->Result();
			c.inBytes += r.inputSize;
			c.outBytes += r.outputSize;
			break;
		}
		case JobState::Skipped: ++c.skipped; break;
		case JobState::Failed: ++c.failed; break;
		case JobState::Cancelled: ++c.cancelled; break;
		}
	}
	return c;
}

void JobQueue::EnsureWorkers(unsigned n) {
	// Called with or without the lock; workers_ is only touched from frontend threads.
	n = std::min(n, 64u);
	while (workers_.size() < n)
		workers_.emplace_back([this] { Worker(); });
}

void JobQueue::Worker() {
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		std::shared_ptr<Job> next;
		cv_.wait(lock, [&] {
			if (stop_)
				return true;
			if (!running_ || active_ >= (int)std::max(1u, settings_.engine.jobs))
				return false;
			for (auto &j : jobs_)
				if (j->State() == JobState::Queued)
					return true;
			return false;
		});
		if (stop_)
			return;
		for (auto &j : jobs_) {
			if (j->State() == JobState::Queued) {
				next = j;
				break;
			}
		}
		if (!next)
			continue;
		next->settings = settings_;
		next->state = (int)JobState::Running;
		++active_;
		lock.unlock();

		RunJob(*next);

		lock.lock();
		--active_;
		// Removed while running: drop it now.
		if (next->removeWhenDone.load())
			jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), next), jobs_.end());
		cv_.notify_all();
		idleCv_.notify_all();
	}
}

}  // namespace isopk
