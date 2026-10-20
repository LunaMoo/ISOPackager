// The mobile layout: one column and one page at a time, with finger-sized buttons and bigger text.
// A header shows where you are and what's running, the page scrolls by dragging it, and a
// navigation bar sits at the bottom with the switch back to the desktop layout in its middle.
//
// It draws the same state as the desktop layout (queue, settings, benchmark, log), so switching
// either way at any time loses nothing; the Settings and Benchmark pages reuse the desktop code,
// which lays itself out in one column while MobileLayout() is on.
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
static const ImVec4 kStart(0.18f, 0.55f, 0.30f, 1), kStartHover(0.22f, 0.66f, 0.36f, 1);
static const ImVec4 kSwitch(0.46f, 0.30f, 0.72f, 1), kSwitchHover(0.55f, 0.38f, 0.84f, 1), kSwitchActive(0.40f, 0.26f, 0.64f, 1);

static const char *const kPageNames[] = { "Queue", "Settings", "Benchmark", "Log", "About" };

// ---- Switching layouts ------------------------------------------------------------------------

bool App::WantMobile() const {
	return settings_.uiLayout == 2 || (settings_.uiLayout == 0 && hooks_.touch);
}

void App::ToggleLayout() {
	// An explicit choice from now on (Settings > Appearance > Layout can go back to Automatic).
	settings_.uiLayout = mobile_ ? 1 : 2;
	SettingsChanged();
}

void App::DrawLayoutToggle(float width, float height) {
	ImGui::PushStyleColor(ImGuiCol_Button, kSwitch);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSwitchHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSwitchActive);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	if (ImGui::Button(mobile_ ? "Desktop UI###layoutswitch" : "Mobile UI###layoutswitch", ImVec2(width, height)) && !TouchScrolled())
		ToggleLayout();
	ImGui::PopStyleColor(4);
	if (!mobile_)
		Tip("Switch to the mobile layout: one column, bigger buttons and text. The same button in the middle at the bottom switches back.");
}

// ---- Frame ------------------------------------------------------------------------------------

