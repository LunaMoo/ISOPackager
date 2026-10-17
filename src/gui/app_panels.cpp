// The Settings, Benchmark, Log and About tabs, the status bar and the dialogs.
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

static const ImVec4 kOk(0.40f, 0.85f, 0.50f, 1), kWarn(0.98f, 0.75f, 0.30f, 1), kErr(0.98f, 0.40f, 0.40f, 1);

// ---- Settings ---------------------------------------------------------------------------------

// One mCSO container (CSO, ZSO, CSO v2, DAX): only the options it takes.
bool App::DrawCsoSection(Format f, const char *header, bool open) {
	if (!ImGui::CollapsingHeader(header, open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
		return false;
	const float em = ImGui::GetFontSize(), labelW = em * 12;
	auto row = [&](const char *label) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine(labelW);
		ImGui::SetNextItemWidth(em * 14);
	};
	const bool zso = f == Format::ZSO, v2 = f == Format::CSO2, dax = f == Format::DAX;
	const bool deflate = !zso, lz4 = zso || v2;
	bool changed = false;
	CsoSettings &c = settings_.Cso(f);
	ImGui::PushID(FormatKey(f));
	ImGui::Indent();
	const char *defLabel = zso ? "Default (lz4 HC, 64K blocks)" : dax ? "Default (libdeflate)" : "Default (libdeflate, 64K blocks)";
	changed |= ImGui::Checkbox(defLabel, &c.useDefault);
	if (f == Format::CSO)
		Tip("Fast to compress and quick to read in PPSSPP. Some old PSP custom firmware can't read libdeflate's output; untick to choose methods "
			"yourself.");
	else
		Tip("Untick to choose the methods yourself.");
	if (!c.useDefault) {
		row("Preset");
		int p = (int)c.preset;
		const char *presets = zso ? "Fast (lz4)\0Standard (lz4 + lz4 HC)\0Maximum (+ lz4 HC brute force)\0Custom\0"
			: v2 ? "Fast (zlib + lz4)\0Standard (deflate + lz4 HC)\0Maximum (every method)\0Custom\0"
				 : "Fast (zlib only)\0Standard (zlib + brute force + 7-zip)\0Maximum (+ Zopfli + libdeflate, slow)\0Custom\0";
		if (ImGui::Combo("##preset", &p, presets)) {
			c.preset = (CsoPreset)p;
			changed = true;
		}
		if (!dax) {
			row("Block size");
			changed |= BlockSizeCombo("##block", &c.blockSize, 2048, 262144, true, "Automatic (2K, 16K above 2 GB)");
			HelpMarker("Bigger blocks compress better but every read decompresses a whole block. PPSSPP handles any size; real PSPs need 2K.");
		}
		if (c.preset == CsoPreset::Custom) {
			if (deflate) {
				ImGui::TextDisabled("Deflate methods: every enabled one is tried on each block, the smallest wins");
				changed |= ImGui::Checkbox("zlib", &c.zlib);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("zlib brute force", &c.zlibBrute);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("7-zip deflate", &c.sevenZip);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("Zopfli", &c.zopfli);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("libdeflate", &c.libdeflate);
			}
			if (lz4) {
				ImGui::TextDisabled(v2 ? "lz4 methods (used where they beat deflate)" : "lz4 methods: the smallest result wins");
				changed |= ImGui::Checkbox("lz4", &c.lz4);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("lz4 HC", &c.lz4hc);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("lz4 HC brute force", &c.lz4Brute);
			}
		}
		if (deflate) {
			row("Zopfli splits");
			int sp = (int)c.zopfliSplits;
			if (ImGui::SliderInt("##splits", &sp, 0, 60, sp == 0 ? "default (15)" : "%d")) {
				c.zopfliSplits = (uint32_t)sp;
				changed = true;
			}
			HelpMarker("Deflate blocks Zopfli may split each block into. Larger blocks gain from more splits (img_bench set B uses 20).");
			row("libdeflate pre-check");
			float pc = (float)c.precheck;
			if (ImGui::SliderFloat("##precheck", &pc, 0, 100, pc <= 0 ? "off" : "%.0f%%")) {
				c.precheck = pc < 1 ? 0 : pc;
				changed = true;
			}
			HelpMarker("Skip Zopfli and 7-zip on blocks libdeflate can't get below this share of the block (video, audio): much faster Maximum "
					   "compression for nearly the same size (img_bench sets C and D use 98%).");
		}
		changed |= ImGui::Checkbox("Compress every block, even when it doesn't save space", &c.forceAll);
	}
	ImGui::TextDisabled("%s", c.Describe(f).c_str());
	DrawAddToBenchmark(MakeMcsoCase(f, c), FormatKey(f));
	ImGui::Unindent();
	ImGui::PopID();
	return changed;
}

// One iRVZ container (RVZ, WIA): only the methods and chunk sizes it takes.
bool App::DrawRvzSection(Format f, const char *header, bool open) {
	if (!ImGui::CollapsingHeader(header, open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
		return false;
	const float em = ImGui::GetFontSize(), labelW = em * 12;
	auto row = [&](const char *label) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine(labelW);
		ImGui::SetNextItemWidth(em * 14);
	};
	const bool wia = f == Format::WIA;
	bool changed = false;
	RvzSettings &c = settings_.Rvz(f);
	ImGui::PushID(FormatKey(f));
	ImGui::Indent();
	changed |= ImGui::Checkbox(wia ? "Default (LZMA2 level 5, 2M chunks)" : "Default (LZMA2 level 5, 128K chunks)", &c.useDefault);
	Tip(wia ? "Untick to choose the method, level and chunk size yourself." : "Small files that Dolphin reads quickly. Untick to choose the method, "
		"level and chunk size yourself.");
	if (!c.useDefault) {
		if (!RvzMethodAllowed(f, c.method)) {
			c.method = RVZ_LZMA2;
			changed = true;
		}
		row("Method");
		if (ImGui::BeginCombo("##rvzmethod", RvzMethodName(c.method))) {
			const char *notes[] = { "store the data as it is", "drops runs of zeros, no compression", "", "", "", "fastest to read" };
			for (int m = RVZ_NONE; m <= RVZ_ZSTD; ++m) {
				if (!RvzMethodAllowed(f, m))
					continue;
				std::string l = std::string(RvzMethodName(m)) + (notes[m][0] ? std::string("  (") + notes[m] + ")" : std::string());
				if (ImGui::Selectable(l.c_str(), c.method == m)) {
					c.method = m;
					int lo, hi;
					RvzMethodLevels(m, &lo, &hi);
					if (lo <= hi)
						c.level = std::clamp(c.level, lo, hi);
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		int lo, hi;
		RvzMethodLevels(c.method, &lo, &hi);
		if (lo <= hi) {
			row("Level");
			if (ImGui::SliderInt("##rvzlevel", &c.level, lo, hi))
				changed = true;
			HelpMarker(c.method == RVZ_ZSTD ? "Higher compresses smaller and slower; negative levels trade size for speed."
											: "Higher compresses smaller and slower.");
		}
		row("Chunk size");
		auto label = [&](uint32_t v) {
			if (v == 0)
				return std::string(wia ? "Default (2M)" : "Default (128K)");
			return v >= (1u << 20) ? Fmt("%uM", v >> 20) : Fmt("%uK", v >> 10);
		};
		if (wia && c.chunk && c.chunk % (2u << 20)) {
			c.chunk = 0;
			changed = true;
		}
		if (ImGui::BeginCombo("##rvzchunk", label(c.chunk).c_str())) {
			std::vector<uint32_t> sizes = { 0 };
			if (!wia)
				for (uint32_t v = 32u << 10; v < (2u << 20); v <<= 1)
					sizes.push_back(v);
			for (uint32_t v = 2u << 20; v <= (32u << 20); v <<= 1)
				sizes.push_back(v);
			for (uint32_t v : sizes)
				if (ImGui::Selectable(label(v).c_str(), c.chunk == v)) {
					c.chunk = v;
					changed = true;
				}
			ImGui::EndCombo();
		}
		HelpMarker(wia ? "WIA chunks are multiples of 2M. Bigger chunks compress better; every read decompresses a whole chunk."
					   : "Bigger chunks compress better; every read decompresses a whole chunk.");
	}
	ImGui::TextDisabled("%s", c.Describe(f).c_str());
	DrawAddToBenchmark(MakeRvzCase(f, c), FormatKey(f));
	ImGui::Unindent();
	ImGui::PopID();
	return changed;
}

void App::DrawSettings() {
	bool changed = false;
	const float em = ImGui::GetFontSize();
	const float labelW = em * 12;
	auto row = [&](const char *label) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine(labelW);
		ImGui::SetNextItemWidth(em * 14);
	};

	// --- Output
	if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		row("When the output exists");
		int c = (int)settings_.out.conflict;
		if (ImGui::Combo("##conflict", &c, "Skip it\0Overwrite it\0Rename the new one\0")) {
			settings_.out.conflict = (Conflict)c;
			changed = true;
		}
		changed |= ImGui::Checkbox("Recreate the scanned folders under the output folder", &settings_.out.mirrorTree);
		Tip("Adding D:\\Games with a subfolder RPG\\ puts its outputs in <output>\\RPG\\ instead of all in one folder.");
		changed |= ImGui::Checkbox("Add folders recursively", &settings_.recursive);
		changed |= ImGui::Checkbox("Delete sources after a successful conversion", &settings_.out.deleteSource);
		Tip("Only after the output was written completely and, with Verify on, compared with the source.");
		ImGui::Unindent();
	}

	// --- Engine
	if (ImGui::CollapsingHeader("Performance and safety", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		changed |= ImGui::Checkbox("Verify every output", &settings_.engine.verify);
		Tip("Decode the new image again and compare it byte for byte with the source. Uses no disk space: the comparison runs as it decodes.");
		changed |= ImGui::Checkbox("Repack compressed images through RAM", &settings_.engine.repackInRam);
		Tip("CHD -> CSO and the like: decompress into memory and compress from there, without a temporary ISO on disk. "
			"Images that don't fit the RAM budget fall back to a temporary ISO.");

		row("RAM budget");
		bool autoRam = settings_.engine.ramBudget == 0;
		if (ImGui::Checkbox("Automatic##ram", &autoRam)) {
			settings_.engine.ramBudget = autoRam ? 0 : (DefaultRamBudget() >> 20 << 20);
			changed = true;
		}
		if (!autoRam) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(em * 10);
			int mb = (int)(settings_.engine.ramBudget >> 20);
			int maxMb = (int)std::max<uint64_t>(TotalRam() >> 20, 1024);
			if (ImGui::SliderInt("##rammb", &mb, 256, maxMb, "%d MB", ImGuiSliderFlags_Logarithmic)) {
				settings_.engine.ramBudget = (uint64_t)std::max(mb, 256) << 20;
				changed = true;
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("now %s of %s total, %s free", HumanBytes(RamBudget::Get().Capacity()).c_str(), HumanBytes(TotalRam()).c_str(),
			HumanBytes(AvailableRam()).c_str());

		row("Temporary ISOs in");
		std::string tmp = settings_.engine.tempDir;
		ImGui::SetNextItemWidth(em * 22);
		if (InputStringHint("##tempdir", "(the output folder)", &tmp)) {
			settings_.engine.tempDir = tmp;
			changed = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Browse...##tmp"))
			PickFolder([this](const std::string &d) {
				settings_.engine.tempDir = d;
				SettingsChanged();
			});

		row("Files at once");
		int jobs = (int)settings_.engine.jobs;
		if (ImGui::SliderInt("##jobs", &jobs, 1, (int)std::max(2u, CpuCount()))) {
			settings_.engine.jobs = (unsigned)jobs;
			changed = true;
		}
		HelpMarker("Each conversion already uses all cores, so 1 is usually fastest for compression. More helps when extracting many small images, "
				   "or mixing CHD/PBP jobs (CSO/ZSO jobs take turns: maxcso runs one task at a time).");
		row("Threads per job");
		int th = (int)settings_.engine.threads;
		if (ImGui::SliderInt("##threads", &th, 0, (int)CpuCount() * 2, th == 0 ? "all cores" : "%d")) {
			settings_.engine.threads = (unsigned)th;
			changed = true;
		}
		HelpMarker("iPBP and mCSO worker threads. mCSO picks this up at the next start of ISOPackager.");
		changed |= ImGui::Checkbox("Run at lower priority", &settings_.engine.lowPriority);
		Tip("Keeps the computer responsive while compressing.");
		ImGui::Unindent();
	}

#ifdef ISOPK_WITH_CSO
	changed |= DrawCsoSection(Format::CSO, "CSO (maxcso)", true);
	changed |= DrawCsoSection(Format::ZSO, "ZSO (maxcso)", true);
#endif

	// --- CHD
	if (ImGui::CollapsingHeader("CHD (CHDmodern)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		ChdSettings &c = settings_.chd;
		row("Hunk size");
		changed |= BlockSizeCombo("##hunk", &c.hunkBytes, 2048, 1u << 20, true, "Default (4K)");
		HelpMarker("Bigger hunks compress better; every read decompresses a whole hunk.");
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Codecs");
		ImGui::SameLine(labelW);
		const float codecX = ImGui::GetCursorPosX();
		bool def = c.codecs.empty();
		if (ImGui::Checkbox("chdman default (lzma, zlib, huff, flac)", &def)) {
			if (def)
				c.codecs.clear();
			else
				c.SetCodecs("lzma+zlib");
			changed = true;
		}
		if (!def) {
			// Own row, lined up under the default checkbox.
			ImGui::SetCursorPosX(codecX);
			bool first = true;
			for (const std::string &name : ChdCodecChoices()) {
				if (!first)
					ImGui::SameLine();
				first = false;
				uint32_t cc = FourCC(name.c_str());
				bool on = std::find(c.codecs.begin(), c.codecs.end(), cc) != c.codecs.end();
				ImGui::PushID(name.c_str());
				ImGui::BeginDisabled(!on && c.codecs.size() >= 4);
				if (ImGui::Checkbox(name.c_str(), &on)) {
					if (on)
						c.codecs.push_back(cc);
					else
						c.codecs.erase(std::remove(c.codecs.begin(), c.codecs.end(), cc), c.codecs.end());
					if (c.codecs.empty())
						c.codecs.push_back(FourCC("zlib"));
					changed = true;
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::SetCursorPosX(codecX);
			ImGui::TextDisabled("Each hunk uses whichever codec makes it smallest. zstd decodes fastest; lzma is smallest. Up to 4.");
		}
		DrawAddToBenchmark(MakeChdCase(c), "chd");
		ImGui::Unindent();
	}

	// --- PBP
	if (ImGui::CollapsingHeader("EBOOT.PBP (iPBP)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent();
		PbpSettings &c = settings_.pbp;
		row("Compression");
		if (ImGui::Combo("##pbpmode", &c.compress, "None\0LZRC (like sign_np)\0LZRC maximum (5-10x slower)\0"))
			changed = true;
		HelpMarker("Maximum is still plain LZRC that real PSPs decode, just searched much harder.");
		row("Layout");
		int lay = (int)c.layout;
		if (ImGui::Combo("##layout", &lay, "Game.pbp\0Game/EBOOT.PBP\0")) {
			c.layout = (PbpLayout)lay;
			changed = true;
		}
		HelpMarker("Real PSPs need PSP/GAME/<folder>/EBOOT.PBP; PPSSPP loads both.");
		row("Content ID");
		ImGui::SetNextItemWidth(em * 22);
		changed |= InputStringHint("##cid", "UP0000-<DISC_ID>_00-0000000000000000", &c.contentId);
		row("Boot screen PNG");
		ImGui::SetNextItemWidth(em * 22);
		changed |= InputStringHint("##png", "(none) 480x272", &c.startDatPng);
		ImGui::SameLine();
		if (ImGui::Button("Browse...##png")) {
			browserThen_ = [this](const std::vector<std::string> &r) {
				if (!r.empty()) {
					settings_.pbp.startDatPng = r[0];
					SettingsChanged();
				}
			};
			std::vector<std::string> files;
			if (hooks_.pickFiles && hooks_.pickFiles(&files)) {
				browserThen_(files);
				browserThen_ = nullptr;
			} else {
				browser_.Open(FileBrowser::PickFiles, "Boot screen PNG");
			}
		}
		changed |= ImGui::Checkbox("Trim extracted ISOs to their ISO9660 size", &c.trimOnExtract);
		Tip("A PBP stores whole 16-sector blocks; trimming gives back the exact dump size.");
		changed |= ImGui::Checkbox("Check block MACs when reading PBPs", &c.verifyMacs);
		DrawAddToBenchmark(MakePbpCase(c), "pbp");
		ImGui::Unindent();
	}

	changed |= DrawRvzSection(Format::RVZ, "RVZ (iRVZ, GameCube and Wii)", true);
	changed |= DrawRvzSection(Format::WIA, "WIA (iRVZ, GameCube and Wii)", false);
#ifdef ISOPK_WITH_CSO
	changed |= DrawCsoSection(Format::CSO2, "CSO v2 (maxcso, experimental)", false);
	changed |= DrawCsoSection(Format::DAX, "DAX (maxcso)", false);
#endif

	// --- Appearance
	if (ImGui::CollapsingHeader("Appearance")) {
		ImGui::Indent();
		row("Theme");
		if (ImGui::Combo("##theme", &settings_.uiTheme, "Dark\0Light\0Classic\0")) {
			ApplyUiStyle();
			changed = true;
		}
		row("Interface scale");
		if (ImGui::SliderFloat("##scale", &settings_.uiScale, 0.6f, 2.5f, "%.2fx")) {
			changed = true;
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
			ApplyUiStyle();
		ImGui::Unindent();
	}

	ImGui::Spacing();
	if (ImGui::Button("Restore defaults")) {
		Settings d;
		d.uiTheme = settings_.uiTheme;
		d.uiScale = settings_.uiScale;
		d.benchCases = settings_.benchCases;
		settings_ = d;
		addTarget_ = settings_.target;
		changed = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Saved automatically to %s", settingsPath_.c_str());
	if (changed)
		SettingsChanged();
}

// ---- Benchmark --------------------------------------------------------------------------------

void App::SaveBenchCases() {
	settings_.benchCases.clear();
	for (const BenchCase &c : benchCases_)
		settings_.benchCases.push_back(SerializeBenchCase(c));
	SettingsChanged();
}

void App::AddToBenchmark(const BenchCase &c) {
	const std::string key = SerializeBenchCase(c);
	for (const BenchCase &b : benchCases_)
		if (SerializeBenchCase(b) == key) {
			benchNotice_ = std::string("Already in the benchmark: ") + FormatName(c.format) + " " + c.label;
			benchNoticeAt_ = ImGui::GetTime();
			return;
		}
	benchCases_.push_back(c);
	SaveBenchCases();
	benchNotice_ = std::string("Added to the benchmark: ") + FormatName(c.format) + " " + c.label;
	benchNoticeAt_ = ImGui::GetTime();
}

void App::DrawAddToBenchmark(const BenchCase &c, const char *id, const std::function<void()> &extra) {
	ImGui::PushID(id);
	ImGui::BeginDisabled(benchRunning_);
	if (ImGui::Button("Add to benchmark"))
		AddToBenchmark(c);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Adds \"%s %s\" to the Benchmark tab's list.", FormatName(c.format), c.label.c_str());
	if (extra)
		extra();
	if (!benchNotice_.empty() && ImGui::GetTime() - benchNoticeAt_ < 3.0 &&
		benchNotice_.find(std::string(FormatName(c.format)) + " " + c.label) != std::string::npos) {
		ImGui::SameLine();
		ImGui::TextColored(kOk, "%s", benchNotice_.c_str());
	}
	ImGui::PopID();
}

void App::StartBenchmark() {
	if (benchRunning_)
		return;
	BenchConfig cfg = benchCfg_;
	cfg.cases = benchCases_;
	cfg.threads = settings_.engine.threads;
	if (cfg.cases.empty() || benchInputs_.empty()) {
		Log(LogLevel::Warning, "Benchmark: pick at least one image and one setting.");
		return;
	}
	if (benchThread_.joinable())
		benchThread_.join();
	std::vector<InputFile> inputs = benchInputs_;
	std::sort(inputs.begin(), inputs.end(), [](const InputFile &a, const InputFile &b) { return a.size < b.size; });
	benchCancel_ = false;
	benchRunning_ = true;
	{
		std::lock_guard<std::mutex> g(benchMutex_);
		benchWhat_ = "Starting...";
		benchStep_ = benchTotal_ = 0;
	}
	ApplyEngineSettings(settings_.engine);
	benchThread_ = std::thread([this, inputs, cfg] {
		BenchCallbacks cb;
		cb.row = [this](const BenchRow &r) {
			std::lock_guard<std::mutex> g(benchMutex_);
			benchRows_.push_back(r);
		};
		cb.progress = [this](const std::string &what, double step, double total) {
			std::lock_guard<std::mutex> g(benchMutex_);
			benchWhat_ = what;
			benchStep_ = step;
			benchTotal_ = total;
		};
		RunBenchmark(inputs, cfg, cb, &benchCancel_);
		benchRunning_ = false;
	});
}

void App::DrawBenchmark() {
	const float em = ImGui::GetFontSize();
	const bool running = benchRunning_;

	ImGui::BeginChild("##benchcfg", ImVec2(em * 24, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
	ImGui::SeparatorText("Images");
	ImGui::BeginDisabled(running);
	if (ImGui::Button("Add files..."))
		PickFiles([this](const std::vector<std::string> &f) { ScanAsync(f, true); });
	ImGui::SameLine();
	if (ImGui::Button("Add folder..."))
		PickFolder([this](const std::string &d) { ScanAsync({ d }, true); });
	ImGui::SameLine();
	if (ImGui::Button("From queue")) {
		for (auto &j : queue_.Jobs()) {
			bool dup = false;
			for (auto &b : benchInputs_)
				dup |= b.path == j->input.path;
			if (!dup)
				benchInputs_.push_back(j->input);
		}
	}
	ImGui::EndDisabled();
	if (ImGui::BeginListBox("##benchinputs", ImVec2(-FLT_MIN, em * 5))) {
		int remove = -1;
		for (int i = 0; i < (int)benchInputs_.size(); ++i) {
			ImGui::PushID(i);
			std::string l = GameName(benchInputs_[i].path) + "  (" + FormatName(benchInputs_[i].format) + ", " + HumanBytes(benchInputs_[i].size) + ")";
			ImGui::Selectable(l.c_str());
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Remove"))
					remove = i;
				ImGui::EndPopup();
			}
			ImGui::SetItemTooltip("%s", benchInputs_[i].path.c_str());
			ImGui::PopID();
		}
		if (benchInputs_.empty())
			ImGui::TextDisabled("No images. Compressed ones are decoded first.");
		ImGui::EndListBox();
		if (remove >= 0 && !running)
			benchInputs_.erase(benchInputs_.begin() + remove);
	}

	ImGui::SeparatorText("Methods");
	{
		int remove = -1;
		const float rowH = ImGui::GetFrameHeight();
		if (ImGui::BeginChild("##benchcases", ImVec2(0, std::min(em * 14, std::max<float>(rowH * 2, (rowH + 4) * (float)benchCases_.size() + 8))),
				ImGuiChildFlags_Borders)) {
			for (int i = 0; i < (int)benchCases_.size(); ++i) {
				const BenchCase &c = benchCases_[i];
				ImGui::PushID(i);
				ImGui::BeginDisabled(running);
				if (ImGui::SmallButton("x"))
					remove = i;
				ImGui::EndDisabled();
				ImGui::SetItemTooltip("Remove from the benchmark");
				ImGui::SameLine();
				ImGui::TextColored(FormatColor(c.format), "%s", FormatName(c.format));
				ImGui::SameLine(em * 5.5f);
				ImGui::TextUnformatted(c.label.c_str());
				ImGui::PopID();
			}
			if (benchCases_.empty())
				ImGui::TextDisabled("Nothing to run yet.");
		}
		ImGui::EndChild();
		if (remove >= 0) {
			benchCases_.erase(benchCases_.begin() + remove);
			SaveBenchCases();
		}
	}
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("Add methods with \"Add to benchmark\" under each format in Settings.");
	ImGui::PopStyleColor();
	ImGui::BeginDisabled(running);
	if (ImGui::SmallButton("Defaults")) {
		benchCases_.clear();
		for (const std::string &line : DefaultBenchCases()) {
			BenchCase c;
			if (ParseBenchCase(line, &c))
				benchCases_.push_back(c);
		}
		SaveBenchCases();
	}
	ImGui::SetItemTooltip("libdeflate 256K CSO, chdman's default CHD and LZRC PBP");
	ImGui::EndDisabled();

	ImGui::BeginDisabled(running);
	ImGui::SeparatorText("Options");
	ImGui::Checkbox("Verify (compare decoded images)", &benchCfg_.verify);
	ImGui::Checkbox("Compress from RAM", &benchCfg_.sourceInRam);
	Tip("Load each image into memory first, so disk speed doesn't show in the compression times.");
	ImGui::SetNextItemWidth(em * 7);
	ImGui::InputInt("Random reads", &benchCfg_.reads, 500, 2000);
	benchCfg_.reads = std::clamp(benchCfg_.reads, 0, 1000000);
	Tip("Per image, each from a block or hunk not read before, so every read pays a full decompression (like an emulator seeking).");
	ImGui::SetNextItemWidth(-FLT_MIN);
	InputStringHint("##workdir", "results folder", &benchCfg_.workDir);
	ImGui::EndDisabled();

	ImGui::Spacing();
	if (!running) {
		ImGui::BeginDisabled(benchInputs_.empty());
		if (WideButton("Run benchmark", 10))
			StartBenchmark();
		ImGui::EndDisabled();
	} else {
		if (WideButton("Cancel", 10))
			benchCancel_ = true;
	}
	if (queue_.Busy() && !running) {
		ImGui::SameLine();
		ImGui::TextColored(kWarn, "Queue running: timings will suffer");
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##benchres", ImVec2(0, 0));
	std::vector<BenchRow> rows;
	std::string what;
	double step, total;
	{
		std::lock_guard<std::mutex> g(benchMutex_);
		rows = benchRows_;
		what = benchWhat_;
		step = benchStep_;
		total = benchTotal_;
	}
	if (running) {
		ImGui::TextUnformatted(what.c_str());
		ImGui::ProgressBar((float)step, ImVec2(-FLT_MIN, 0));
		ImGui::ProgressBar((float)total, ImVec2(-FLT_MIN, em * 0.4f), "");
	} else {
		if (ImGui::Button("Copy as CSV")) {
			std::string csv = BenchCsvHeader() + "\n";
			for (auto &r : rows)
				csv += BenchCsvLine(r) + "\n";
			ImGui::SetClipboardText(csv.c_str());
		}
		ImGui::SameLine();
		if (hooks_.reveal && ImGui::Button("Open results folder"))
			hooks_.reveal(benchCfg_.workDir.empty() ? DefaultBenchDir() : benchCfg_.workDir);
		ImGui::SameLine();
		if (ImGui::Button("Clear")) {
			std::lock_guard<std::mutex> g(benchMutex_);
			benchRows_.clear();
			rows.clear();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Results are also appended to summary.txt and results.csv.");
	}

	if (!rows.empty()) {
		ImGui::SeparatorText("Size vs speed");
		ImGui::SetNextItemWidth(em * 12);
		ImGui::Combo("x axis", &benchChartX_, "Compression MB/s\0Decode seconds\0Random read ms\0");
		DrawBenchChart(std::max(em * 12, ImGui::GetContentRegionAvail().y * 0.45f));
	}

	ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
		ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	if (!rows.empty() && ImGui::BeginTable("##benchrows", 10, tf)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Format");
		ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Output");
		ImGui::TableSetupColumn("of ISO", ImGuiTableColumnFlags_DefaultSort);
		ImGui::TableSetupColumn("Compress s");
		ImGui::TableSetupColumn("MB/s");
		ImGui::TableSetupColumn("Decode s");
		ImGui::TableSetupColumn("Read ms");
		ImGui::TableSetupColumn("Verify");
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
			const ImGuiTableColumnSortSpecs s = ss->Specs[0];
			auto key = [&](const BenchRow &r) -> double {
				switch (s.ColumnIndex) {
				case 3: return (double)r.output;
				case 4: return r.Ratio();
				case 5: return r.compSecs;
				case 6: return r.CompMBs();
				case 7: return r.decompSecs;
				case 8: return r.readMs;
				default: return 0;
				}
			};
			std::stable_sort(rows.begin(), rows.end(), [&](const BenchRow &a, const BenchRow &b) {
				bool less;
				if (s.ColumnIndex <= 2 || s.ColumnIndex == 9) {
					const std::string &x = s.ColumnIndex == 0 ? a.game : s.ColumnIndex == 1 ? a.format : s.ColumnIndex == 2 ? a.setting : a.verify;
					const std::string &y = s.ColumnIndex == 0 ? b.game : s.ColumnIndex == 1 ? b.format : s.ColumnIndex == 2 ? b.setting : b.verify;
					less = x < y;
				} else {
					less = key(a) < key(b);
				}
				return s.SortDirection == ImGuiSortDirection_Ascending ? less : !less && key(a) != key(b);
			});
		}
		for (const BenchRow &r : rows) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(r.game.c_str());
			ImGui::TableNextColumn();
			ImGui::TextColored(FormatColor(r.format), "%s", r.format.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(r.setting.c_str());
			if (r.failed) {
				ImGui::TableNextColumn();
				ImGui::TextColored(kErr, "FAILED: %s", r.error.c_str());
				continue;
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(HumanBytes(r.output).c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%.3f%%", r.Ratio());
			ImGui::TableNextColumn();
			ImGui::Text("%.1f", r.compSecs);
			ImGui::TableNextColumn();
			ImGui::Text("%.1f", r.CompMBs());
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", r.decompSecs);
			ImGui::TableNextColumn();
			if (r.readMs >= 0)
				ImGui::Text("%.3f", r.readMs);
			else
				ImGui::TextDisabled("-");
			ImGui::TableNextColumn();
			ImGui::TextColored(r.verify.rfind("OK", 0) == 0 ? kOk : r.verify == "not compared" ? ImVec4(0.6f, 0.6f, 0.6f, 1) : kErr, "%s", r.verify.c_str());
		}
		ImGui::EndTable();
	} else if (rows.empty() && !running) {
		ImGui::Spacing();
		ImGui::TextWrapped("Pick images and settings on the left and run. Each image is compressed with every setting, decoded again and compared, "
						   "and CSO/ZSO/CHD outputs get timed random reads. The chart then shows which settings give the smallest files for the time "
						   "they cost.");
	}
	ImGui::EndChild();
}

void App::DrawBenchChart(float height) {
	std::vector<BenchRow> rows;
	{
		std::lock_guard<std::mutex> g(benchMutex_);
		rows = benchRows_;
	}
	auto xval = [&](const BenchRow &r) -> double {
		switch (benchChartX_) {
		case 1: return r.decompSecs;
		case 2: return r.readMs;
		default: return r.CompMBs();
		}
	};
	std::vector<const BenchRow *> pts;
	double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
	for (auto &r : rows) {
		if (r.failed || xval(r) <= 0)
			continue;
		pts.push_back(&r);
		xmin = std::min(xmin, xval(r));
		xmax = std::max(xmax, xval(r));
		ymin = std::min(ymin, r.Ratio());
		ymax = std::max(ymax, r.Ratio());
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImVec2 size(ImGui::GetContentRegionAvail().x, height);
	ImGui::InvisibleButton("##chart", size);
	const bool hovered = ImGui::IsItemHovered();
	const float em = ImGui::GetFontSize();
	ImVec2 a(p0.x + em * 3.5f, p0.y + em * 0.5f), b(p0.x + size.x - em * 0.5f, p0.y + size.y - em * 2.0f);
	ImU32 axis = ImGui::GetColorU32(ImGuiCol_TextDisabled), grid = ImGui::GetColorU32(ImGuiCol_Border);
	dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
	if (pts.empty()) {
		dl->AddText(ImVec2(a.x + em, a.y + em), axis, "No data for this axis yet");
		return;
	}
	// Log x for speed axes (they span orders of magnitude); linear y.
	const bool logX = xmax / std::max(xmin, 1e-9) > 20;
	auto fx = [&](double v) {
		double t = logX ? (std::log10(v) - std::log10(xmin)) / std::max(1e-9, std::log10(xmax) - std::log10(xmin)) : (v - xmin) / std::max(1e-9, xmax - xmin);
		return a.x + em + (float)t * (b.x - a.x - 2 * em);
	};
	double ypad = std::max(0.5, (ymax - ymin) * 0.1);
	double y0 = ymin - ypad, y1 = ymax + ypad;
	auto fy = [&](double v) { return b.y - (float)((v - y0) / (y1 - y0)) * (b.y - a.y); };
	for (int i = 0; i <= 4; ++i) {
		double v = y0 + (y1 - y0) * i / 4;
		float y = fy(v);
		dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), grid);
		std::string t = Fmt("%.1f%%", v);
		dl->AddText(ImVec2(p0.x, y - em * 0.5f), axis, t.c_str());
	}
	for (int i = 0; i <= 4; ++i) {
		double v = logX ? std::pow(10.0, std::log10(xmin) + (std::log10(xmax) - std::log10(xmin)) * i / 4) : xmin + (xmax - xmin) * i / 4;
		float x = fx(v);
		std::string t = Fmt(v < 10 ? "%.2f" : "%.0f", v);
		dl->AddText(ImVec2(x - ImGui::CalcTextSize(t.c_str()).x * 0.5f, b.y + 2), axis, t.c_str());
	}
	const char *xl = benchChartX_ == 0 ? "compression MB/s (right = faster)" : benchChartX_ == 1 ? "decode seconds (left = faster)" : "ms per random read (left = faster)";
	dl->AddText(ImVec2(b.x - ImGui::CalcTextSize(xl).x, b.y + em), axis, xl);
	dl->AddText(ImVec2(a.x + 4, a.y + 2), axis, "size, % of ISO (lower = smaller)");

	const BenchRow *hot = nullptr;
	float best = em * em;
	ImVec2 mouse = ImGui::GetIO().MousePos;
	for (const BenchRow *r : pts) {
		ImVec2 c(fx(xval(*r)), fy(r->Ratio()));
		ImVec4 col = FormatColor(r->format);
		dl->AddCircleFilled(c, em * 0.32f, ImGui::GetColorU32(col));
		float d = (c.x - mouse.x) * (c.x - mouse.x) + (c.y - mouse.y) * (c.y - mouse.y);
		if (hovered && d < best) {
			best = d;
			hot = r;
		}
	}
	if (hot) {
		ImVec2 c(fx(xval(*hot)), fy(hot->Ratio()));
		dl->AddCircle(c, em * 0.55f, ImGui::GetColorU32(ImGuiCol_Text), 0, 2);
		ImGui::BeginTooltip();
		ImGui::TextColored(FormatColor(hot->format), "%s %s", hot->format.c_str(), hot->setting.c_str());
		ImGui::Text("%s: %.3f%% (%s)", hot->game.c_str(), hot->Ratio(), HumanBytes(hot->output).c_str());
		ImGui::Text("compress %.1f s (%.1f MB/s), decode %.2f s", hot->compSecs, hot->CompMBs(), hot->decompSecs);
		if (hot->readMs >= 0)
			ImGui::Text("random read %.3f ms", hot->readMs);
		ImGui::EndTooltip();
	}
	// Legend.
	float lx = a.x + em * 0.5f;
	std::vector<std::string> seen;
	for (const BenchRow *r : pts) {
		if (std::find(seen.begin(), seen.end(), r->format) != seen.end())
			continue;
		seen.push_back(r->format);
		dl->AddCircleFilled(ImVec2(lx + em * 0.3f, b.y - em * 0.7f), em * 0.3f, ImGui::GetColorU32(FormatColor(r->format)));
		dl->AddText(ImVec2(lx + em * 0.8f, b.y - em * 1.2f), ImGui::GetColorU32(ImGuiCol_Text), r->format.c_str());
		lx += em * 0.8f + ImGui::CalcTextSize(r->format.c_str()).x + em;
	}
}

// ---- Log --------------------------------------------------------------------------------------

void App::DrawLog() {
	unseenErrors_ = 0;
	if (ImGui::Button("Copy")) {
		std::string all;
		std::lock_guard<std::mutex> g(logMutex_);
		for (auto &l : log_)
			all += l.time + "  " + l.text + "\n";
		ImGui::SetClipboardText(all.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		std::lock_guard<std::mutex> g(logMutex_);
		log_.clear();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
	ImGui::BeginChild("##logtext", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	{
		std::lock_guard<std::mutex> g(logMutex_);
		ImGuiListClipper clip;
		clip.Begin((int)log_.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const LogLine &l = log_[i];
				ImGui::TextDisabled("%s", l.time.c_str());
				ImGui::SameLine();
				if (l.level == LogLevel::Error)
					ImGui::TextColored(kErr, "%s", l.text.c_str());
				else if (l.level == LogLevel::Warning)
					ImGui::TextColored(kWarn, "%s", l.text.c_str());
				else
					ImGui::TextUnformatted(l.text.c_str());
			}
		}
	}
	if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

// ---- About ------------------------------------------------------------------------------------

void App::DrawAbout() {
	ImGui::Text("ISOPackager %s", ISOPK_VERSION);
	ImGui::TextDisabled("Compress, extract, repack and benchmark disc images.");
	ImGui::Spacing();
	ImGui::Text("Libraries: %s, Dear ImGui %s", LibraryVersions().c_str(), IMGUI_VERSION);
	ImGui::Text("System: %s, %u cores, %s RAM", PlatformName().c_str(), CpuCount(), HumanBytes(TotalRam()).c_str());
	ImGui::Text("Settings: %s", settingsPath_.c_str());
	ImGui::Spacing();
	ImGui::SeparatorText("Formats");
	for (Format f : OutputFormats()) {
		FormatBadge(f);
		ImGui::SameLine(ImGui::GetFontSize() * 4);
		ImGui::TextUnformatted(FormatDescription(f));
	}
	ImGui::Spacing();
	ImGui::SeparatorText("Links");
	const char *links[][2] = { { "ISOPackager", "https://github.com/LunaMoo/ISOPackager" }, { "maxcso (mCSO)", "https://github.com/LunaMoo/maxcso" },
		{ "CHDmodern", "https://github.com/LunaMoo/CHDmodern" }, { "iPBP", "https://github.com/LunaMoo/iPBP" },
		{ "iRVZ", "https://github.com/LunaMoo/iRVZ" },
		{ "Dear ImGui", "https://github.com/ocornut/imgui" } };
	for (auto &l : links) {
		if (hooks_.openUrl) {
			if (ImGui::TextLink(l[0]))
				hooks_.openUrl(l[1]);
		} else {
			ImGui::TextUnformatted(l[0]);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", l[1]);
	}
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextUnformatted("Made by Claude AI based on LunaMoo idea, released under GPL 3.0 license.");
}

// ---- Status bar and dialogs -------------------------------------------------------------------

void App::DrawStatusBar() {
	ImGui::Separator();
	auto c = queue_.Count();
	if (scansRunning_ > 0) {
		ImGui::TextColored(kWarn, "Scanning...");
		ImGui::SameLine();
	}
	ImGui::Text("%d queued, %d running, %d done", c.queued, c.running, c.done);
	if (c.failed) {
		ImGui::SameLine();
		ImGui::TextColored(kErr, "%d failed", c.failed);
	}
	if (c.skipped) {
		ImGui::SameLine();
		ImGui::TextColored(kWarn, "%d skipped", c.skipped);
	}
	if (c.done && c.inBytes) {
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::Text("%s -> %s", HumanBytes(c.inBytes).c_str(), HumanBytes(c.outBytes).c_str());
	}
	std::string right = Fmt("RAM %s / %s   ->  %s", HumanBytes(RamBudget::Get().InUse()).c_str(), HumanBytes(RamBudget::Get().Capacity()).c_str(),
		OutputFolderLabel().c_str());
	float w = ImGui::CalcTextSize(right.c_str()).x;
	ImGui::SameLine();
	float avail = ImGui::GetContentRegionAvail().x;
	if (avail > w + ImGui::GetFontSize())
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
	ImGui::TextDisabled("%s", right.c_str());
}

void App::DrawModals() {
	if (browser_.Draw() && browserThen_) {
		auto then = browserThen_;
		browserThen_ = nullptr;
		then(browser_.Result());
	}

	if (quitRequested_) {
		ImGui::OpenPopup("Quit?");
		quitRequested_ = false;
	}
	if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Conversions or a benchmark are still running.");
		ImGui::TextUnformatted("Quitting cancels them; finished outputs are kept, partial ones deleted.");
		if (ImGui::Button("Cancel and quit")) {
			queue_.CancelAll();
			benchCancel_ = true;
			quitConfirmed_ = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep working")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (showOverwriteWarning_) {
		ImGui::OpenPopup("Delete sources?");
		showOverwriteWarning_ = false;
	}
	if (ImGui::BeginPopupModal("Delete sources?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Each source image will be deleted after its conversion succeeds.");
		if (!settings_.engine.verify)
			ImGui::TextColored(kWarn, "Verify is off: outputs won't be checked before their source is deleted.");
		if (ImGui::Button("OK")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Don't delete")) {
			settings_.out.deleteSource = false;
			SettingsChanged();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

}  // namespace isopk::gui
