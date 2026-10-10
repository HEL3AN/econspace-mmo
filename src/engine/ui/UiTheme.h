#pragma once

#include "raylib.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <string>

// The interface's style as the code written before the theme (#297) reads it: colours by
// name and a text call at a pixel size. The names are references into the loaded theme, so
// data/ui_theme.json reaches these screens as well; new code asks Ui::CurrentTheme() and
// lays out with Ui::Layout instead (documents/ui.md).
namespace Ui
{
inline const Color& PANEL_BG = MutableTheme().colors.panel;
inline const Color& PANEL_BORDER = MutableTheme().colors.border;
inline const Color& TITLE_BG = MutableTheme().colors.title;
inline const Color& ACCENT = MutableTheme().colors.accent;
inline const Color& TEXT = MutableTheme().colors.text;
inline const Color& TEXT_DIM = MutableTheme().colors.dim;

// Unscaled pixel metrics of the screens that still place their content by hand.
constexpr int TITLE_HEIGHT = 26;
constexpr int PADDING = 12;

// The theme and the fonts it names, from data/. After InitWindow;
// UnloadAssets before CloseWindow. A broken theme is logged and the defaults are kept.
void LoadAssets(const std::string& dataDir);
void UnloadAssets();

// Draws text in the regular face at `size` pixels, unscaled. x, y is the top left.
void Text(const char* text, int x, int y, int size, Color color);
int  TextWidth(const char* text, int size);
}  // namespace Ui
