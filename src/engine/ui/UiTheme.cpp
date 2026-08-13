#include "ui/UiTheme.h"
#include <string>

namespace
{
Font g_font;
bool g_loaded = false;
bool g_custom = false;  // whether a custom font was loaded (it must be unloaded)
}  // namespace

void Ui::LoadAssets()
{
    // Load a custom font from data/ui_font.ttf; if the file is missing, use raylib's default.
    std::string path = std::string(GetApplicationDirectory()) + "data/ui_font.ttf";
    if (FileExists(path.c_str()))
    {
        g_font = LoadFontEx(path.c_str(), 32, nullptr, 0);
        g_custom = true;
    }
    else
    {
        g_font = GetFontDefault();
        g_custom = false;
    }
    g_loaded = true;
}

void Ui::UnloadAssets()
{
    if (g_loaded && g_custom)
        UnloadFont(g_font);
}

Font Ui::GetFont()
{
    return g_loaded ? g_font : GetFontDefault();
}

void Ui::Text(const char* text, int x, int y, int size, Color color)
{
    DrawTextEx(GetFont(), text, { (float)x, (float)y }, (float)size, 1.0f, color);
}

int Ui::TextWidth(const char* text, int size)
{
    return (int)MeasureTextEx(GetFont(), text, (float)size, 1.0f).x;
}
