// A file / folder picker drawn with ImGui, for platforms without a native dialog (Linux without a
// portal, FreeBSD, Android, iOS, UWP) or when the user prefers it.
#pragma once

#include <string>
#include <vector>

namespace isopk::gui {

class FileBrowser {
public:
	enum Mode { PickFiles, PickFolder };

	// Opens the browser; Draw() must then be called every frame.
	void Open(Mode mode, const std::string &title, const std::string &startDir = "");
	// Draws it while open. Returns true on the frame the user confirmed; Result() then has the paths.
	bool Draw();
	bool IsOpen() const { return open_; }
	const std::vector<std::string> &Result() const { return result_; }

	// Folders offered on the left (home, drives, storage roots).
	void SetPlaces(const std::vector<std::pair<std::string, std::string>> &namedPaths) { places_ = namedPaths; }
	std::string CurrentDir() const { return dir_; }

private:
	struct Entry {
		std::string name;
		bool dir = false, image = false;
		uint64_t size = 0;
	};
	void Refresh();
	void Navigate(const std::string &dir);

	Mode mode_ = PickFiles;
	std::string title_, dir_, pathEdit_, error_;
	bool open_ = false, openRequested_ = false, showAll_ = false;
	std::vector<Entry> entries_;
	std::vector<bool> selected_;
	std::vector<std::string> result_;
	std::vector<std::pair<std::string, std::string>> places_;
	int lastClicked_ = -1;
};

std::vector<std::pair<std::string, std::string>> DefaultPlaces();

}  // namespace isopk::gui