void App::DrawMobile() {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::Begin("ISOPackager##mobile", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar(3);

	if (requestTab_ >= 0) {  // --tab, or "Change..." in the ZArchive dialog
		mobilePage_ = std::clamp(requestTab_, 0, 4);
		requestTab_ = -1;
	}

	const ImGuiStyle &st = ImGui::GetStyle();
	// On a tablet or a desktop window the column stays phone-shaped, centred, instead of stretching.
	{
		const float avail = ImGui::GetContentRegionAvail().x;
		mobileColW_ = std::floor(std::min(avail, ImGui::GetFontSize() * 34));
		mobileColX_ = std::floor((avail - mobileColW_) * 0.5f);
	}
	const float navH = MobileNavHeight();
	const float actionsH = mobilePage_ == 0 ? MobileActionsHeight() + st.ItemSpacing.y : 0;

	DrawMobileHeader();

	const float em = ImGui::GetFontSize();
	// One child per page, so each keeps its own scroll position.
	ImGui::PushID(mobilePage_ == 4 ? 3 : mobilePage_);
	ImGui::SetCursorPosX(mobileColX_);
	ImGui::BeginChild("##page", ImVec2(mobileColW_, -(navH + actionsH + st.ItemSpacing.y)), ImGuiChildFlags_AlwaysUseWindowPadding);
	// Everything on the page wraps at the screen's edge instead of running off it.
	ImGui::PushTextWrapPos(0.0f);
	switch (mobilePage_) {
	case 0: DrawMobileQueue(); break;
	case 1: DrawSettings(); break;
	case 2:
		DrawBenchConfig();
		ImGui::Spacing();
		DrawBenchResults();
		break;
	default: DrawMobileLog(); break;
	}
	ImGui::PopTextWrapPos();
	// Room to scroll the last item above the bars.
	ImGui::Dummy(ImVec2(0, em));
	TouchScroll();
	ImGui::EndChild();
	ImGui::PopID();

	if (mobilePage_ == 0)
		DrawMobileQueueActions();
	DrawMobileNav();
	DrawModals();
	ImGui::End();
}

void App::DrawMobileHeader() {
	const float em = ImGui::GetFontSize();
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
	ImGui::SetCursorPosX(mobileColX_);
	ImGui::BeginChild("##header", ImVec2(mobileColW_, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleColor();

	// Page name on the left, big; what the queue is doing on the right.
	const float titleY = ImGui::GetCursorPosY();
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
	ImGui::TextUnformatted(kPageNames[std::clamp(mobilePage_, 0, 4)]);
	ImGui::PopFont();

	auto c = queue_.Count();
	std::string right;
	if (scansRunning_ > 0)
		right = "Scanning...";
	else if (c.running)
		right = Fmt("%d running, %d queued", c.running, c.queued);
	else if (c.queued)
		right = Fmt("%d queued", c.queued);
	else if (c.done)
		right = Fmt("%d done", c.done);
	else
		right = "ISOPackager";
	ImGui::SameLine();
	const float w = ImGui::CalcTextSize(right.c_str()).x;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + em, ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w));
	ImGui::SetCursorPosY(titleY + em * 0.3f * 0.8f);  // on the title's baseline
	ImGui::TextColored(c.failed ? kErr : scansRunning_ > 0 ? kWarn : kDim, "%s", right.c_str());

	// Overall progress while converting.
	if (c.running) {
		double sum = 0;
		int n = 0;
		for (auto &j : queue_.Jobs()) {
			JobState s = j->State();
			if (s == JobState::Cancelled)
				continue;
			++n;
			sum += s == JobState::Running ? j->overall.load() : s == JobState::Queued ? 0.0 : 1.0;
		}
		std::string ov = Fmt("%d of %d", c.done + c.failed + c.skipped, n);
		ImGui::ProgressBar(n ? (float)(sum / n) : 0.0f, ImVec2(-FLT_MIN, em * 0.9f), ov.c_str());
	}
	if (c.failed || c.skipped) {
		if (c.failed)
			ImGui::TextColored(kErr, "%d failed", c.failed);
		if (c.skipped) {
			if (c.failed)
				ImGui::SameLine();
			ImGui::TextColored(kWarn, "%d skipped", c.skipped);
		}
	}
	ImGui::EndChild();
}

// The bottom bar: four pages and the layout switch in the middle.
void App::DrawMobileNav() {
	const ImGuiStyle &st = ImGui::GetStyle();
	const float em = ImGui::GetFontSize();
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x * 0.5f, st.ItemSpacing.y));
	ImGui::SetCursorPosX(mobileColX_);
	ImGui::BeginChild("##nav", ImVec2(mobileColW_, MobileNavHeight()), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleColor();
	// Five cells, the switch in the middle a little wider; smaller text so the names fit a phone.
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.76f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, st.FramePadding.y));
	const float h = em * 2.6f;
	const float unit = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5.4f;
	const float w = std::floor(unit), wMid = std::floor(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4 - w * 4);
	auto c = queue_.Count();
	struct Item {
		int page;
		std::string label;
	};
	Item items[] = {
		{ 0, "Queue" },  // the header has the counts
		{ 1, "Settings" },
		{ -1, "" },  // layout switch
		{ 2, "Bench" },
		{ 3, unseenErrors_ > 0 ? Fmt("Log (%d)", unseenErrors_) : std::string("Log") },
	};
	for (int i = 0; i < 5; ++i) {
		if (i > 0)
			ImGui::SameLine();
		if (items[i].page < 0) {
			DrawLayoutToggle(wMid, h);
			continue;
		}
		const bool active = mobilePage_ == items[i].page || (items[i].page == 3 && mobilePage_ == 4);
		ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, active ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if (items[i].page == 3 && unseenErrors_ > 0 && !active)
			ImGui::PushStyleColor(ImGuiCol_Text, kErr);
		std::string id = items[i].label + "###nav" + std::to_string(i);
		if (ImGui::Button(id.c_str(), ImVec2(w, h)) && !TouchScrolled()) {
			if (mobilePage_ != items[i].page)
				mobileOpenJob_ = nullptr;
			mobilePage_ = items[i].page;
		}
		if (items[i].page == 3 && unseenErrors_ > 0 && !active)
			ImGui::PopStyleColor();
		ImGui::PopStyleColor(2);
	}
	ImGui::PopStyleVar();
	ImGui::PopFont();
	ImGui::EndChild();
	ImGui::PopStyleVar();
}

