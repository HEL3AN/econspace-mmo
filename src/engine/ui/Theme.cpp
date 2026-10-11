#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

namespace Ui
{

namespace
{
float       g_userScale = 1.0f;
float       g_displayOverride = 0.0f;
std::string g_theme = DEFAULT_THEME;

int Hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// One section of the file: every key it must have, read into the field it names. The
// section is refused if it has a key not on the list or lacks one that is.
template <typename T> using Fields = std::vector<std::pair<const char*, T*>>;

bool CheckKeys(const nlohmann::json& j, const char* section, const std::set<std::string>& known,
               std::string& error)
{
    if (!j.is_object())
    {
        error = std::string(section) + ": expected an object";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key()))
        {
            error = std::string(section) + ": unknown field '" + it.key() + "'";
            return false;
        }
    for (const std::string& k : known)
        if (!j.contains(k))
        {
            error = std::string(section) + ": missing field '" + k + "'";
            return false;
        }
    return true;
}

template <typename T> std::set<std::string> Keys(const Fields<T>& f)
{
    std::set<std::string> s;
    for (const auto& p : f)
        s.insert(p.first);
    return s;
}

bool ReadColors(const nlohmann::json& j, const char* section, const Fields<Color>& f,
                std::string& error)
{
    if (!CheckKeys(j, section, Keys(f), error))
        return false;
    for (const auto& p : f)
    {
        const nlohmann::json& v = j[p.first];
        if (!v.is_string() || !ParseColor(v.get<std::string>(), *p.second))
        {
            error =
                std::string(section) + "." + p.first + ": expected \"#RRGGBB\" or \"#RRGGBBAA\"";
            return false;
        }
    }
    return true;
}

bool ReadNumbers(const nlohmann::json& j, const char* section, const Fields<float>& f,
                 std::string& error)
{
    if (!CheckKeys(j, section, Keys(f), error))
        return false;
    for (const auto& p : f)
    {
        const nlohmann::json& v = j[p.first];
        if (!v.is_number() || v.get<float>() < 0.0f)
        {
            error = std::string(section) + "." + p.first + ": expected a number, zero or more";
            return false;
        }
        *p.second = v.get<float>();
    }
    return true;
}

bool ReadStrings(const nlohmann::json& j, const char* section, const Fields<std::string>& f,
                 std::string& error)
{
    if (!CheckKeys(j, section, Keys(f), error))
        return false;
    for (const auto& p : f)
    {
        const nlohmann::json& v = j[p.first];
        if (!v.is_string() || v.get<std::string>().empty())
        {
            error = std::string(section) + "." + p.first + ": expected a path";
            return false;
        }
        *p.second = v.get<std::string>();
    }
    return true;
}
}  // namespace

