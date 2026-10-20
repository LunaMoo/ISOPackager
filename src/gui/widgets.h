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

// ---- Mobile layout ----------------------------------------------------------------------------
// The mobile layout is one column with big targets: labels go above their widgets, widgets take
// the whole row, and HelpMarker() writes its text out (there is no hover on a touch screen).
void SetMobileLayout(bool on);
bool MobileLayout();
// SameLine() on the desktop; nothing (a new row) in the mobile layout.
void SameLineDesktop(float offsetFromStartX = 0);
// SetNextItemWidth(em * widthEm) on the desktop, the whole row in the mobile layout.
void ItemWidthEm(float widthEm);
// A labelled setting row: label then the widget at labelEm on the desktop, label on its own line
// above a full-width widget in the mobile layout. Call right before the widget.
void SettingLabel(const char *label, float labelEm = 12, float widgetEm = 14);
// A big button: width < 0 fills the row like SetNextItemWidth(-x), 0 fits the label. Height in em.
bool BigButton(const char *label, float width = -FLT_MIN, float heightEm = 2.6f);
// Checkbox whose label wraps onto more lines in the mobile layout (and is tappable too).
bool CheckboxWrap(const char *label, bool *v);
// The width of the n-th of `count` buttons side by side in the remaining row.
float SplitWidth(int count);
// Drag-to-scroll for the current window and its children, with a little inertia. A vertical drag
// that starts on a button scrolls instead of pressing it.
void TouchScroll();
// True from the moment a drag became a scroll until the next press (so a release isn't a tap).
bool TouchScrolled();
// A scroll or its inertia is still moving (keep redrawing).
bool TouchScrollAnimating();

void ApplyTheme(int theme, float scale, bool mobile = false);

}  // namespace isopk::gui
