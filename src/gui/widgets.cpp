#include "gui/widgets.h"

#include <algorithm>
#include <cstring>

#include "core/util.h"

namespace isopk::gui {

void HelpMarker(const char *text) {
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	Tip(text);
}

void Tip(const char *text) {
	if (ImGui::BeginItemTooltip()) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

bool FormatCombo(const char *label, Format *f, bool includeIso) {
	bool changed = false;
	if (ImGui::BeginCombo(label, FormatName(*f))) {
		for (Format o : OutputFormats()) {
			if (o == Format::ISO && !includeIso)
				continue;
			bool sel = o == *f;
			std::string item = std::string(FormatName(o)) + "##" + FormatKey(o);
			if (ImGui::Selectable(item.c_str(), sel)) {
				*f = o;
				changed = true;
			}
			Tip(FormatDescription(o));
			if (sel)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	return changed;
}

static std::string SizeLabel(uint32_t v) {
	if (v >= 1024 && v % 1024 == 0)
		return Fmt("%uK", v / 1024);
	return Fmt("%u", v);
}

bool BlockSizeCombo(const char *label, uint32_t *value, uint32_t minSize, uint32_t maxSize, bool allowAuto, const char *autoLabel) {
	bool changed = false;
	std::string cur = (*value == 0 && allowAuto) ? autoLabel : SizeLabel(*value);
	if (ImGui::BeginCombo(label, cur.c_str())) {
		if (allowAuto && ImGui::Selectable(autoLabel, *value == 0)) {
			*value = 0;
			changed = true;
		}
		for (uint32_t v = minSize; v <= maxSize; v *= 2) {
			if (ImGui::Selectable(SizeLabel(v).c_str(), *value == v)) {
				*value = v;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

static int StringResize(ImGuiInputTextCallbackData *d) {
	if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
		std::string *s = (std::string *)d->UserData;
		s->resize(d->BufTextLen);
		d->Buf = s->data();
	}
	return 0;
}

bool InputString(const char *label, std::string *s, ImGuiInputTextFlags flags) {
	return ImGui::InputText(label, s->data(), s->capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, StringResize, s);
}

bool InputStringHint(const char *label, const char *hint, std::string *s, ImGuiInputTextFlags flags) {
	return ImGui::InputTextWithHint(label, hint, s->data(), s->capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, StringResize, s);
}

ImVec4 FormatColor(Format f) {
	switch (f) {
	case Format::ISO: return ImVec4(0.62f, 0.64f, 0.68f, 1);
	case Format::CSO: return ImVec4(0.30f, 0.62f, 0.98f, 1);
	case Format::CSO2: return ImVec4(0.45f, 0.78f, 0.98f, 1);
	case Format::ZSO: return ImVec4(0.98f, 0.70f, 0.20f, 1);
	case Format::DAX: return ImVec4(0.75f, 0.55f, 0.95f, 1);
	case Format::CHD: return ImVec4(0.36f, 0.82f, 0.48f, 1);
	case Format::PBP: return ImVec4(0.96f, 0.42f, 0.45f, 1);
	case Format::RVZ: return ImVec4(0.30f, 0.85f, 0.85f, 1);
	case Format::WIA: return ImVec4(0.55f, 0.75f, 0.60f, 1);
	default: return ImVec4(1, 1, 1, 1);
	}
}

ImVec4 FormatColor(const std::string &name) {
	for (int i = 1; i < (int)Format::Count; ++i)
		if (name == FormatName((Format)i))
			return FormatColor((Format)i);
	return ImVec4(1, 1, 1, 1);
}

void FormatBadge(Format f) {
	ImGui::TextColored(FormatColor(f), "%s", FormatName(f));
}

bool WideButton(const char *label, float minWidthEm) {
	float w = std::max(ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2, ImGui::GetFontSize() * minWidthEm);
	return ImGui::Button(label, ImVec2(w, 0));
}

void ApplyTheme(int theme, float scale) {
	ImGuiStyle &style = ImGui::GetStyle();
	style = ImGuiStyle();
	if (theme == 1)
		ImGui::StyleColorsLight();
	else if (theme == 2)
		ImGui::StyleColorsClassic();
	else
		ImGui::StyleColorsDark();
	style.WindowRounding = 6;
	style.FrameRounding = 4;
	style.GrabRounding = 4;
	style.PopupRounding = 4;
	style.TabRounding = 4;
	style.ChildRounding = 4;
	style.ScrollbarRounding = 6;
	style.FramePadding = ImVec2(8, 5);
	style.ItemSpacing = ImVec2(8, 6);
	style.CellPadding = ImVec2(6, 4);
	if (theme == 0) {
		ImVec4 *c = style.Colors;
		c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1);
		c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.14f, 1);
		c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.15f, 0.98f);
		c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.22f, 1);
		c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.31f, 1);
		c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.30f, 0.38f, 1);
		c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.20f, 0.33f, 1);
		c[ImGuiCol_Button] = ImVec4(0.20f, 0.36f, 0.62f, 1);
		c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.45f, 0.76f, 1);
		c[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.32f, 0.56f, 1);
		c[ImGuiCol_Header] = ImVec4(0.20f, 0.34f, 0.56f, 0.55f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.40f, 0.66f, 0.80f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.44f, 0.72f, 1);
		c[ImGuiCol_Tab] = ImVec4(0.14f, 0.17f, 0.22f, 1);
		c[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.45f, 0.76f, 1);
		c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.36f, 0.62f, 1);
		c[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.62f, 0.98f, 1);
		c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.72f, 1.0f, 1);
		c[ImGuiCol_SliderGrab] = ImVec4(0.38f, 0.62f, 0.95f, 1);
		c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.16f, 0.20f, 1);
		c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
	}
	style.ScaleAllSizes(scale);
}

}  // namespace isopk::gui
