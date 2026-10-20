#include "gui/file_browser.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "imgui.h"

#include "core/formats.h"
#include "core/util.h"
#include "gui/widgets.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace isopk::gui {

std::vector<std::pair<std::string, std::string>> DefaultPlaces() {
	std::vector<std::pair<std::string, std::string>> p;
#if defined(_WIN32) && (!defined(WINAPI_FAMILY) || WINAPI_FAMILY != WINAPI_FAMILY_APP)
	if (const char *home = getenv("USERPROFILE")) {
		p.push_back({ "Home", home });
		p.push_back({ "Downloads", PathU8(U8Path(home) / "Downloads") });
	}
	DWORD drives = GetLogicalDrives();
	for (int i = 0; i < 26; ++i)
		if (drives & (1u << i))
			p.push_back({ std::string(1, (char)('A' + i)) + ":", std::string(1, (char)('A' + i)) + ":\\" });
#elif defined(__ANDROID__)
	p.push_back({ "Storage", "/storage/emulated/0" });
	p.push_back({ "Downloads", "/storage/emulated/0/Download" });
	p.push_back({ "PSP folder", "/storage/emulated/0/PSP" });
	p.push_back({ "SD cards", "/storage" });
#else
	if (const char *home = getenv("HOME")) {
		p.push_back({ "Home", home });
		p.push_back({ "Downloads", PathU8(U8Path(home) / "Downloads") });
#if defined(__APPLE__)
		p.push_back({ "Documents", PathU8(U8Path(home) / "Documents") });
#endif
	}
	p.push_back({ "/", "/" });
#if defined(__APPLE__)
	p.push_back({ "Volumes", "/Volumes" });
#else
	p.push_back({ "Media", "/media" });
	p.push_back({ "Mounts", "/mnt" });
#endif
#endif
	return p;
}

void FileBrowser::Open(Mode mode, const std::string &title, const std::string &startDir) {
	mode_ = mode;
	title_ = title;
	result_.clear();
	error_.clear();
	if (places_.empty())
		places_ = DefaultPlaces();
	std::string start = startDir;
	std::error_code ec;
	if (start.empty() || !fs::is_directory(U8Path(start), ec))
		start = dir_;
	if (start.empty() || !fs::is_directory(U8Path(start), ec))
		start = places_.empty() ? PathU8(fs::current_path(ec)) : places_.front().second;
	Navigate(start);
	openRequested_ = true;
}

void FileBrowser::Navigate(const std::string &dir) {
	std::error_code ec;
	fs::path p = U8Path(dir);
	if (!fs::is_directory(p, ec)) {
		error_ = "Not a folder: " + dir;
		return;
	}
	dir_ = PathU8(fs::absolute(p, ec).lexically_normal());
	pathEdit_ = dir_;
	lastClicked_ = -1;
	Refresh();
}

void FileBrowser::Refresh() {
	entries_.clear();
	error_.clear();
	std::error_code ec;
	for (auto it = fs::directory_iterator(U8Path(dir_), fs::directory_options::skip_permission_denied, ec);
		 !ec && it != fs::directory_iterator(); it.increment(ec)) {
		Entry e;
		e.name = PathU8(it->path().filename());
		if (e.name.empty() || e.name[0] == '.')
			continue;
		std::error_code ec2;
		e.dir = it->is_directory(ec2);
		if (!e.dir) {
			e.image = IsImageExtension(ToLower(PathU8(it->path().extension())));
			if (mode_ == PickFolder || (mode_ == PickFiles && !e.image && !showAll_))
				continue;
			e.size = it->file_size(ec2);
		}
		entries_.push_back(e);
	}
	if (ec)
		error_ = ec.message();
	std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
		if (a.dir != b.dir)
			return a.dir;
		return ToLower(a.name) < ToLower(b.name);
	});
	selected_.assign(entries_.size(), false);
}

