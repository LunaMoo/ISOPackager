#include "gui/app.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui.h"

#include "core/engine.h"
#include "core/sysinfo.h"
#include "core/util.h"
#include "gui/widgets.h"

namespace isopk::gui {

static const ImVec4 kOk(0.40f, 0.85f, 0.50f, 1), kWarn(0.98f, 0.75f, 0.30f, 1), kErr(0.98f, 0.40f, 0.40f, 1), kDim(0.6f, 0.6f, 0.65f, 1);

static double Now() {
	return ImGui::GetTime();
}

// ---- Lifecycle --------------------------------------------------------------------------------

App::App(const PlatformHooks &hooks) : hooks_(hooks) {
	settingsPath_ = Settings::DefaultPath();
	settings_.Load(settingsPath_);
	addTarget_ = settings_.target;
	queue_.SetSettings(settings_);

	SetLogSink([this](LogLevel level, const std::string &text) {
		std::lock_guard<std::mutex> g(logMutex_);
		log_.push_back({ level, Timestamp().substr(11), text });
		if (log_.size() > 5000)
			log_.pop_front();
		if (level == LogLevel::Error)
			++unseenErrors_;
	});
	Log(LogLevel::Info, Fmt("ISOPackager %s on %s. %s", ISOPK_VERSION, PlatformName().c_str(), LibraryVersions().c_str()));
	Log(LogLevel::Info, Fmt("%u cores, %s RAM, RAM budget %s. Settings: %s", CpuCount(), HumanBytes(TotalRam()).c_str(),
		HumanBytes(RamBudget::Get().Capacity()).c_str(), settingsPath_.c_str()));

	benchCfg_.workDir = DefaultBenchDir();
	for (const std::string &line : settings_.benchCases) {
		BenchCase c;
		if (ParseBenchCase(line, &c))
			benchCases_.push_back(c);
	}
	metaThread_ = std::thread([this] { MetaWorker(); });
	ApplyUiStyle();
}

App::~App() {
	shuttingDown_ = true;
	benchCancel_ = true;
	queue_.CancelAll();
	{
		std::lock_guard<std::mutex> g(metaMutex_);
		metaQueue_.clear();
	}
	metaCv_.notify_all();
	if (metaThread_.joinable())
		metaThread_.join();
	if (benchThread_.joinable())
		benchThread_.join();
	for (auto &t : scanThreads_)
		if (t.joinable())
			t.join();
	if (settingsDirty_)
		SaveSettingsNow();
	SetLogSink(nullptr);
}

void App::ApplyUiStyle() {
	float scale = hooks_.dpiScale * settings_.uiScale * (hooks_.touch ? 1.35f : 1.0f);
	ApplyTheme(settings_.uiTheme, scale);
	ImGui::GetStyle().FontScaleDpi = scale;
	appliedScale_ = scale;
	appliedTheme_ = settings_.uiTheme;
}

void App::SetDpiScale(float s) {
	if (std::fabs(s - hooks_.dpiScale) > 0.01f) {
		hooks_.dpiScale = s;
		ApplyUiStyle();
	}
}

bool App::RequestClose() {
	if (!queue_.Busy() && !benchRunning_)
		return true;
	quitRequested_ = true;
	return false;
}

bool App::Animating() const {
	return queue_.Busy() || benchRunning_ || scansRunning_ > 0 || settingsDirty_;
}

void App::SettingsChanged() {
	settingsDirty_ = true;
	settingsChangedAt_ = Now();
	queue_.SetSettings(settings_);
}

void App::SaveSettingsNow() {
	settings_.target = addTarget_;
	if (!settings_.Save(settingsPath_))
		Log(LogLevel::Warning, "Can't save settings to " + settingsPath_);
	settingsDirty_ = false;
}

// ---- Adding files -----------------------------------------------------------------------------

void App::AddPaths(const std::vector<std::string> &paths) {
	if (!paths.empty())
		ScanAsync(paths, false);
}

void App::HandleArgs(const std::vector<std::string> &args) {
	std::vector<std::string> paths;
	for (size_t i = 0; i < args.size(); ++i) {
		if ((args[i] == "--to" || args[i] == "-f") && i + 1 < args.size()) {
			Format f = FormatFromName(args[++i]);
			if (f != Format::Unknown && f != Format::ZAR && FormatAvailable(f))
				addTarget_ = f;
			else
				Log(LogLevel::Warning, "Unknown format on the command line: " + args[i]);
		} else if (args[i] == "--tab" && i + 1 < args.size()) {
			const char *names[] = { "queue", "settings", "benchmark", "log", "about" };
			std::string t = ToLower(args[++i]);
			for (int k = 0; k < 5; ++k)
				if (t == names[k])
					requestTab_ = k;
		} else if (args[i] == "--zar" && i + 1 < args.size()) {
			StartCreateZar(args[++i]);
		} else if (args[i] == "--start") {
			startWhenScanned_ = true;
		} else if (!args[i].empty() && args[i][0] != '-') {
			paths.push_back(args[i]);
		}
	}
	AddPaths(paths);
}

void App::ScanAsync(const std::vector<std::string> &paths, bool forBench) {
	++scansRunning_;
	bool recursive = settings_.recursive;
	scanThreads_.emplace_back([this, paths, recursive, forBench] {
		std::vector<std::string> warnings;
		std::vector<InputFile> found = ScanInputs(paths, recursive, &warnings, &shuttingDown_);
		{
			std::lock_guard<std::mutex> g(scanMutex_);
			auto &dst = forBench ? benchScanned_ : scanned_;
			dst.insert(dst.end(), found.begin(), found.end());
			scanWarnings_.insert(scanWarnings_.end(), warnings.begin(), warnings.end());
		}
		--scansRunning_;
	});
}

void App::RequestMeta(const std::string &path) {
	std::lock_guard<std::mutex> g(metaMutex_);
	if (meta_.count(path))
		return;
	meta_[path] = ImageMeta();
	metaQueue_.push_back(path);
	metaCv_.notify_one();
}

void App::MetaWorker() {
	for (;;) {
		std::string path;
		{
			std::unique_lock<std::mutex> lock(metaMutex_);
			metaCv_.wait(lock, [&] { return shuttingDown_.load() || !metaQueue_.empty(); });
			if (shuttingDown_)
				return;
			path = metaQueue_.front();
			metaQueue_.pop_front();
		}
		ImageInfo info = InspectImage(path);
		std::lock_guard<std::mutex> g(metaMutex_);
		ImageMeta &m = meta_[path];
		m.discId = info.discId;
		m.title = info.title;
		m.detail = info.detail;
		m.isoSize = info.isoSize;
		m.done = true;
	}
}

void App::PickFiles(std::function<void(const std::vector<std::string> &)> then) {
	std::vector<std::string> files;
	if (hooks_.pickFiles && hooks_.pickFiles(&files)) {
		if (!files.empty())
			then(files);
		return;
	}
	browserThen_ = then;
	browser_.Open(FileBrowser::PickFiles, "Add disc images");
}

void App::PickFolder(std::function<void(const std::string &)> then) {
	std::string folder;
	if (hooks_.pickFolder && hooks_.pickFolder(&folder)) {
		if (!folder.empty())
			then(folder);
		return;
	}
	browserThen_ = [then](const std::vector<std::string> &r) {
		if (!r.empty())
			then(r[0]);
	};
	browser_.Open(FileBrowser::PickFolder, "Choose a folder");
}

// ---- Create ZArchive --------------------------------------------------------------------------

void App::StartCreateZar(const std::string &folder) {
	auto open = [this](const std::string &d) {
		ZarSpec spec;
		spec.rootDir = d;
		fs::path p = U8Path(d);
		if (!p.has_filename())
			p = p.parent_path();
		spec.name = PathU8(p.filename());
		{
			std::lock_guard<std::mutex> g(scanMutex_);
			zarDlg_.spec = spec;
			zarDlg_.size = 0;
			zarDlg_.error.clear();
			++zarDlg_.generation;
		}
		zarDlg_.requestOpen = true;
		MeasureZarAsync(spec, false);
	};
	if (!folder.empty())
		open(folder);
	else
		PickFolder(open);
}

void App::MeasureZarAsync(const ZarSpec &spec, bool forQueue) {
	++scansRunning_;
	int gen;
	{
		std::lock_guard<std::mutex> g(scanMutex_);
		gen = zarDlg_.generation;
	}
	scanThreads_.emplace_back([this, spec, forQueue, gen] {
		ZarSpec m = spec;
		std::string err;
		bool ok = MeasureZarSpec(&m, &err);
		{
			std::lock_guard<std::mutex> g(scanMutex_);
			if (forQueue) {
				if (ok)
					zarReady_.push_back(m);
				else
					scanWarnings_.push_back(m.name + ": " + err);
			} else if (gen == zarDlg_.generation) {
				zarDlg_.size = m.totalSize;
				zarDlg_.error = ok ? std::string() : err;
				zarDlg_.measuredGeneration = gen;
			}
		}
		--scansRunning_;
	});
}

void App::DrawZarDialog() {
	if (zarDlg_.requestOpen) {
		ImGui::OpenPopup("Create ZArchive");
		zarDlg_.requestOpen = false;
	}
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x * 0.9f, ImGui::GetFontSize() * 44), 0), ImGuiCond_Appearing);
	zarDlg_.open = ImGui::BeginPopupModal("Create ZArchive", nullptr, ImGuiWindowFlags_NoSavedSettings);
	if (!zarDlg_.open)
		return;
	const float em = ImGui::GetFontSize();
	ZarSpec spec;
	uint64_t size;
	std::string error;
	bool measured;
	{
		std::lock_guard<std::mutex> g(scanMutex_);
		spec = zarDlg_.spec;
		size = zarDlg_.size;
		error = zarDlg_.error;
		measured = zarDlg_.measuredGeneration == zarDlg_.generation;
	}
	auto change = [&](const std::function<void(ZarSpec &)> &fn) {
		ZarSpec copy;
		{
			std::lock_guard<std::mutex> g(scanMutex_);
			fn(zarDlg_.spec);
			++zarDlg_.generation;
			copy = zarDlg_.spec;
		}
		MeasureZarAsync(copy, false);
	};

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Name");
	ImGui::SameLine(em * 6);
	ImGui::SetNextItemWidth(-em * 3);
	std::string name = spec.name;
	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	if (InputStringHint("##zarname", "Legend of Heroes - Trails in the Sky", &name)) {
		std::lock_guard<std::mutex> g(scanMutex_);
		zarDlg_.spec.name = name;
	}
	ImGui::SameLine();
	ImGui::TextDisabled(".zar");

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Folder");
	ImGui::SameLine(em * 6);
	if (spec.rootDir.empty())
		ImGui::TextDisabled("(none)");
	else
		ImGui::TextWrapped("%s", spec.rootDir.c_str());
	Tip("Everything in this folder goes into the archive's root, the way the zarchive tool packs a folder.");
	ImGui::SetCursorPosX(em * 6);
	if (ImGui::Button(spec.rootDir.empty() ? "Choose folder..." : "Change folder..."))
		PickFolder([this](const std::string &d) {
			{
				std::lock_guard<std::mutex> g(scanMutex_);
				const bool wasDefaultName = zarDlg_.spec.name.empty() ||
					zarDlg_.spec.name == PathU8(U8Path(zarDlg_.spec.rootDir).filename());
				zarDlg_.spec.rootDir = d;
				if (wasDefaultName)
					zarDlg_.spec.name = PathU8(U8Path(d).filename());
				++zarDlg_.generation;
			}
			ZarSpec copy;
			{
				std::lock_guard<std::mutex> g(scanMutex_);
				copy = zarDlg_.spec;
			}
			MeasureZarAsync(copy, false);
		});
	if (!spec.rootDir.empty()) {
		ImGui::SameLine();
		if (ImGui::Button("Don't use a folder"))
			change([](ZarSpec &z) { z.rootDir.clear(); });
	}

	ImGui::Spacing();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Also add");
	ImGui::SameLine(em * 6);
	if (spec.extras.empty())
		ImGui::TextDisabled("(nothing else: optional)");
	int removeAt = -1;
	for (size_t i = 0; i < spec.extras.size(); ++i) {
		if (i > 0)
			ImGui::SetCursorPosX(em * 6);
		ImGui::PushID((int)i);
		if (ImGui::SmallButton("x"))
			removeAt = (int)i;
		ImGui::PopID();
		ImGui::SameLine();
		ImGui::TextWrapped("%s", spec.extras[i].c_str());
	}
	if (removeAt >= 0)
		change([removeAt](ZarSpec &z) { z.extras.erase(z.extras.begin() + removeAt); });
	ImGui::SetCursorPosX(em * 6);
	auto addExtras = [this](const std::vector<std::string> &paths) {
		ZarSpec copy;
		{
			std::lock_guard<std::mutex> g(scanMutex_);
			for (const std::string &p : paths)
				if (std::find(zarDlg_.spec.extras.begin(), zarDlg_.spec.extras.end(), p) == zarDlg_.spec.extras.end() && p != zarDlg_.spec.rootDir)
					zarDlg_.spec.extras.push_back(p);
			if (zarDlg_.spec.name.empty() && !zarDlg_.spec.extras.empty())
				zarDlg_.spec.name = PathU8(U8Path(zarDlg_.spec.extras.front()).stem());
			++zarDlg_.generation;
			copy = zarDlg_.spec;
		}
		MeasureZarAsync(copy, false);
	};
	if (ImGui::Button("Add files and folders...")) {
		browserThen_ = addExtras;
		browser_.Open(FileBrowser::PickAny, "Add to the archive: files and folders (each goes to the archive's root under its own name)",
			spec.SourceDir());
	}
	Tip("Discs or DLC folders from elsewhere. Each goes to the archive's root under its own name.");
	ImGui::SameLine();
	if (ImGui::Button("Add folder..."))
		PickFolder([addExtras](const std::string &d) { addExtras({ d }); });

	ImGui::Spacing();
	ImGui::Separator();
	if (!error.empty() && measured)
		ImGui::TextColored(kErr, "%s", error.c_str());
	else if (!measured)
		ImGui::TextDisabled("Measuring...");
	else
		ImGui::Text("Total %s", HumanBytes(size).c_str());
	ImGui::TextDisabled("Compression: %s", settings_.zar.Describe().c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("Change...")) {
		requestTab_ = 1;
		ImGui::CloseCurrentPopup();
	}
	if (!spec.name.empty() && (!spec.rootDir.empty() || !spec.extras.empty())) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Output: %s", PlanZarOutputPath(spec, settings_).c_str());
		ImGui::PopStyleColor();
	}

	const bool ready = !name.empty() && (!spec.rootDir.empty() || !spec.extras.empty()) && (!measured || error.empty());
	ImGui::BeginDisabled(!ready);
	const bool keys = !browser_.IsOpen();
	if (WideButton("Add to queue") || (keys && ready && ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsAnyItemActive())) {
		ZarSpec copy;
		{
			std::lock_guard<std::mutex> g(scanMutex_);
			copy = zarDlg_.spec;
			copy.name = name;
		}
		MeasureZarAsync(copy, true);
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (WideButton("Cancel") || (keys && ImGui::IsKeyPressed(ImGuiKey_Escape)))
		ImGui::CloseCurrentPopup();

	// The file browser opens over this dialog (a popup opened outside it would close it).
	if (browser_.Draw() && browserThen_) {
		auto then = browserThen_;
		browserThen_ = nullptr;
		then(browser_.Result());
	}
	ImGui::EndPopup();
}

