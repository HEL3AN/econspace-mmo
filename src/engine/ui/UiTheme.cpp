#include "ui/UiTheme.h"

void Ui::LoadAssets(const std::string& dataDir)
{
    std::string error;
    if (!LoadTheme(dataDir + "ui_theme.json", MutableTheme(), error))
        TraceLog(LOG_ERROR, "UI theme: %s -- using the built-in one", error.c_str());
    const Theme& t = CurrentTheme();
    LoadFonts(dataDir + t.fonts.regular, dataDir + t.fonts.strong);
}

void Ui::UnloadAssets()
{
    UnloadFonts();
}

void Ui::Text(const char* text, int x, int y, int size, Color color)
{
    DrawString(Face::Regular, text, { (float)x, (float)y }, (float)size, color);
}

int Ui::TextWidth(const char* text, int size)
{
    return (int)MeasureString(Face::Regular, (float)size, text).x;
}