float App::MobileNavHeight() const {
	return ImGui::GetFontSize() * 2.6f + ImGui::GetStyle().WindowPadding.y * 2;
}

float App::MobileActionsHeight() const {
	return ImGui::GetFontSize() * 2.7f + ImGui::GetStyle().WindowPadding.y * 2;
}

// ---- Queue page -------------------------------------------------------------------------------

void App::DrawMobileQueue() {
	const float em = ImGui::GetFontSize();

	// Adding.
	const float w2 = SplitWidth(2);
	if (BigButton("Add files", w2) && !TouchScrolled())
		PickFiles([this](const std::vector<std::string> &f) { AddPaths(f); });
	ImGui::SameLine();
	if (BigButton("Add folder", w2) && !TouchScrolled())
		PickFolder([this](const std::string &d) { AddPaths({ d }); });
	if (BigButton("Create ZArchive...", -FLT_MIN, 2.2f) && !TouchScrolled())
		StartCreateZar();

	// Target format.
	ImGui::Spacing();
	ImGui::TextUnformatted("Convert new images to");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::PushStyleColor(ImGuiCol_Text, FormatColor(addTarget_));
	if (FormatCombo("##target", &addTarget_)) {
		settings_.target = addTarget_;
		SettingsChanged();
	}
	ImGui::PopStyleColor();
	auto jobs = queue_.Jobs();
	int retarget = 0;
	for (auto &j : jobs)
		retarget += j->State() == JobState::Queued && !j->zar && j->input.format != Format::ZAR && j->target.load() != addTarget_;
	if (retarget > 0) {
		std::string l = Fmt("Use %s for the %d queued image%s", FormatName(addTarget_), retarget, retarget == 1 ? "" : "s");
		if (BigButton(l.c_str(), -FLT_MIN, 2.2f) && !TouchScrolled())
			for (auto &j : jobs)
				if (j->State() == JobState::Queued && !j->zar && j->input.format != Format::ZAR)
					j->target = addTarget_;
	}

	// Output options, folded: they're set once and rarely touched.
	const bool nextTo = settings_.out.dir.empty();
	(void)em;
	std::string where = nextTo ? std::string("next to each source") : PathU8(U8Path(settings_.out.dir).filename());
	if (where.empty())
		where = settings_.out.dir;
	std::string hdr = (nextTo ? std::string("Output: next to source") : "Output: " + where) + "###mout";
	ImGui::Spacing();
	if (ImGui::CollapsingHeader(hdr.c_str())) {
		const float w = SplitWidth(2);
		auto segment = [&](const char *label, bool on) {
			ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
			bool r = BigButton(label, w, 2.2f) && !TouchScrolled();
			ImGui::PopStyleColor();
			return r;
		};
		if (segment("Next to source", nextTo) && !nextTo) {
			settings_.out.dir.clear();
			SettingsChanged();
		}
		ImGui::SameLine();
		if (segment(nextTo ? "Folder..." : "Other folder...", !nextTo))
			PickFolder([this](const std::string &d) {
				settings_.out.dir = d;
				SettingsChanged();
			});
		if (!nextTo)
			ImGui::TextDisabled("%s", settings_.out.dir.c_str());
		if (CheckboxWrap("Verify every output", &settings_.engine.verify))
			SettingsChanged();
		HelpMarker("Decodes each output again and compares it byte for byte with the source.");
		if (CheckboxWrap("Delete sources after a successful conversion", &settings_.out.deleteSource)) {
			if (settings_.out.deleteSource)
				showOverwriteWarning_ = true;
			SettingsChanged();
		}
		ImGui::TextDisabled("Everything else is in Settings.");
	}

	// The queue, as cards.
	ImGui::Spacing();
	auto c = queue_.Count();
	const int finished = c.done + c.skipped + c.failed + c.cancelled;
	ImGui::SeparatorText(Fmt("Images (%zu)", jobs.size()).c_str());
	if (jobs.empty()) {
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", scansRunning_ > 0 ? "Scanning..." : "Nothing queued yet. Add disc images or a folder of them above.");
		ImGui::Spacing();
		ImGui::TextWrapped("ISO, CSO, ZSO, CSO v2, DAX, CHD, EBOOT.PBP, RVZ and WIA in, any of them out. Create ZArchive packs a folder; a .zar "
						   "extracts to one.");
		ImGui::PopStyleColor();
		return;
	}
	bool openAlive = false;
	for (auto &j : jobs)
		openAlive |= j.get() == mobileOpenJob_;
	if (!openAlive)
		mobileOpenJob_ = nullptr;

	std::shared_ptr<Job> removeJob, retryJob;
	for (auto &j : jobs)
		DrawMobileJobCard(j, &removeJob, &retryJob);
	if (removeJob) {
		if (mobileOpenJob_ == removeJob.get())
			mobileOpenJob_ = nullptr;
		selection_.erase(removeJob.get());
		queue_.Remove(removeJob);
	}
	if (retryJob)
		queue_.Retry(retryJob);

	if (finished > 0) {
		ImGui::Spacing();
		if (BigButton(Fmt("Clear %d finished", finished).c_str(), -FLT_MIN, 2.2f) && !TouchScrolled()) {
			selection_.clear();
			mobileOpenJob_ = nullptr;
			queue_.ClearFinished();
		}
	}
}