std::string App::OutputFolderLabel() const {
	return settings_.out.dir.empty() ? std::string("Next to each source") : settings_.out.dir;
}

// ---- Frame ------------------------------------------------------------------------------------

void App::Frame() {
	if (settings_.uiTheme != appliedTheme_)
		ApplyUiStyle();

	// Collect finished scans.
	{
		std::lock_guard<std::mutex> g(scanMutex_);
		for (const InputFile &f : scanned_) {
			queue_.Add(f, addTarget_);
			RequestMeta(f.path);
		}
		if (!scanned_.empty())
			Log(LogLevel::Info, Fmt("Added %zu image(s)", scanned_.size()));
		scanned_.clear();
		if (startWhenScanned_ && scansRunning_ == 0) {
			startWhenScanned_ = false;
			queue_.Start();
		}
		for (const ZarSpec &z : zarReady_) {
			queue_.AddZArchive(z);
			Log(LogLevel::Info, Fmt("Queued ZArchive %s (%s)", z.name.c_str(), HumanBytes(z.totalSize).c_str()));
		}
		zarReady_.clear();
		for (const InputFile &f : benchScanned_) {
			if (f.format == Format::ZAR)
				continue;
			bool dup = false;
			for (auto &b : benchInputs_)
				dup |= b.path == f.path;
			if (!dup)
				benchInputs_.push_back(f);
		}
		benchScanned_.clear();
		for (auto &w : scanWarnings_)
			Log(LogLevel::Warning, w);
		scanWarnings_.clear();
	}
	if (settingsDirty_ && Now() - settingsChangedAt_ > 1.0)
		SaveSettingsNow();

	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("ISOPackager", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar();

	DrawToolbar();
	ImGui::Separator();

	const float statusH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	if (ImGui::BeginTabBar("##tabs")) {
		struct TabDef {
			const char *name;
			void (App::*draw)();
		};
		const TabDef tabs[] = { { "Queue", &App::DrawQueue }, { "Settings", &App::DrawSettings }, { "Benchmark", &App::DrawBenchmark },
			{ "Log", &App::DrawLog }, { "About", &App::DrawAbout } };
		for (int i = 0; i < 5; ++i) {
			std::string label = tabs[i].name;
			if (i == 3 && unseenErrors_ > 0)
				label += Fmt(" (%d)", unseenErrors_);
			label += "###tab" + std::to_string(i);
			ImGuiTabItemFlags flags = requestTab_ == i ? ImGuiTabItemFlags_SetSelected : 0;
			if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
				activeTab_ = i;
				ImGui::BeginChild("##tabbody", ImVec2(0, -statusH), ImGuiChildFlags_None);
				(this->*tabs[i].draw)();
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
		}
		requestTab_ = -1;
		ImGui::EndTabBar();
	}
	DrawStatusBar();
	DrawModals();
	ImGui::End();

}

// ---- Toolbar ----------------------------------------------------------------------------------

void App::DrawToolbar() {
	if (WideButton("Add files..."))
		PickFiles([this](const std::vector<std::string> &f) { AddPaths(f); });
	Tip("Add ISO, CSO, ZSO, DAX, CHD, PBP, RVZ or WIA images, or ZArchives (.zar) to extract. You can also drop files and folders on the window.");
	ImGui::SameLine();
	if (WideButton("Add folder..."))
		PickFolder([this](const std::string &d) { AddPaths({ d }); });
	Tip("Add every disc image in a folder (and its subfolders, see Settings > Add folders recursively).");
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Convert to");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.5f);
	if (FormatCombo("##target", &addTarget_)) {
		settings_.target = addTarget_;
		SettingsChanged();
	}
	Tip("Format for newly added images. Change a single queued image with right click > Convert to.");
	ImGui::SameLine();
	if (ImGui::Button("Apply to queued")) {
		for (auto &j : queue_.Jobs())
			if (j->State() == JobState::Queued && !j->zar && j->input.format != Format::ZAR)
				j->target = addTarget_;
	}
	Tip("Set every image that hasn't started yet to this format.");
	ImGui::SameLine();
	if (ImGui::Button("Create ZArchive..."))
		StartCreateZar();
	Tip("Pack a folder (several discs, DLC folders...) into one .zar archive. Choose the folder, name the archive, and it's "
		"added to the queue. Settings > ZArchive sets the compression.");

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	const bool running = queue_.Running();
	auto counts = queue_.Count();
	if (!running) {
		ImGui::BeginDisabled(counts.queued == 0);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.30f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.66f, 0.36f, 1));
		if (WideButton("Start"))
			queue_.Start();
		ImGui::PopStyleColor(2);
		ImGui::EndDisabled();
		Tip("Convert every queued image.");
	} else {
		if (WideButton("Pause"))
			queue_.Pause();
		Tip("Let the running conversions finish, and don't start new ones.");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(counts.running == 0 && counts.queued == 0);
	if (WideButton("Cancel all"))
		queue_.CancelAll();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(counts.done + counts.skipped + counts.failed + counts.cancelled == 0);
	if (WideButton("Clear finished")) {
		selection_.clear();
		queue_.ClearFinished();
	}
	ImGui::EndDisabled();

	// Second row: where outputs go.
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Output");
	ImGui::SameLine();
	bool nextTo = settings_.out.dir.empty();
	if (ImGui::RadioButton("next to source", nextTo) && !nextTo) {
		settings_.out.dir.clear();
		SettingsChanged();
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("folder:", !nextTo) && nextTo)
		PickFolder([this](const std::string &d) {
			settings_.out.dir = d;
			SettingsChanged();
		});
	ImGui::SameLine();
	{
		const ImGuiStyle &st = ImGui::GetStyle();
		float reserve = ImGui::CalcTextSize("Browse...").x + ImGui::CalcTextSize("Verify").x + ImGui::CalcTextSize("Delete sources").x +
			ImGui::GetFrameHeight() * 2 + st.FramePadding.x * 2 + st.ItemSpacing.x * 6 + st.ItemInnerSpacing.x * 2;
		ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8, ImGui::GetContentRegionAvail().x - reserve));
	}
	std::string dir = settings_.out.dir;
	if (InputStringHint("##outdir", "(next to each source)", &dir)) {
		settings_.out.dir = dir;
		SettingsChanged();
	}
	ImGui::SameLine();
	if (ImGui::Button("Browse..."))
		PickFolder([this](const std::string &d) {
			settings_.out.dir = d;
			SettingsChanged();
		});
	ImGui::SameLine();
	if (ImGui::Checkbox("Verify", &settings_.engine.verify))
		SettingsChanged();
	Tip("Decode every output again and compare it byte for byte with the source image (in memory, no extra disk space).");
	ImGui::SameLine();
	if (ImGui::Checkbox("Delete sources", &settings_.out.deleteSource)) {
		if (settings_.out.deleteSource)
			showOverwriteWarning_ = true;
		SettingsChanged();
	}
	Tip("Delete each source image after its conversion succeeded (and was verified, when Verify is on).");
}

