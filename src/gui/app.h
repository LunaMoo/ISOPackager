// The ISOPackager window: queue, settings, benchmark, log. Platform layers (main_win32.cpp,
// main_sdl.cpp) own the window and the renderer and call Frame() once per frame.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/bench.h"
#include "core/queue.h"
#include "core/settings.h"
#include "gui/file_browser.h"

namespace isopk::gui {

struct PlatformHooks {
	// Native dialogs. Return false when unavailable (the built-in browser is used instead).
	std::function<bool(std::vector<std::string> *files)> pickFiles;
	std::function<bool(std::string *folder)> pickFolder;
	// Shows a file or folder in the system file manager.
	std::function<void(const std::string &path)> reveal;
	std::function<void(const std::string &url)> openUrl;
	// Called when the DPI scale or theme must be re-applied.
	float dpiScale = 1.0f;
	bool touch = false;  // phones and tablets: bigger targets, simpler layout
	std::string platformName;
};

class App {
public:
	explicit App(const PlatformHooks &hooks);
	~App();

	// Files dropped on the window or passed on the command line.
	void AddPaths(const std::vector<std::string> &paths);
	// Command line: [--to FMT] [--start] [files...]
	void HandleArgs(const std::vector<std::string> &args);
	void Frame();

	// The platform asks before closing: true = close now. Otherwise the app asks the user and
	// QuitConfirmed() turns true when they agree.
	bool RequestClose();
	bool QuitConfirmed() const { return quitConfirmed_; }
	// Whether anything is running (to keep redrawing instead of waiting for input).
	bool Animating() const;
	void SetDpiScale(float s);

private:
	struct ImageMeta {
		std::string discId, title, detail;
		uint64_t isoSize = 0;
		bool done = false;
	};
	struct LogLine {
		LogLevel level;
		std::string time, text;
	};

	void DrawToolbar();
	void DrawQueue();
	void DrawSettings();
	void DrawBenchmark();
	void DrawLog();
	void DrawAbout();
	void DrawStatusBar();
	void DrawModals();
	void DrawBenchChart(float height);

	void PickFiles(std::function<void(const std::vector<std::string> &)> then);
	void PickFolder(std::function<void(const std::string &)> then);
	void ScanAsync(const std::vector<std::string> &paths, bool forBench);
	void RequestMeta(const std::string &path);
	void MetaWorker();
	void SettingsChanged();
	void SaveSettingsNow();
	void StartBenchmark();
	void AddToBenchmark(const BenchCase &c);
	void SaveBenchCases();
	void DrawAddToBenchmark(const BenchCase &c, const char *id, const std::function<void()> &extra = nullptr);
	void ApplyUiStyle();
	std::string OutputFolderLabel() const;

	PlatformHooks hooks_;
	Settings settings_;
	std::string settingsPath_;
	bool settingsDirty_ = false;
	double settingsChangedAt_ = 0;
	Format addTarget_ = Format::CSO;

	JobQueue queue_;
	std::set<Job *> selection_;
	int lastClickedRow_ = -1;

	// Folder scans run off the UI thread.
	std::mutex scanMutex_;
	std::vector<std::thread> scanThreads_;
	std::vector<InputFile> scanned_;
	std::vector<InputFile> benchScanned_;
	std::vector<std::string> scanWarnings_;
	std::atomic<int> scansRunning_{ 0 };
	std::atomic<bool> shuttingDown_{ false };

	// Disc ID / title lookups, also off the UI thread.
	std::mutex metaMutex_;
	std::condition_variable metaCv_;
	std::deque<std::string> metaQueue_;
	std::map<std::string, ImageMeta> meta_;
	std::thread metaThread_;

	// Log.
	std::mutex logMutex_;
	std::deque<LogLine> log_;
	bool logAutoScroll_ = true, logShowDebug_ = false;
	int unseenErrors_ = 0;

	// Benchmark.
	std::vector<InputFile> benchInputs_;
	std::vector<BenchCase> benchCases_;  // what the benchmark runs; saved in the settings
	Format benchAddCsoFormat_ = Format::CSO;  // which mCSO format Settings > CSO adds
	std::string benchNotice_;                // "Added ..." under the Add buttons
	double benchNoticeAt_ = -10;
	BenchConfig benchCfg_;
	std::thread benchThread_;
	std::atomic<bool> benchRunning_{ false }, benchCancel_{ false };
	std::mutex benchMutex_;
	std::vector<BenchRow> benchRows_;
	std::string benchWhat_;
	double benchStep_ = 0, benchTotal_ = 0;
	int benchChartX_ = 0;  // 0 compress MB/s, 1 decode s, 2 random read ms

	// UI.
	FileBrowser browser_;
	std::function<void(const std::vector<std::string> &)> browserThen_;
	int activeTab_ = 0, requestTab_ = -1;
	bool showDemo_ = false, quitRequested_ = false, quitConfirmed_ = false, showOverwriteWarning_ = false;
	float appliedScale_ = 0;
	bool startWhenScanned_ = false;
	int appliedTheme_ = -1;
};

}  // namespace isopk::gui