bool ParseColor(const std::string& s, Color& out)
{
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#')
        return false;
    unsigned char c[4] = { 0, 0, 0, 255 };
    for (size_t i = 1, k = 0; i < s.size(); i += 2, k++)
    {
        const int hi = Hex(s[i]), lo = Hex(s[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        c[k] = (unsigned char)(hi * 16 + lo);
    }
    out = { c[0], c[1], c[2], c[3] };
    return true;
}

bool ParseTheme(const nlohmann::json& j, Theme& into, std::string& error)
{
    Theme t;  // filled whole, then copied: a failure leaves `into` as it was
    t.id = into.id;
    if (!CheckKeys(j, "theme",
                   { "name", "colors", "standing", "metrics", "fontSize", "fonts", "text" }, error))
        return false;
    if (!j["name"].is_string() || j["name"].get<std::string>().empty())
    {
        error = "name: expected what Settings calls the theme";
        return false;
    }
    t.name = j["name"].get<std::string>();

    Theme::Colors& c = t.colors;
    if (!ReadColors(j["colors"], "colors",
                    { { "panel", &c.panel },
                      { "border", &c.border },
                      { "title", &c.title },
                      { "titleText", &c.titleText },
                      { "accent", &c.accent },
                      { "text", &c.text },
                      { "dim", &c.dim },
                      { "good", &c.good },
                      { "warn", &c.warn },
                      { "bad", &c.bad },
                      { "money", &c.money },
                      { "track", &c.track },
                      { "hover", &c.hover },
                      { "selected", &c.selected },
                      { "backdrop", &c.backdrop },
                      { "shade", &c.shade },
                      { "sensor", &c.sensor } },
                    error))
        return false;

    Theme::Standing& s = t.standing;
    if (!ReadColors(j["standing"], "standing",
                    { { "own", &s.own },
                      { "friendly", &s.friendly },
                      { "neutral", &s.neutral },
                      { "hostile", &s.hostile },
                      { "unowned", &s.unowned } },
                    error))
        return false;

    Theme::Metrics& m = t.metrics;
    if (!ReadNumbers(j["metrics"], "metrics",
                     { { "titleHeight", &m.titleHeight },
                       { "padding", &m.padding },
                       { "gap", &m.gap },
                       { "rowGap", &m.rowGap },
                       { "radius", &m.radius },
                       { "border", &m.border },
                       { "barHeight", &m.barHeight },
                       { "buttonHeight", &m.buttonHeight },
                       { "resizeGrip", &m.resizeGrip },
                       { "rowHeight", &m.rowHeight },
                       { "scrollbar", &m.scrollbar },
                       { "snap", &m.snap },
                       { "tabWidth", &m.tabWidth },
                       { "sidePanel", &m.sidePanel },
                       { "menuBar", &m.menuBar },
                       { "formLabel", &m.formLabel },
                       { "titleMark", &m.titleMark },
                       { "tabMark", &m.tabMark } },
                     error))
        return false;

    Theme::FontSizes& f = t.fontSize;
    if (!ReadNumbers(j["fontSize"], "fontSize",
                     { { "small", &f.small },
                       { "label", &f.label },
                       { "body", &f.body },
                       { "title", &f.title },
                       { "heading", &f.heading } },
                     error))
        return false;
    for (float size : { f.small, f.label, f.body, f.title, f.heading })
        if (size < 6.0f)
        {
            error = "fontSize: a size below 6 cannot be read";
            return false;
        }

    const std::vector<std::pair<const char*, FaceSpec*>> faces = { { "regular", &t.fonts.regular },
                                                                   { "strong", &t.fonts.strong },
                                                                   { "heading", &t.fonts.heading },
                                                                   { "numbers",
                                                                     &t.fonts.numbers } };
    if (!CheckKeys(j["fonts"], "fonts", Keys(faces), error))
        return false;
    for (const auto& face : faces)
    {
        const std::string     section = std::string("fonts.") + face.first;
        const nlohmann::json& fj = j["fonts"][face.first];
        if (!CheckKeys(fj, section.c_str(), { "file", "upper", "tracking" }, error))
            return false;
        if (!ReadStrings({ { "file", fj["file"] } }, section.c_str(),
                         { { "file", &face.second->file } }, error) ||
            !ReadNumbers({ { "tracking", fj["tracking"] } }, section.c_str(),
                         { { "tracking", &face.second->tracking } }, error))
            return false;
        if (!fj["upper"].is_boolean())
        {
            error = section + ".upper: expected true or false";
            return false;
        }
        face.second->upper = fj["upper"].get<bool>();
    }

    if (!ReadNumbers(j["text"], "text",
                     { { "lineHeight", &t.text.lineHeight }, { "gamma", &t.text.gamma } }, error))
        return false;
    if (t.text.lineHeight < 1.0f || t.text.gamma < 0.5f || t.text.gamma > 3.0f)
    {
        error = "text: lineHeight is at least 1, gamma between 0.5 and 3";
        return false;
    }

    into = t;
    return true;
}

bool LoadTheme(const std::string& path, Theme& into, std::string& error)
{
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded())
    {
        error = path + ": not valid JSON";
        return false;
    }
    Theme t;
    t.id = std::filesystem::path(path).stem().string();
    if (!ParseTheme(j, t, error))
    {
        error = path + ": " + error;
        return false;
    }
    into = t;
    return true;
}

std::vector<ThemeEntry> ListThemes(const std::string& dir, std::string* problems)
{
    std::vector<ThemeEntry>  themes;
    std::error_code          ec;
    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".json")
            paths.push_back(entry.path().string());
    std::sort(paths.begin(), paths.end());
    for (const std::string& p : paths)
    {
        Theme       t;
        std::string error;
        if (LoadTheme(p, t, error))
            themes.push_back({ t.id, t.name });
        else if (problems != nullptr)
            *problems += error + "\n";
    }
    return themes;
}

Theme& MutableTheme()
{
    // A function's own static, not a global: UiTheme.h's colour names bind to it while
    // other files are still being initialised, and some of those read it straight away.
    static Theme theme;
    return theme;
}

const Theme& CurrentTheme()
{
    return MutableTheme();
}

float DisplayScale()
{
    if (g_displayOverride > 0.0f)
        return g_displayOverride;
    if (!IsWindowReady())
        return 1.0f;
#if defined(__APPLE__)
    // Without HIGHDPI a Mac draws in points and scales the window itself; scaling the
    // interface by the display's factor as well would do it twice.
    if (!IsWindowState(FLAG_WINDOW_HIGHDPI))
        return 1.0f;
#endif
    const Vector2 dpi = GetWindowScaleDPI();
    return dpi.x > 0.0f ? dpi.x : 1.0f;
}

void OverrideDisplayScale(float s)
{
    g_displayOverride = s;
}

float UserScale()
{
    return g_userScale;
}

void SetUserScale(float s)
{
    g_userScale = std::clamp(s, MIN_USER_SCALE, MAX_USER_SCALE);
}

float Scale()
{
    return g_userScale * DisplayScale();
}

float Px(float units)
{
    return std::round(units * Scale());
}

bool LoadUiSettings(const std::string& path, std::string& error)
{
    std::ifstream in(path);
    if (!in)
        return true;  // nothing saved yet
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        error = path + ": not valid JSON";
        return false;
    }
    if (j.value("version", 0) > UI_SETTINGS_VERSION)
    {
        error = path + ": written by a newer build";
        return false;
    }
    if (j.contains("scale") && j["scale"].is_number())
        SetUserScale(j["scale"].get<float>());
    if (j.contains("theme") && j["theme"].is_string() && !j["theme"].get<std::string>().empty())
        g_theme = j["theme"].get<std::string>();
    return true;
}

const std::string& ThemeSetting()
{
    return g_theme;
}

void SetThemeSetting(const std::string& id)
{
    g_theme = id.empty() ? std::string(DEFAULT_THEME) : id;
}

bool SaveUiSettings(const std::string& path)
{
    nlohmann::json j;
    j["version"] = UI_SETTINGS_VERSION;
    j["scale"] = g_userScale;
    j["theme"] = g_theme;
    std::ofstream out(path);
    if (!out)
        return false;
    out << j.dump(4) << "\n";
    return (bool)out;
}

}  // namespace Ui