void App::DrawMobileJobCard(const std::shared_ptr<Job> &j, std::shared_ptr<Job> *remove, std::shared_ptr<Job> *retry) {
	const float em = ImGui::GetFontSize();
	const ImGuiStyle &st = ImGui::GetStyle();
	JobState s = j->State();
	JobResult r = j->Result();
	const ZarSpec *zar = j->zar.get();
	const bool unpack = j->input.format == Format::ZAR;
	ImageMeta meta;
	if (!zar) {
		std::lock_guard<std::mutex> g(metaMutex_);
		auto it = meta_.find(j->input.path);
		if (it != meta_.end())
			meta = it->second;
	}
	const bool open = mobileOpenJob_ == j.get();

	ImGui::PushID(j.get());
	ImGui::PushStyleColor(ImGuiCol_ChildBg, open ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
	ImGui::PushStyleColor(ImGuiCol_Border, s == JobState::Failed ? kErr : ImGui::GetStyleColorVec4(ImGuiCol_Border));
	ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleColor(2);
	ImGui::PushTextWrapPos(0.0f);

	// Name, with a + / - showing the card opens.
	{
		const float signW = ImGui::CalcTextSize("+").x + em * 0.5f;
		ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - signW + ImGui::GetCursorPosX());
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.1f);
		ImGui::TextUnformatted((zar ? zar->name + ".zar" : GameName(j->input.path)).c_str());
		ImGui::PopFont();
		ImGui::PopTextWrapPos();
		const ImVec2 rmin = ImGui::GetItemRectMin();
		ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - st.WindowPadding.x - signW * 0.6f, rmin.y),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), open ? "-" : "+");
	}
	if (!meta.discId.empty() || !meta.title.empty()) {
		std::string sub = meta.discId;
		if (!meta.title.empty())
			sub += (sub.empty() ? "" : "  ") + meta.title;
		ImGui::TextDisabled("%s", sub.c_str());
	}

	// From -> to.
	if (zar)
		ImGui::TextDisabled("files");
	else
		FormatBadge(j->input.format);
	ImGui::SameLine();
	ImGui::TextDisabled("%s  ->", HumanBytes(j->input.size).c_str());
	ImGui::SameLine();
	if (unpack)
		ImGui::TextDisabled("folder");
	else
		FormatBadge(zar ? Format::ZAR : j->target.load());

	// State.
	switch (s) {
	case JobState::Queued: ImGui::TextDisabled("%s", queue_.Running() ? "Waiting" : "Queued"); break;
	case JobState::Running: {
		Phase ph = j->CurrentPhase();
		std::string ov = Fmt("%s %.0f%%", PhaseName(ph), j->phaseProgress.load() * 100);
		ImGui::ProgressBar((float)j->overall.load(), ImVec2(-FLT_MIN, em * 1.5f), ov.c_str());
		break;
	}
	case JobState::Done: {
		std::string line = HumanBytes(r.outputSize);
		if (r.isoSize)
			line += Fmt(" (%.1f%%)", 100.0 * r.outputSize / r.isoSize);
		line += " in " + HumanDuration(r.seconds);
		if (r.verify == "OK")
			line += ", verified";
		ImGui::TextColored(kOk, "%s", line.c_str());
		if (!r.message.empty())
			ImGui::TextDisabled("%s", r.message.c_str());
		break;
	}
	case JobState::Skipped: ImGui::TextColored(kWarn, "Skipped: %s", r.message.c_str()); break;
	case JobState::Failed:
		ImGui::TextColored(kErr, "Failed: %s", r.message.c_str());
		if (r.verify != "-" && r.verify != "OK" && !r.verify.empty())
			ImGui::TextColored(kErr, "Verify: %s", r.verify.c_str());
		break;
	case JobState::Cancelled: ImGui::TextColored(kDim, "Cancelled"); break;
	}

	// Actions, when the card is open.
	if (open) {
		ImGui::Separator();
		const bool convertible = !zar && !unpack && s != JobState::Running;
		if (convertible) {
			ImGui::TextUnformatted(s == JobState::Queued ? "Convert to" : "Convert again to");
			Format t = j->target.load();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::PushStyleColor(ImGuiCol_Text, FormatColor(t));
			if (FormatCombo("##to", &t)) {
				j->target = t;
				if (s != JobState::Queued)
					*retry = j;
			}
			ImGui::PopStyleColor();
		}
		const float w2 = SplitWidth(2);
		int n = 0;
		auto button = [&](const char *label, bool enabled) {
			if (n++ % 2)
				ImGui::SameLine();
			ImGui::BeginDisabled(!enabled);
			bool r2 = BigButton(label, w2, 2.3f) && !TouchScrolled();
			ImGui::EndDisabled();
			return r2;
		};
		if (button("Retry", s == JobState::Failed || s == JobState::Cancelled || s == JobState::Skipped || s == JobState::Done))
			*retry = j;
		if (button("Cancel", s == JobState::Running || s == JobState::Queued))
			j->cancel = true;
		if (hooks_.reveal) {
			if (button("Show source", true))
				hooks_.reveal(j->input.path);
			if (button("Show output", !r.outputPath.empty() && s == JobState::Done))
				hooks_.reveal(r.outputPath);
		}
		if (button("Copy path", true))
			ImGui::SetClipboardText(j->input.path.c_str());
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.20f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.22f, 0.24f, 1));
		if (button("Remove", true))
			*remove = j;
		ImGui::PopStyleColor(2);

		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
		if (zar) {
			if (!zar->rootDir.empty())
				ImGui::TextDisabled("Contents of %s", zar->rootDir.c_str());
			for (const std::string &x : zar->extras)
				ImGui::TextDisabled("+ %s", x.c_str());
			ImGui::TextDisabled("%s", j->settings.zar.Describe().c_str());
		} else {
			ImGui::TextDisabled("%s", j->input.path.c_str());
		}
		if (!meta.detail.empty())
			ImGui::TextDisabled("%s", meta.detail.c_str());
		if (meta.isoSize)
			ImGui::TextDisabled("Uncompressed: %s", HumanBytes(meta.isoSize).c_str());
		if (!r.outputPath.empty())
			ImGui::TextDisabled("Output: %s", r.outputPath.c_str());
		if (s == JobState::Done)
			ImGui::TextDisabled("Extract %s, compress %s, verify %s%s", HumanDuration(r.extractSecs).c_str(), HumanDuration(r.compressSecs).c_str(),
				HumanDuration(r.verifySecs).c_str(), r.viaRam ? ", repacked in RAM" : r.viaTempFile ? ", via a temporary ISO" : "");
		ImGui::PopFont();
	}

	// A tap anywhere on the card that isn't a control opens or closes it.
	{
		ImGuiIO &io = ImGui::GetIO();
		const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
		const ImVec2 press = io.MouseClickedPos[0];
		const bool pressedHere = press.x >= wp.x && press.y >= wp.y && press.x < wp.x + ws.x && press.y < wp.y + ws.y;
		if (pressedHere && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() && !TouchScrolled() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
			mobileOpenJob_ = open ? nullptr : j.get();
	}
	ImGui::PopTextWrapPos();
	ImGui::EndChild();
	ImGui::PopID();
}