// ---- Queue ------------------------------------------------------------------------------------

void App::DrawQueue() {
	auto jobs = queue_.Jobs();
	if (jobs.empty()) {
		ImVec2 avail = ImGui::GetContentRegionAvail();
		const char *msg = scansRunning_ > 0 ? "Scanning..." : "Drop disc images or folders here, or use Add files / Add folder.";
		ImVec2 ts = ImGui::CalcTextSize(msg);
		ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f, avail.y * 0.4f));
		ImGui::TextDisabled("%s", msg);
		const char *sub = "ISO, CSO, ZSO, CSO v2, DAX, CHD, EBOOT.PBP, RVZ and WIA in, any of them out. Create ZArchive packs a folder; a .zar extracts to one.";
		ts = ImGui::CalcTextSize(sub);
		ImGui::SetCursorPosX((avail.x - ts.x) * 0.5f);
		ImGui::TextDisabled("%s", sub);
		return;
	}

	// Drop selection entries for jobs that are gone.
	{
		std::set<Job *> alive;
		for (auto &j : jobs)
			alive.insert(j.get());
		for (auto it = selection_.begin(); it != selection_.end();)
			it = alive.count(*it) ? std::next(it) : selection_.erase(it);
	}

	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("##queue", 9, flags))
		return;
	const float em = ImGui::GetFontSize();
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, em * 3.5f);
	ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
	ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
	ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
	ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthFixed, em * 3.5f);
	ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, em * 4.0f);
	ImGui::TableSetupColumn("Verify", ImGuiTableColumnFlags_WidthFixed, em * 4.0f);
	ImGui::TableHeadersRow();

	std::shared_ptr<Job> removeJob, retryJob;
	bool removeSelected = false;
	for (int row = 0; row < (int)jobs.size(); ++row) {
		auto &j = jobs[row];
		JobState st = j->State();
		JobResult r = j->Result();
		ImageMeta meta;
		const ZarSpec *zar = j->zar.get();
		const bool unpack = j->input.format == Format::ZAR;  // a .zar to extract
		if (!zar) {
			std::lock_guard<std::mutex> g(metaMutex_);
			auto it = meta_.find(j->input.path);
			if (it != meta_.end())
				meta = it->second;
		}
		ImGui::PushID(j.get());
		ImGui::TableNextRow(0, ImGui::GetFrameHeight());

		// Image (selectable across the row).
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		std::string name = zar ? zar->name : GameName(j->input.path);
		bool sel = selection_.count(j.get()) != 0;
		std::string label = name;
		if (!meta.discId.empty())
			label += "  [" + meta.discId + "]";
		if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
			ImGuiIO &io = ImGui::GetIO();
			if (io.KeyShift && lastClickedRow_ >= 0) {
				int a = std::min(lastClickedRow_, row), b = std::max(lastClickedRow_, row);
				for (int k = a; k <= b && k < (int)jobs.size(); ++k)
					selection_.insert(jobs[k].get());
			} else if (io.KeyCtrl) {
				if (sel)
					selection_.erase(j.get());
				else
					selection_.insert(j.get());
				lastClickedRow_ = row;
			} else {
				selection_.clear();
				selection_.insert(j.get());
				lastClickedRow_ = row;
			}
		}
		if (ImGui::BeginItemTooltip()) {
			if (zar) {
				ImGui::Text("ZArchive %s.zar", zar->name.c_str());
				if (!zar->rootDir.empty())
					ImGui::TextDisabled("Contents of %s", zar->rootDir.c_str());
				for (const std::string &x : zar->extras)
					ImGui::TextDisabled("+ %s", x.c_str());
				ImGui::TextDisabled("%s", j->settings.zar.Describe().c_str());
			}
			if (!meta.title.empty())
				ImGui::Text("%s", meta.title.c_str());
			if (!zar)
				ImGui::TextDisabled("%s", j->input.path.c_str());
			if (!meta.detail.empty())
				ImGui::TextDisabled("%s", meta.detail.c_str());
			if (meta.isoSize)
				ImGui::TextDisabled("Uncompressed: %s", HumanBytes(meta.isoSize).c_str());
			if (!r.outputPath.empty())
				ImGui::Text("Output: %s", r.outputPath.c_str());
			if (!r.message.empty())
				ImGui::TextColored(st == JobState::Failed ? kErr : kDim, "%s", r.message.c_str());
			if (st == JobState::Done)
				ImGui::TextDisabled("Extract %s, compress %s, verify %s%s", HumanDuration(r.extractSecs).c_str(), HumanDuration(r.compressSecs).c_str(),
					HumanDuration(r.verifySecs).c_str(), r.viaRam ? ", repacked in RAM" : r.viaTempFile ? ", via a temporary ISO" : "");
			ImGui::EndTooltip();
		}
		if (ImGui::BeginPopupContextItem("##ctx")) {
			if (!sel) {
				selection_.clear();
				selection_.insert(j.get());
			}
			if (ImGui::BeginMenu("Convert to", !zar && !unpack && (st == JobState::Queued || st == JobState::Failed || st == JobState::Cancelled || st == JobState::Done ||
					st == JobState::Skipped))) {
				for (Format f : OutputFormats()) {
					if (ImGui::MenuItem(FormatName(f), nullptr, j->target.load() == f)) {
						for (auto &o : jobs)
							if (selection_.count(o.get()) && o->State() != JobState::Running && !o->zar && o->input.format != Format::ZAR) {
								o->target = f;
								if (o->State() != JobState::Queued)
									queue_.Retry(o);
							}
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Retry", nullptr, false, st == JobState::Failed || st == JobState::Cancelled || st == JobState::Skipped || st == JobState::Done))
				retryJob = j;
			if (ImGui::MenuItem("Cancel", nullptr, false, st == JobState::Running || st == JobState::Queued))
				j->cancel = true;
			ImGui::Separator();
			if (hooks_.reveal) {
				if (ImGui::MenuItem("Show source"))
					hooks_.reveal(j->input.path);
				if (ImGui::MenuItem("Show output", nullptr, false, !r.outputPath.empty() && st == JobState::Done))
					hooks_.reveal(r.outputPath);
			}
			if (ImGui::MenuItem("Copy path"))
				ImGui::SetClipboardText(j->input.path.c_str());
			ImGui::Separator();
			if (ImGui::MenuItem("Remove", "Del"))
				removeSelected = true;
			ImGui::EndPopup();
		}

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (zar)
			ImGui::TextDisabled("files");
		else
			FormatBadge(j->input.format);
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(HumanBytes(j->input.size).c_str());

		// Target: editable while queued.
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (unpack) {
			ImGui::TextDisabled("folder");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Extracted next to the archive, into a folder named after it");
		} else if (st == JobState::Queued && !zar) {
			Format t = j->target.load();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::PushStyleColor(ImGuiCol_Text, FormatColor(t));
			if (FormatCombo("##to", &t))
				j->target = t;
			ImGui::PopStyleColor();
		} else {
			FormatBadge(j->target.load());
		}

		// Progress.
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		switch (st) {
		case JobState::Queued:
			ImGui::TextDisabled("%s", queue_.Running() ? "Waiting" : "Queued");
			break;
		case JobState::Running: {
			Phase ph = j->CurrentPhase();
			std::string ov = Fmt("%s %.0f%%", PhaseName(ph), j->phaseProgress.load() * 100);
			ImGui::ProgressBar((float)j->overall.load(), ImVec2(-FLT_MIN, 0), ov.c_str());
			break;
		}
		case JobState::Done:
			ImGui::TextColored(kOk, "Done%s", r.message.empty() ? "" : (" - " + r.message).c_str());
			break;
		case JobState::Skipped:
			ImGui::TextColored(kWarn, "Skipped: %s", r.message.c_str());
			break;
		case JobState::Failed:
			ImGui::TextColored(kErr, "Failed: %s", r.message.c_str());
			break;
		case JobState::Cancelled:
			ImGui::TextColored(kDim, "Cancelled");
			break;
		}

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (st == JobState::Done)
			ImGui::TextUnformatted(HumanBytes(r.outputSize).c_str());
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (st == JobState::Done && r.isoSize)
			ImGui::Text("%.1f%%", 100.0 * r.outputSize / r.isoSize);
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (st == JobState::Done || st == JobState::Failed)
			ImGui::TextUnformatted(HumanDuration(r.seconds).c_str());
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		if (st == JobState::Done)
			ImGui::TextColored(r.verify == "OK" ? kOk : kDim, "%s", r.verify.c_str());
		else if (st == JobState::Failed && r.verify != "-" && r.verify != "OK")
			ImGui::TextColored(kErr, "BAD");
		ImGui::PopID();
	}
	ImGui::EndTable();

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete))
		removeSelected = true;
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
		for (auto &j : jobs)
			selection_.insert(j.get());
	if (removeSelected) {
		for (auto &j : jobs)
			if (selection_.count(j.get()))
				queue_.Remove(j);
		selection_.clear();
	}
	if (retryJob) {
		for (auto &j : jobs)
			if (selection_.count(j.get()) || j == retryJob)
				queue_.Retry(j);
	}
}

}  // namespace isopk::gui