bool FileBrowser::Draw() {
	if (openRequested_) {
		ImGui::OpenPopup("##filebrowser");
		openRequested_ = false;
		open_ = true;
	}
	if (!open_)
		return false;
	bool confirmed = false;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	if (mobile_) {
		ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
	} else {
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.8f, vp->Size.y * 0.8f), ImGuiCond_Appearing);
	}
	if (!ImGui::BeginPopupModal("##filebrowser", &open_, ImGuiWindowFlags_NoSavedSettings | (mobile_ ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize : 0))) {
		open_ = false;
		return false;
	}
	if (mobile_)
		ImGui::TextWrapped("%s", title_.c_str());
	else
		ImGui::TextUnformatted(title_.c_str());
	ImGui::Separator();

	if (mobile_) {
		// Phone: the places as a drop-down, then Up beside the path, Refresh and All files below.
		ImGui::SetNextItemWidth(-FLT_MIN);
		std::string cur = "Places";
		for (auto &pl : places_)
			if (pl.second == dir_)
				cur = pl.first;
		if (ImGui::BeginCombo("##places", cur.c_str())) {
			for (auto &pl : places_) {
				std::error_code ec;
				if (!fs::is_directory(U8Path(pl.second), ec))
					continue;
				if (ImGui::Selectable(pl.first.c_str(), dir_ == pl.second))
					Navigate(pl.second);
			}
			ImGui::EndCombo();
		}
	}

	// Path bar.
	if (mobile_ ? ImGui::Button("Up", ImVec2(ImGui::GetFrameHeight() * 1.6f, 0)) : ImGui::ArrowButton("##up", ImGuiDir_Up)) {
		fs::path parent = U8Path(dir_).parent_path();
		if (!parent.empty())
			Navigate(PathU8(parent));
	}
	ImGui::SetItemTooltip("Parent folder");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(mobile_ ? -FLT_MIN : -ImGui::GetFrameHeight() * 6);
	char buf[2048];
	snprintf(buf, sizeof(buf), "%s", pathEdit_.c_str());
	if (ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
		Navigate(buf);
	else
		pathEdit_ = buf;
	if (!mobile_)
		ImGui::SameLine();
	if (ImGui::Button("Refresh"))
		Refresh();
	if (mode_ == PickFiles) {
		ImGui::SameLine();
		if (ImGui::Checkbox("All files", &showAll_))
			Refresh();
	}

	const float footer = ImGui::GetFrameHeightWithSpacing() * (mobile_ ? 3.4f : 2.2f);
	// Places.
	if (!mobile_) {
		ImGui::BeginChild("##places", ImVec2(ImGui::GetFontSize() * 10, -footer), ImGuiChildFlags_Borders);
		for (auto &pl : places_) {
			std::error_code ec;
			if (!fs::is_directory(U8Path(pl.second), ec))
				continue;
			if (ImGui::Selectable(pl.first.c_str(), dir_ == pl.second))
				Navigate(pl.second);
			ImGui::SetItemTooltip("%s", pl.second.c_str());
		}
		ImGui::EndChild();
		ImGui::SameLine();
	}

	// Entries.
	ImGui::BeginChild("##entries", ImVec2(0, -footer), ImGuiChildFlags_Borders);
	std::string navigateTo;
	if (ImGui::BeginTable("##list", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
		for (size_t i = 0; i < entries_.size(); ++i) {
			const Entry &e = entries_[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			std::string label = (e.dir ? (mobile_ ? "> " : "[DIR] ") : (mobile_ ? "  " : "      ")) + e.name + "##" + std::to_string(i);
			const float rowH = mobile_ ? ImGui::GetFrameHeight() : 0;
			if (ImGui::Selectable(label.c_str(), selected_[i], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH)) &&
				!(mobile_ && TouchScrolled())) {
				ImGuiIO &io = ImGui::GetIO();
				if (mobile_ && e.dir && mode_ != PickAny) {
					// One tap opens a folder (folder mode: "Use this folder" picks the one shown).
					navigateTo = PathU8(U8Path(dir_) / U8Path(e.name));
				} else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (e.dir)
						navigateTo = PathU8(U8Path(dir_) / U8Path(e.name));
					else if (mode_ == PickFiles || mode_ == PickAny) {
						result_ = { PathU8(U8Path(dir_) / U8Path(e.name)) };
						confirmed = true;
					}
				} else if (io.KeyShift && lastClicked_ >= 0) {
					int a = std::min<int>(lastClicked_, (int)i), b = std::max<int>(lastClicked_, (int)i);
					for (int k = a; k <= b; ++k)
						selected_[k] = true;
				} else if (io.KeyCtrl || mode_ == PickFiles || mode_ == PickAny) {
					selected_[i] = !selected_[i];
					lastClicked_ = (int)i;
				} else {
					std::fill(selected_.begin(), selected_.end(), false);
					selected_[i] = true;
					lastClicked_ = (int)i;
				}
			}
			ImGui::TableNextColumn();
			if (!e.dir)
				ImGui::TextDisabled("%s", HumanBytes(e.size).c_str());
		}
		ImGui::EndTable();
	}
	if (mobile_)
		TouchScroll();
	if (entries_.empty())
		ImGui::TextDisabled(mode_ == PickFolder ? "(no subfolders)" : mode_ == PickAny ? "(empty folder)" : "(no disc images here)");
	ImGui::EndChild();
	if (!navigateTo.empty())
		Navigate(navigateTo);

	if (!error_.empty())
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
	int count = (int)std::count(selected_.begin(), selected_.end(), true);
	if (mode_ == PickFiles || mode_ == PickAny) {
		if (mobile_)
			ImGui::TextWrapped(mode_ == PickAny ? "%d selected. Tap to select, double-tap a folder to open it." : "%d selected. Tap files to select them, a folder to open it.", count);
		else
			ImGui::Text("%d selected (click to select several; double-click a folder to open it)", count);
		const float w3 = mobile_ ? std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3) : 0;
		if (mode_ == PickFiles && ImGui::Button(mobile_ ? "Select all" : "Select all images", ImVec2(w3, 0))) {
			for (size_t i = 0; i < entries_.size(); ++i)
				selected_[i] = entries_[i].image;
		}
		if (mode_ == PickAny && ImGui::Button("Select all", ImVec2(w3, 0))) {
			std::fill(selected_.begin(), selected_.end(), true);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(count == 0);
		std::string add = mobile_ ? Fmt("Add (%d)###add", count) : std::string("Add selected");
		if (ImGui::Button(add.c_str(), ImVec2(w3, 0))) {
			for (size_t i = 0; i < entries_.size(); ++i)
				if (selected_[i])
					result_.push_back(PathU8(U8Path(dir_) / U8Path(entries_[i].name)));
			confirmed = true;
		}
		ImGui::EndDisabled();
	} else {
		std::string target = dir_;
		for (size_t i = 0; i < entries_.size(); ++i)
			if (selected_[i])
				target = PathU8(U8Path(dir_) / U8Path(entries_[i].name));
		if (mobile_)
			ImGui::TextWrapped("Folder: %s", target.c_str());
		else
			ImGui::Text("Folder: %s", target.c_str());
		if (ImGui::Button("Use this folder", ImVec2(mobile_ ? std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2) : 0, 0))) {
			result_ = { target };
			confirmed = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(mobile_ ? ImGui::GetContentRegionAvail().x : 0, 0))) {
		result_.clear();
		open_ = false;
		ImGui::CloseCurrentPopup();
	}
	if (confirmed) {
		open_ = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
	return confirmed;
}

}  // namespace isopk::gui