// Start / Pause and Cancel all, always in reach above the navigation bar.
void App::DrawMobileQueueActions() {
	const float em = ImGui::GetFontSize();
	ImGui::SetCursorPosX(mobileColX_);
	ImGui::BeginChild("##actions", ImVec2(mobileColW_, MobileActionsHeight()), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	const bool running = queue_.Running();
	auto c = queue_.Count();
	const float total = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x;
	const float mainW = std::floor(total * 0.62f), sideW = total - mainW;
	if (!running) {
		ImGui::BeginDisabled(c.queued == 0);
		ImGui::PushStyleColor(ImGuiCol_Button, kStart);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kStartHover);
		std::string l = c.queued ? Fmt("Start (%d)###start", c.queued) : std::string("Start###start");
		if (BigButton(l.c_str(), mainW, 2.7f))
			queue_.Start();
		ImGui::PopStyleColor(2);
		ImGui::EndDisabled();
	} else {
		if (BigButton("Pause###start", mainW, 2.7f))
			queue_.Pause();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(c.running == 0 && c.queued == 0);
	if (BigButton("Cancel all", sideW, 2.7f))
		queue_.CancelAll();
	ImGui::EndDisabled();
	ImGui::EndChild();
}

// ---- Benchmark results ------------------------------------------------------------------------

void App::DrawMobileBenchResults(const std::vector<BenchRow> &rowsIn) {
	std::vector<BenchRow> rows = rowsIn;
	ImGui::SeparatorText("Results");
	SettingLabel("Sort by");
	ImGui::Combo("##benchsort", &mobileBenchSort_, "Smallest first\0Fastest compression first\0Fastest decode first\0Fastest random read first\0Image\0");
	std::stable_sort(rows.begin(), rows.end(), [this](const BenchRow &a, const BenchRow &b) {
		if (a.failed != b.failed)
			return !a.failed;
		switch (mobileBenchSort_) {
		case 1: return a.CompMBs() > b.CompMBs();
		case 2: return a.decompSecs < b.decompSecs;
		case 3: return (a.readMs < 0 ? 1e30 : a.readMs) < (b.readMs < 0 ? 1e30 : b.readMs);
		case 4: return a.game < b.game;
		default: return a.Ratio() < b.Ratio();
		}
	});
	for (size_t i = 0; i < rows.size(); ++i) {
		const BenchRow &r = rows[i];
		ImGui::PushID((int)i);
		ImGui::BeginChild("##res", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(FormatColor(r.format), "%s", r.format.c_str());
		ImGui::SameLine();
		ImGui::TextUnformatted(r.setting.c_str());
		ImGui::TextDisabled("%s", r.game.c_str());
		if (r.failed) {
			ImGui::TextColored(kErr, "FAILED: %s", r.error.c_str());
		} else {
			ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
			ImGui::Text("%.3f%%", r.Ratio());
			ImGui::PopFont();
			ImGui::SameLine();
			ImGui::TextDisabled("of the ISO, %s", HumanBytes(r.output).c_str());
			ImGui::Text("Compress %.1f s (%.1f MB/s)", r.compSecs, r.CompMBs());
			ImGui::Text("Decode %.2f s", r.decompSecs);
			if (r.readMs >= 0) {
				ImGui::SameLine();
				ImGui::Text(", random read %.3f ms", r.readMs);
			}
			ImGui::TextColored(r.verify.rfind("OK", 0) == 0 ? kOk : r.verify == "not compared" ? kDim : kErr, "Verify: %s", r.verify.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
		ImGui::PopID();
	}
}

// ---- Log and About ----------------------------------------------------------------------------

void App::DrawMobileLog() {
	// Log and About share the page, as two segments.
	const float w2 = SplitWidth(2);
	auto segment = [&](const char *label, bool on) {
		ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		bool r = BigButton(label, w2, 2.2f) && !TouchScrolled();
		ImGui::PopStyleColor();
		return r;
	};
	if (segment("Log", mobilePage_ == 3))
		mobilePage_ = 3;
	ImGui::SameLine();
	if (segment("About", mobilePage_ == 4))
		mobilePage_ = 4;
	ImGui::Spacing();
	if (mobilePage_ == 4) {
		DrawAbout();
		return;
	}

	unseenErrors_ = 0;
	if (BigButton("Copy all", w2, 2.2f) && !TouchScrolled()) {
		std::string all;
		std::lock_guard<std::mutex> g(logMutex_);
		for (auto &l : log_)
			all += l.time + "  " + l.text + "\n";
		ImGui::SetClipboardText(all.c_str());
	}
	ImGui::SameLine();
	if (BigButton("Clear", w2, 2.2f) && !TouchScrolled()) {
		std::lock_guard<std::mutex> g(logMutex_);
		log_.clear();
	}
	CheckboxWrap("Follow new lines", &logAutoScroll_);
	ImGui::Separator();

	const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetFontSize();
	{
		// Wrapped lines have different heights, so no clipper: the last 400 lines are plenty on a phone
		// (Copy all takes everything).
		std::lock_guard<std::mutex> g(logMutex_);
		const size_t from = log_.size() > 400 ? log_.size() - 400 : 0;
		if (from > 0)
			ImGui::TextDisabled("(%zu earlier lines: Copy all has them)", from);
		const float small = ImGui::GetStyle().FontSizeBase * 0.8f;
		for (size_t i = from; i < log_.size(); ++i) {
			const LogLine &l = log_[i];
			ImGui::PushFont(nullptr, small);
			ImGui::TextDisabled("%s", l.time.c_str());
			ImGui::PopFont();
			const ImVec4 col = l.level == LogLevel::Error ? kErr : l.level == LogLevel::Warning ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text);
			ImGui::TextColored(col, "%s", l.text.c_str());
		}
	}
	if (logAutoScroll_ && atBottom && !TouchScrollAnimating())
		ImGui::SetScrollHereY(1.0f);
}

}  // namespace isopk::gui
