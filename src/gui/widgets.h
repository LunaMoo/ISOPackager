// Small ImGui helpers shared by the ISOPackager screens.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

#include "core/formats.h"

namespace isopk::gui {

// A "(?)" that shows text when hovered.
void HelpMarker(const char *text);
// Tooltip for the previous item, wrapped at a readable width.
void Tip(const char *text);

// Combo over the output formats. Returns true when changed.
bool FormatCombo(const char *label, Format *f, bool includeIso = true);
// Power-of-two size combo ("Auto", "2K" ... "256K"); value 0 = auto when allowAuto.
bool BlockSizeCombo(const char *label, uint32_t *value, uint32_t minSize, uint32_t maxSize, bool allowAuto, const char *autoLabel = "Automatic");
// Text input bound to a std::string.
bool InputString(const char *label, std::string *s, ImGuiInputTextFlags flags = 0);
bool InputStringHint(const char *label, const char *hint, std::string *s, ImGuiInputTextFlags flags = 0);

// Colors used for format badges and chart series.
ImVec4 FormatColor(Format f);
ImVec4 FormatColor(const std::string &formatName);
void FormatBadge(Format f);

// Button with a fixed minimum width (touch friendly on mobile).
bool WideButton(const char *label, float minWidthEm = 6.0f);

void ApplyTheme(int theme, float scale);

}  // namespace isopk::gui
