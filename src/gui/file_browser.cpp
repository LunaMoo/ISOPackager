#include "gui/file_browser.h"

#include <algorithm>
#include <cstdlib>

#include "imgui.h"

#include "core/formats.h"
#include "core/util.h"

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
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.8f, vp->Size.y * 0.8f), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("##filebrowser", &open_, ImGuiWindowFlags_NoSavedSettings)) {
		open_ = false;
		return false;
	}
	ImGui::TextUnformatted(title_.c_str());
	ImGui::Separator();

	// Path bar.
	if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
		fs::path parent = U8Path(dir_).parent_path();
		if (!parent.empty())
			Navigate(PathU8(parent));
	}
	ImGui::SetItemTooltip("Parent folder");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 6);
	char buf[2048];
	snprintf(buf, sizeof(buf), "%s", pathEdit_.c_str());
	if (ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
		Navigate(buf);
	else
		pathEdit_ = buf;
	ImGui::SameLine();
	if (ImGui::Button("Refresh"))
		Refresh();
	if (mode_ == PickFiles) {
		ImGui::SameLine();
		if (ImGui::Checkbox("All files", &showAll_))
			Refresh();
	}

	const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
	// Places.
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
			std::string label = (e.dir ? "[DIR] " : "      ") + e.name + "##" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), selected_[i], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
				ImGuiIO &io = ImGui::GetIO();
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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
	if (entries_.empty())
		ImGui::TextDisabled(mode_ == PickFolder ? "(no subfolders)" : mode_ == PickAny ? "(empty folder)" : "(no disc images here)");
	ImGui::EndChild();
	if (!navigateTo.empty())
		Navigate(navigateTo);

	if (!error_.empty())
		ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
	int count = (int)std::count(selected_.begin(), selected_.end(), true);
	if (mode_ == PickFiles || mode_ == PickAny) {
		ImGui::Text("%d selected (click to select several; double-click a folder to open it)", count);
		if (mode_ == PickFiles && ImGui::Button("Select all images")) {
			for (size_t i = 0; i < entries_.size(); ++i)
				selected_[i] = entries_[i].image;
		}
		if (mode_ == PickAny && ImGui::Button("Select all")) {
			std::fill(selected_.begin(), selected_.end(), true);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(count == 0);
		if (ImGui::Button("Add selected")) {
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
		ImGui::Text("Folder: %s", target.c_str());
		if (ImGui::Button("Use this folder")) {
			result_ = { target };
			confirmed = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
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
