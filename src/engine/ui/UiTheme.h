#pragma once

#include "raylib.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <string>

// The interface's style as the code written before the theme (#297) reads it: colours by
// name and a text call at a pixel size. The names are references into the theme in use, so
// a theme file -- and switching to another -- reaches these screens as well; new code asks
// Ui::CurrentTheme() and
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

// The theme and the fonts it names, from data/ (data/ui_themes/<id>.json; empty for the
// default). After InitWindow; UnloadAssets before CloseWindow. A theme that does not load is
// logged and the default is used; if that fails too, the built-in one.
void LoadAssets(const std::string& dataDir, const std::string& themeId = "");
void UnloadAssets();
// Switches the theme in use, at once: the file is read, the theme replaced and every font
// rasterised again. False, and the theme in use kept, if the file does not load. Between
// frames, never while a layout is being declared.
bool        UseTheme(const std::string& id);
std::string ThemeDir();  // where the theme files are: data/ui_themes/

// Draws text in the regular face at `size` pixels, unscaled. x, y is the top left.
void Text(const char* text, int x, int y, int size, Color color);
int  TextWidth(const char* text, int size);
}  // namespace Ui
