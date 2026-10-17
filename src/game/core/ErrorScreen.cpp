#include "core/ErrorScreen.h"

#include "core/DataDir.h"
#include "core/Version.h"
#include "raylib.h"
#include "ui/Fonts.h"
#include "ui/Input.h"
#include "ui/Paint.h"
#include "ui/Theme.h"
#include "ui/UiTheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

namespace ErrorScreen
{

std::string FindLink(const std::string& text)
{
    size_t at = text.find("https://");
    if (at == std::string::npos)
        at = text.find("http://");
    if (at == std::string::npos)
        return std::string();
    size_t end = at;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\t' &&
           text[end] != '"' && text[end] != ')')
        end++;
    // "...from https://host/releases." -- the full stop ends the sentence, not the link.
    while (end > at && std::string_view(".,;:!?").find(text[end - 1]) != std::string_view::npos)
        end--;
    return text.substr(at, end - at);
}

namespace
{

float Width(Ui::Face face, float px, std::string_view s)
{
    return Ui::MeasureString(face, px, s).x;
}

// Where a run may be cut without splitting a UTF-8 sequence: not before a continuation byte.
bool Boundary(std::string_view s, size_t i)
{
    return i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80;
}

// `text` as lines no wider than `width`: broken at spaces, and a word too long for a line
// on its own -- a link, a file name -- broken wherever it must be.
std::vector<std::string> Wrap(Ui::Face face, float px, const std::string& text, float width)
{
    std::vector<std::string> lines;
    size_t                   start = 0;
    while (start <= text.size())
    {
        size_t                 nl = text.find('\n', start);
        const std::string_view para(text.data() + start,
                                    (nl == std::string::npos ? text.size() : nl) - start);
        std::string            line;
        size_t                 i = 0;
        while (i < para.size())
        {
            size_t sp = para.find(' ', i);
            if (sp == std::string_view::npos)
                sp = para.size();
            std::string_view word = para.substr(i, sp - i);
            i = sp + 1;
            const std::string joined =
                line.empty() ? std::string(word) : line + " " + std::string(word);
            if (Width(face, px, joined) <= width)
            {
                line = joined;
                continue;
            }
            if (!line.empty())
                lines.push_back(line);
            line.clear();
            // Too long for a line by itself: as much of it as fits, then the rest.
            while (!word.empty() && Width(face, px, word) > width)
            {
                size_t cut = word.size() - 1;
                while (cut > 1 &&
                       (!Boundary(word, cut) || Width(face, px, word.substr(0, cut)) > width))
                    cut--;
                lines.emplace_back(word.substr(0, cut));
                word = word.substr(cut);
            }
            line = std::string(word);
        }
        lines.push_back(line);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return lines;
}

// A button in the theme's colours; `primary` is filled with the accent. True when clicked.
bool Button(Rectangle r, const char* label, bool primary, float px)
{
    const Ui::Theme& t = Ui::CurrentTheme();
    const float      radius = t.WidgetRadius() * Ui::Scale();
    const bool       over = Ui::MouseOver(r);
    Color            fill = primary ? t.colors.accent : t.colors.title;
    if (over)
        fill = primary ? Ui::Over(Ui::Faded(WHITE, 0.15f), fill) : Ui::Over(t.colors.hover, fill);
    Ui::FillShape(r, t.style.corner, radius, fill);
    Ui::OutlineShape(r, t.style.corner, radius, std::max(1.0f, Ui::Px(t.metrics.border)),
                     primary ? t.colors.accent : t.colors.border);
    const float w = Width(Ui::Face::Strong, px, label);
    Ui::DrawString(Ui::Face::Strong, label,
                   { std::floor(r.x + (r.width - w) / 2.0f),
                     Ui::CentredTop(Ui::Face::Strong, px, r.y, r.height) },
                   px, primary ? t.colors.onAccent : t.colors.text);
    return over && Ui::MousePressed(MOUSE_BUTTON_LEFT);
}

enum class Action
{
    None,
    Close,
    Copy,
    CopyLink,
};

// One frame of the screen. Returns what the player clicked.
Action Draw(const Message& m, const std::string& link, double copiedAt, const char* copiedWhat)
{
    const Ui::Theme& t = Ui::CurrentTheme();
    const float      sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    ClearBackground(t.colors.backdrop);

    // Larger than a window's running text: this is the whole screen, read once, carefully.
    const float pad = Ui::Px(24.0f);
    const float headPx = Ui::Px(std::max(22.0f, t.fontSize.heading));
    const float bodyPx = Ui::Px(16.0f);
    const float smallPx = Ui::Px(13.0f);
    const float buttonH = Ui::Px(32.0f);
    const float lineH = std::floor(bodyPx * t.text.lineHeight);
    const float panelW = std::min(Ui::Px(640.0f), sw - Ui::Px(32.0f));
    const float textW = panelW - 2.0f * pad;

    const std::vector<std::string> body = Wrap(Ui::Face::Regular, bodyPx, m.text, textW);
    const std::vector<std::string> linkLines =
        link.empty() ? std::vector<std::string>() : Wrap(Ui::Face::Strong, bodyPx, link, textW);

    const float headH = Ui::MetricsAt(Ui::Face::Heading, headPx).height;
    float       panelH = pad + headH + Ui::Px(14.0f) + lineH * (float)body.size();
    if (!linkLines.empty())
        panelH += Ui::Px(12.0f) + lineH * (float)linkLines.size();
    panelH += Ui::Px(22.0f) + buttonH + pad;

    const Rectangle panel = { std::floor((sw - panelW) / 2.0f),
                              std::floor(std::max(Ui::Px(16.0f), (sh - panelH) / 2.0f)), panelW,
                              panelH };
    const float     radius = t.metrics.windowRadius * Ui::Scale();
    Ui::DrawShadow(panel, t.style.corner, radius, t.style.shadow, Ui::Scale());
    Ui::FillShape(panel, t.style.corner, radius, t.colors.panel);
    Ui::OutlineShape(panel, t.style.corner, radius, std::max(1.0f, Ui::Px(t.metrics.border)),
                     t.colors.border);
    // A stripe in the colour of failure down the left, as a title bar's mark is drawn.
    DrawRectangleRec(
        { panel.x, panel.y + radius, std::max(2.0f, Ui::Px(3.0f)), panel.height - 2.0f * radius },
        t.colors.bad);

    float y = panel.y + pad;
    Ui::DrawGlowString(Ui::Face::Heading, m.title, { panel.x + pad, y }, headPx, t.colors.text,
                       Ui::Faded(t.colors.text, t.style.glow));
    y += headH + Ui::Px(14.0f);
    for (const std::string& line : body)
    {
        Ui::DrawString(Ui::Face::Regular, line, { panel.x + pad, y }, bodyPx, t.colors.text);
        y += lineH;
    }
    if (!linkLines.empty())
    {
        y += Ui::Px(12.0f);
        for (const std::string& line : linkLines)
        {
            Ui::DrawString(Ui::Face::Strong, line, { panel.x + pad, y }, bodyPx, t.colors.accent);
            y += lineH;
        }
    }

    // The buttons, right to left from the primary one; what was copied, and how to leave, on
    // the left.
    const float by = panel.y + panel.height - pad - buttonH;
    const float bpx = Ui::Px(15.0f);
    Action      action = Action::None;
    float       right = panel.x + panel.width - pad;
    const auto  place = [&](const char* label, bool primary, Action a)
    {
        const float w =
            std::max(Ui::Px(92.0f), Width(Ui::Face::Strong, bpx, label) + Ui::Px(32.0f));
        right -= w;
        if (Button({ right, by, w, buttonH }, label, primary, bpx))
            action = a;
        right -= Ui::Px(10.0f);
    };
    place("Close", true, Action::Close);
    place("Copy message", false, Action::Copy);
    if (!link.empty())
        place("Copy link", false, Action::CopyLink);

    const bool  copied = copiedAt >= 0.0 && GetTime() - copiedAt < 2.5;
    const char* hint = copied ? copiedWhat : "Esc closes";
    Ui::DrawString(Ui::Face::Regular, hint,
                   { panel.x + pad, Ui::CentredTop(Ui::Face::Regular, smallPx, by, buttonH) },
                   smallPx, copied ? t.colors.good : t.colors.dim);
    return action;
}

}  // namespace

void Show(const Message& message, const std::string& shotPath, int shotFrames)
{
    std::fprintf(stderr, "%s: %s\n", message.title.c_str(), message.text.c_str());

    const bool shooting = !shotPath.empty();
    const bool ownWindow = !IsWindowReady();
    if (ownWindow)
    {
        // Nothing is open yet: the failure came before the game's window. A window of our
        // own, in the player's theme, at a size that leaves room for a long reason.
        unsigned int flags = FLAG_WINDOW_RESIZABLE;
        if (shooting)
            flags |= FLAG_WINDOW_HIDDEN;  // as the game does for a scripted shot (#293)
        SetConfigFlags(flags);
        InitWindow(820, 460, Version::NAME);
        SetTargetFPS(60);
        std::string error;
        if (!Ui::LoadUiSettings(std::string(GetApplicationDirectory()) + "ui_settings.json", error))
            TraceLog(LOG_WARNING, "UI settings: %s", error.c_str());
        Ui::LoadAssets(DataDir::Path(), Ui::ThemeSetting());
    }
    // Esc is ours here: raylib's default would end the loop without the player reading on.
    SetExitKey(KEY_NULL);

    const std::string link = FindLink(message.text);
    const std::string all = message.title + ": " + message.text;
    double            copiedAt = -1.0;
    const char*       copiedWhat = "";
    RenderTexture2D   target{};
    int               frame = 0;
    while (!WindowShouldClose())
    {
        if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
            break;
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrl && IsKeyPressed(KEY_C))
        {
            SetClipboardText(all.c_str());
            copiedAt = GetTime();
            copiedWhat = "The message is on the clipboard.";
        }

        if (shooting && target.id == 0)
            target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        BeginDrawing();
        if (shooting)
            BeginTextureMode(target);
        const Action a = Draw(message, link, copiedAt, copiedWhat);
        if (shooting)
        {
            EndTextureMode();
            if (++frame >= shotFrames)
            {
                Image shot = LoadImageFromTexture(target.texture);
                ImageFlipVertical(&shot);  // a render texture is stored upside down
                ImageFormat(&shot, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
                if (!ExportImage(shot, shotPath.c_str()))
                    TraceLog(LOG_WARNING, "Could not write %s", shotPath.c_str());
                UnloadImage(shot);
                EndDrawing();
                break;
            }
        }
        EndDrawing();

        if (a == Action::Close)
            break;
        if (a == Action::Copy)
        {
            SetClipboardText(all.c_str());
            copiedAt = GetTime();
            copiedWhat = "The message is on the clipboard.";
        }
        else if (a == Action::CopyLink)
        {
            SetClipboardText(link.c_str());
            copiedAt = GetTime();
            copiedWhat = "The link is on the clipboard.";
        }
    }
    if (target.id != 0)
        UnloadRenderTexture(target);

    if (ownWindow)
    {
        Ui::UnloadAssets();
        CloseWindow();
    }
}

}  // namespace ErrorScreen
