#include "ui/UiTheme.h"
#include "ui/Paint.h"

namespace
{
std::string g_dataDir;
}

bool Ui::UseTheme(const std::string& id)
{
    std::string error;
    Theme       t;
    const bool  ok = LoadTheme(ThemeDir() + id + ".json", t, error);
    if (ok)
    {
        MutableTheme() = t;
        AdoptStanding();  // the colour-blind palette over its standing, if the player has it on
    }
    else
        TraceLog(LOG_ERROR, "UI theme: %s -- keeping the one in use", error.c_str());
    // The fonts are rasterised again either way: on the first load the theme in use is the
    // built-in one, whose fonts have not been loaded yet.
    const Theme&    c = CurrentTheme();
    FontFiles       files;
    const FaceSpec* faces[FACE_COUNT] = { &c.fonts.regular, &c.fonts.strong, &c.fonts.heading,
                                          &c.fonts.numbers };
    for (int i = 0; i < FACE_COUNT; i++)
        files.faces[i] = { g_dataDir + faces[i]->file, faces[i]->textCase, faces[i]->tracking };
    files.gamma = c.text.gamma;
    LoadFonts(files);
    return ok;
}

std::string Ui::ThemeDir()
{
    return g_dataDir + "ui_themes/";
}

void Ui::LoadAssets(const std::string& dataDir, const std::string& themeId)
{
    g_dataDir = dataDir;
    if (!UseTheme(themeId.empty() ? DEFAULT_THEME : themeId) && themeId != DEFAULT_THEME)
        UseTheme(DEFAULT_THEME);  // a theme named by an older build's settings, since removed
}

void Ui::UnloadAssets()
{
    UnloadFonts();
    UnloadPaint();
}

void Ui::Text(const char* text, int x, int y, int size, Color color)
{
    DrawString(Face::Regular, text, { (float)x, (float)y }, (float)size, color);
}

int Ui::TextWidth(const char* text, int size)
{
    return (int)MeasureString(Face::Regular, (float)size, text).x;
}
