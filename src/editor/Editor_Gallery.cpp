// The gallery: every archetype in the registry on one screen, drawn through the same
// backend the game draws through, with the state that changes how a thing looks on
// sliders. One translation unit of Editor (#17).
//
// It exists because a look is judged by eye and nothing else, and the loop it replaces was
// build, serve, connect, fly there, look (#118). Everything in M6 -- lighting, the screen
// treatment, materials, silhouettes -- is tuned by watching eighteen objects change at
// once rather than one object a minute.
//
// The cards are built from the archetype, not from an entity, for two reasons. Half the
// registry has no entity a hand can place -- stars, ships -- and would simply be missing.
// And the sliders set what is *happening* to an object, which no entity would report on
// demand: a hull at a fifth, a belt nearly mined out, a wreck already looted are the cases
// that have to read at a glance, and there is no way to ask an entity to be damaged.

#include "Editor.h"

#include "core/ArchetypeEdit.h"
#include "render/TreatmentPanel.h"
#include "ui/Controls.h"
#include "ui/UiTheme.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
const float kCardW = 168.0f;
const float kCardH = 150.0f;
const float kCardGap = 10.0f;
const float kGridTop = 132.0f;  // below the header and the button row
const float kGridLeft = 16.0f;
const float kPanelW = 300.0f;

// What an archetype can do, one line. The palette derives the same line; both read the
// component set rather than a description someone has to keep true.
std::string Components(const Archetype& a)
{
    std::string s;
    for (Component c : AllComponents())
        if (a.Has(c))
        {
            if (!s.empty())
                s += ", ";
            s += ComponentName(c);
        }
    return s.empty() ? std::string("scenery") : s;
}

const char* StyleName(GlyphStyle s)
{
    switch (s)
    {
        case GlyphStyle::Point: return "point";
        case GlyphStyle::Region: return "region";
        case GlyphStyle::Directional: return "directional";
    }
    return "point";
}

// raylib's scissor does not nest: EndScissorMode turns clipping off rather than
// restoring the outer rectangle. So every clipped draw states its own rectangle, already
// intersected with the grid it lives in.
Rectangle Clip(Rectangle a, Rectangle b)
{
    const float x1 = fmaxf(a.x, b.x), y1 = fmaxf(a.y, b.y);
    const float x2 = fminf(a.x + a.width, b.x + b.width);
    const float y2 = fminf(a.y + a.height, b.y + b.height);
    return { x1, y1, fmaxf(0.0f, x2 - x1), fmaxf(0.0f, y2 - y1) };
}

void BeginClip(Rectangle r)
{
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
}

}  // namespace

Rectangle Editor::GalleryButtonRect() const
{
    return { screenWidth_ / 2.0f - 151.0f - 6.0f - 110.0f, 12.0f, 110.0f, 30.0f };
}

void Editor::OpenGallery()
{
    EnterGalleryMode(true);
}

void Editor::UseShapes()
{
    backend_ = &shapeBackend_;
}

void Editor::EnterGalleryMode(bool on)
{
    mode_ = on ? Mode::Gallery : Mode::System;
    activeField_.clear();
    openDropdown_.clear();
    placeArchetype_.clear();
    if (on)
    {
        galleryScroll_ = 0.0f;
        // Open with something selected: an empty look panel beside a screen full of
        // objects reads as a panel that does not work.
        if (gallerySelected_ < 0 && !Archetypes::All().empty())
            gallerySelected_ = 0;
    }
}

Rectangle Editor::GalleryCardRect(int index) const
{
    const float gridW = (float)screenWidth_ - kPanelW - kGridLeft * 2.0f;
    int         cols = (int)((gridW + kCardGap) / (kCardW + kCardGap));
    if (cols < 1)
        cols = 1;
    const int row = index / cols;
    const int col = index % cols;
    return { kGridLeft + col * (kCardW + kCardGap),
             kGridTop + row * (kCardH + kCardGap) - galleryScroll_, kCardW, kCardH };
}

int Editor::GalleryHit(Vector2 p) const
{
    if (p.x > (float)screenWidth_ - kPanelW || p.y < kGridTop)
        return -1;
    for (int i = 0; i < (int)Archetypes::All().size(); i++)
        if (CheckCollisionPointRec(p, GalleryCardRect(i)))
            return i;
    return -1;
}

// A card has no system around it, so the light is made up -- but made up in the same
// shape the world produces: a Render::Light at a distance, which is what the game builds
// from its stars. Tuning against anything else would be tuning against a different thing.
Render::Lighting Editor::GalleryLighting(Vector2 at, float size) const
{
    Render::Lighting lg;
    lg.ambient = galleryAmbient_;
    if (!galleryLit_)
        return lg;  // empty means unlit, which is full colour -- the old picture

    // Placed just outside the object, with the intensity solved backwards from the
    // falloff so the strength slider means what it says: at 1.00 the object is fully lit
    // whatever its size. A slider that reads 1.00 over a half-lit object is a slider that
    // has to be re-learned every time it is used.
    const float dist = size * 3.0f;
    const float reach = dist * 4.0f;
    const float t = 1.0f - dist / reach;
    const float compensate = 1.0f / (t * t);

    lg.lights.push_back({ { at.x + std::cos(galleryLightAngle_) * dist,
                            at.y + std::sin(galleryLightAngle_) * dist },
                          Color{ 255, 244, 214, 255 },
                          galleryLightStrength_ * compensate,
                          reach });
    if (gallerySecondLight_)
    {
        const float a2 = galleryLightAngle_ + PI * 0.8f;
        lg.lights.push_back({ { at.x + std::cos(a2) * dist, at.y + std::sin(a2) * dist },
                              Color{ 150, 190, 255, 255 },
                              galleryLightStrength_ * compensate * 0.55f,
                              reach });
    }
    return lg;
}

Render::Item Editor::GalleryItem(const Archetype& a, Vector2 pos, float size) const
{
    Render::Item it = Render::FromArchetype(a, pos, size);
    it.intensity = galleryIntensity_;
    it.heading = galleryHeading_;
    it.thrusting = galleryThrusting_;
    return it;
}

void Editor::HandleGalleryInput()
{
    const Vector2 m = GetMousePosition();
    const bool    overPanel = m.x > (float)screenWidth_ - kPanelW;

    if (IsKeyPressed(KEY_ESCAPE))
    {
        EnterGalleryMode(false);
        return;
    }

    if (!overPanel)
    {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            galleryScroll_ -= wheel * 48.0f;

        // Never scroll past the last row: an empty screen looks like a crash.
        const int   count = (int)Archetypes::All().size();
        const float lastBottom =
            count > 0 ? GalleryCardRect(count - 1).y + galleryScroll_ + kCardH : kGridTop;
        const float maxScroll = fmaxf(0.0f, lastBottom + 16.0f - (float)screenHeight_);
        galleryScroll_ = Clamp(galleryScroll_, 0.0f, maxScroll);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            const int hit = GalleryHit(m);
            activeField_.clear();
            gallerySelected_ = (hit == gallerySelected_) ? -1 : hit;
        }
    }
}

void Editor::DrawGallery()
{
    const std::vector<Archetype>& all = Archetypes::All();

    // "True scale" measures every card against the largest object in the registry, so a
    // station reads as the speck it is beside a star. Fitted is the default because most
    // of the time the question is what a thing looks like, not how big it is.
    float largest = 1.0f;
    for (const Archetype& a : all)
        largest = fmaxf(largest, a.defaultSize);

    const Rectangle grid{ 0.0f, kGridTop - 8.0f, (float)screenWidth_ - kPanelW,
                          (float)screenHeight_ - kGridTop + 8.0f };

    for (int i = 0; i < (int)all.size(); i++)
    {
        const Archetype& a = all[(size_t)i];
        const Rectangle  card = GalleryCardRect(i);
        if (card.y + card.height < grid.y || card.y > grid.y + grid.height)
            continue;  // scrolled out of sight

        const bool sel = (i == gallerySelected_);
        const bool over = CheckCollisionPointRec(GetMousePosition(), card);

        BeginClip(Clip(grid, card));
        DrawRectangleRec(card, sel ? Fade(Ui::ACCENT, 0.14f)
                                   : (over ? Fade(Ui::ACCENT, 0.06f) : Fade(Ui::PANEL_BG, 0.75f)));
        DrawRectangleLinesEx(card, 1.0f, sel ? Ui::ACCENT : Ui::PANEL_BORDER);
        EndScissorMode();

        const Rectangle box{ card.x + 6.0f, card.y + 6.0f, card.width - 12.0f, 92.0f };
        const float     size = a.defaultSize > 0.0f ? a.defaultSize : 100.0f;

        Camera2D cam{};
        cam.offset = { box.x + box.width / 2.0f, box.y + box.height / 2.0f };
        cam.target = { 0.0f, 0.0f };
        cam.zoom = (fminf(box.width, box.height) * 0.42f) / (galleryTrueScale_ ? largest : size);

        BeginClip(Clip(grid, box));
        BeginMode2D(cam);
        // Through Present rather than Draw, so the card is lit by the same path the world
        // is -- and so a card can never inherit the lights of whatever drew last.
        Render::Present({ GalleryItem(a, { 0.0f, 0.0f }, size) },
                        GalleryLighting({ 0.0f, 0.0f }, size), *backend_);
        EndMode2D();
        EndScissorMode();

        // Clipped to the card, not to the grid: a long component list belongs to one
        // archetype and must not be read as part of the next one.
        const Rectangle text{ card.x + 4.0f, card.y + 96.0f, card.width - 8.0f, 50.0f };
        BeginClip(Clip(grid, text));
        Ui::Text(a.name.c_str(), (int)card.x + 8, (int)(card.y + 100.0f), 14,
                 sel ? Ui::ACCENT : Ui::TEXT);
        Ui::Text(TextFormat("%s   r %g", a.id.c_str(), (double)size), (int)card.x + 8,
                 (int)(card.y + 117.0f), 10, Ui::TEXT_DIM);
        Ui::Text(Components(a).c_str(), (int)card.x + 8, (int)(card.y + 131.0f), 10, Ui::TEXT_DIM);
        EndScissorMode();
    }
}

void Editor::DrawGalleryPanel()
{
    const Rectangle panel{ (float)screenWidth_ - kPanelW, 0.0f, kPanelW, (float)screenHeight_ };
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);

    float       y = 16.0f;
    const float x = panel.x + 14.0f;
    const float w = panel.width - 28.0f;

    Ui::Text("BACKEND", (int)x, (int)y, 12, Ui::TEXT_DIM);
    Ui::Text(TextFormat("%s   [F2]", backend_->Name()), (int)x + 90, (int)y, 13, Ui::ACCENT);
    y += 26.0f;

    Ui::Toggle({ x, y, w, 24.0f }, "True scale (compare sizes)", galleryTrueScale_);
    y += 30.0f;

    // The lighting section (#119). Flat colour is what makes a simple shape read as a
    // toy; these are the knobs that decide whether it stops doing that.
    Ui::Text("LIGHT", (int)x, (int)y, 12, Ui::ACCENT);
    y += 20.0f;
    Ui::Toggle({ x, y, w, 24.0f }, galleryLit_ ? "lit" : "unlit (flat colour)", galleryLit_);
    y += 30.0f;
    Ui::Slider({ x, y, w, 28.0f }, "angle", galleryLightAngle_, 0.0f, 2.0f * PI, "%.2f");
    y += 32.0f;
    Ui::Slider({ x, y, w, 28.0f }, "strength", galleryLightStrength_, 0.0f, 1.0f, "%.2f");
    y += 32.0f;
    Ui::Slider({ x, y, w, 28.0f }, "ambient floor", galleryAmbient_, 0.0f, 1.0f, "%.2f");
    y += 32.0f;
    Ui::Toggle({ x, y, w, 24.0f }, "second star (cooler, opposite)", gallerySecondLight_);
    y += 34.0f;

    // The half of a look that is not what the object is but what is happening to it.
    // These are the cases that have to read at a glance and the ones a static picture of
    // a healthy object never shows.
    Ui::Text("STATE", (int)x, (int)y, 12, Ui::ACCENT);
    y += 20.0f;
    Ui::Slider({ x, y, w, 28.0f }, "intensity (hull, ore, loot)", galleryIntensity_, 0.0f, 1.0f,
               "%.2f");
    y += 34.0f;
    Ui::Slider({ x, y, w, 28.0f }, "heading", galleryHeading_, 0.0f, 2.0f * PI, "%.2f");
    y += 34.0f;
    Ui::Toggle({ x, y, w, 24.0f }, "thrusting", galleryThrusting_);
    y += 38.0f;

    const std::vector<Archetype>& all = Archetypes::All();
    if (gallerySelected_ < 0 || gallerySelected_ >= (int)all.size())
    {
        Ui::Text("Select a card to edit its look.", (int)x, (int)y, 13, Ui::TEXT_DIM);
        return;
    }

    Archetype* a = Archetypes::Mutable(all[(size_t)gallerySelected_].id);
    if (a == nullptr)
        return;

    DrawLine((int)x, (int)y - 10, (int)(x + w), (int)y - 10, Ui::PANEL_BORDER);
    Ui::Text("LOOK", (int)x, (int)y, 12, Ui::ACCENT);
    Ui::Text(a->id.c_str(), (int)x + 46, (int)y + 1, 11, Ui::TEXT_DIM);
    y += 22.0f;

    // Colour is three sliders rather than a picker because the value that has to end up
    // in the file is the three numbers, and a picker would hide them.
    float r = a->visual.color.r, g = a->visual.color.g, b = a->visual.color.b;
    bool  colorChanged = false;
    colorChanged |= Ui::Slider({ x, y, w - 34.0f, 28.0f }, "red", r, 0.0f, 255.0f, "%.0f");
    y += 32.0f;
    colorChanged |= Ui::Slider({ x, y, w - 34.0f, 28.0f }, "green", g, 0.0f, 255.0f, "%.0f");
    y += 32.0f;
    colorChanged |= Ui::Slider({ x, y, w - 34.0f, 28.0f }, "blue", b, 0.0f, 255.0f, "%.0f");
    DrawRectangleRec({ x + w - 28.0f, y - 64.0f, 28.0f, 92.0f }, a->visual.color);
    DrawRectangleLinesEx({ x + w - 28.0f, y - 64.0f, 28.0f, 92.0f }, 1.0f, Ui::PANEL_BORDER);
    y += 34.0f;
    if (colorChanged)
    {
        a->visual.color = { (unsigned char)lroundf(r), (unsigned char)lroundf(g),
                            (unsigned char)lroundf(b), a->visual.color.a };
        NoteLookEdit(a->id, "color");
    }

    float size = a->defaultSize;
    if (Ui::Slider({ x, y, w, 28.0f }, "size (world units)", size, 4.0f, 1200.0f, "%.0f"))
    {
        a->defaultSize = roundf(size);
        NoteLookEdit(a->id, "size");
    }
    y += 34.0f;

    float layer = (float)a->visual.layer;
    if (Ui::Slider({ x, y, w, 28.0f }, "layer (draw order)", layer, -4.0f, 8.0f, "%.0f"))
    {
        a->visual.layer = (int)lroundf(layer);
        NoteLookEdit(a->id, "layer");
    }
    y += 34.0f;

    // What this archetype emits (#119). Here rather than in the JSON by hand because how
    // far a star reaches is the single number that decides whether a system reads as lit
    // or as a dark map with a lamp in the middle, and it is only decidable by looking.
    float lightR = a->visual.lightRadius;
    if (Ui::Slider({ x, y, w, 28.0f }, "light reach", lightR, 0.0f, 90000.0f, "%.0f"))
    {
        a->visual.lightRadius = roundf(lightR / 1000.0f) * 1000.0f;
        NoteLookEdit(a->id, "light");
    }
    y += 32.0f;
    float lightI = a->visual.lightIntensity;
    if (Ui::Slider({ x, y, w, 28.0f }, "light intensity", lightI, 0.0f, 2.0f, "%.2f"))
    {
        a->visual.lightIntensity = lightI;
        NoteLookEdit(a->id, "light");
    }
    y += 38.0f;

    Ui::Text("glyph", (int)x, (int)y + 5, 12, Ui::TEXT_DIM);
    // The glyph grammar is a small set of characters; cycling through the ones already in
    // use beats a text field, and it cannot produce an unrenderable one.
    // The characters the registry already uses, plus a few near them. Every glyph in
    // data must be in this list, or selecting that archetype would light nothing up.
    static const char* kGlyphs = "*#%~oO0AvxX@.:+=-/|\\";
    Rectangle          gr{ x + 60.0f, y, 28.0f, 26.0f };
    for (int k = 0; kGlyphs[k] != '\0'; k++)
    {
        const std::string ch(1, kGlyphs[k]);
        const bool        on = (a->visual.glyph == ch);
        const bool        over = CheckCollisionPointRec(GetMousePosition(), gr);
        DrawRectangleRec(gr, on ? Fade(Ui::ACCENT, 0.25f)
                                : (over ? Fade(Ui::ACCENT, 0.10f) : Fade(Ui::TITLE_BG, 0.7f)));
        DrawRectangleLinesEx(gr, 1.0f, on ? Ui::ACCENT : Ui::PANEL_BORDER);
        Ui::Text(ch.c_str(), (int)gr.x + 10, (int)gr.y + 4, 16, on ? Ui::ACCENT : Ui::TEXT);
        if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            a->visual.glyph = ch;
            NoteLookEdit(a->id, "glyph");
        }
        gr.x += 30.0f;
        if (gr.x + gr.width > x + w)
        {
            gr.x = x + 60.0f;
            gr.y += 30.0f;
        }
    }
    y = gr.y + 38.0f;

    Ui::Text("style", (int)x, (int)y + 5, 12, Ui::TEXT_DIM);
    Rectangle        sr{ x + 60.0f, y, (w - 60.0f) / 3.0f - 4.0f, 26.0f };
    const GlyphStyle styles[] = { GlyphStyle::Point, GlyphStyle::Region, GlyphStyle::Directional };
    for (GlyphStyle s : styles)
    {
        const bool on = (a->visual.style == s);
        const bool over = CheckCollisionPointRec(GetMousePosition(), sr);
        DrawRectangleRec(sr, on ? Fade(Ui::ACCENT, 0.25f)
                                : (over ? Fade(Ui::ACCENT, 0.10f) : Fade(Ui::TITLE_BG, 0.7f)));
        DrawRectangleLinesEx(sr, 1.0f, on ? Ui::ACCENT : Ui::PANEL_BORDER);
        Ui::Text(StyleName(s), (int)sr.x + 6, (int)sr.y + 6, 11, on ? Ui::ACCENT : Ui::TEXT);
        if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            a->visual.style = s;
            NoteLookEdit(a->id, "style");
        }
        sr.x += sr.width + 4.0f;
    }
    y += 40.0f;

    if (!lookEdits_.empty())
        Ui::Text(TextFormat("%d archetype(s) edited  ·  Ctrl+S writes archetypes.json",
                            (int)lookEdits_.size()),
                 (int)x, (int)y, 11, Ui::ACCENT);
}

void Editor::NoteLookEdit(const std::string& id, const std::string& key)
{
    lookEdits_[id].insert(key);
    archetypesDirty_ = true;
}

// The look field as the text that goes in the file. Formatted here because only this
// knows what a key means; ArchetypeEdit places the text and parses nothing.
static std::string LookFieldJson(const Archetype& a, const std::string& key)
{
    if (key == "glyph")
        return "\"" + a.visual.glyph + "\"";
    if (key == "style")
        return std::string("\"") + StyleName(a.visual.style) + "\"";
    if (key == "layer")
        return std::to_string(a.visual.layer);
    if (key == "size")
        return std::to_string((long long)llroundf(a.defaultSize));
    if (key == "light")
    {
        std::ostringstream os;
        // A fixed two decimals rather than %g, so the text a save produces is the text
        // the file already holds and re-saving an unchanged light changes nothing.
        os << "{ \"radius\": " << (long long)llroundf(a.visual.lightRadius)
           << ", \"intensity\": " << TextFormat("%.2f", (double)a.visual.lightIntensity) << " }";
        return os.str();
    }
    if (key == "color")
    {
        std::ostringstream os;
        os << "[" << (int)a.visual.color.r << ", " << (int)a.visual.color.g << ", "
           << (int)a.visual.color.b << ", " << (int)a.visual.color.a << "]";
        return os.str();
    }
    return std::string();
}

// Writes the edited fields back into data/archetypes.json, changing nothing else in it.
//
// All or nothing: if any one field cannot be placed, the file is left as it was. A look
// file half-written is worse than one not written, because the half that landed is
// indistinguishable from a look someone chose.
void Editor::SaveArchetypes()
{
    if (lookEdits_.empty())
        return;

    const std::string path = dataDir_ + "archetypes.json";
    std::string       text;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            TraceLog(LOG_WARNING, "Gallery: cannot read %s", path.c_str());
            return;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        text = buf.str();
    }

    for (const auto& entry : lookEdits_)
    {
        const Archetype* a = Archetypes::Find(entry.first);
        if (a == nullptr)
            continue;
        for (const std::string& key : entry.second)
        {
            const std::string value = LookFieldJson(*a, key);
            if (value.empty() || !ArchetypeEdit::SetField(text, a->id, key, value))
            {
                TraceLog(LOG_WARNING, "Gallery: cannot write %s of %s -- nothing saved",
                         key.c_str(), a->id.c_str());
                return;
            }
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
    {
        TraceLog(LOG_WARNING, "Gallery: cannot write %s", path.c_str());
        return;
    }
    out << text;
    out.close();

    lookEdits_.clear();
    archetypesDirty_ = false;
    TraceLog(LOG_INFO, "Gallery: saved %s", path.c_str());
}

// The treatment's settings, drawn over everything and never treated (#120). The same
// panel the game shows, so a look tuned here is the look tuned there.
void Editor::DrawTreatmentSettings()
{
    const float w = 320.0f;
    const float h = Render::TreatmentPanelHeight(treatment_) + 24.0f;
    Rectangle   panel{ 16.0f, 60.0f, w, fminf(h, (float)screenHeight_ - 80.0f) };

    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);

    BeginScissorMode((int)panel.x, (int)panel.y, (int)panel.width, (int)panel.height);
    Render::DrawTreatmentPanel(
        { panel.x + 12.0f, panel.y + 12.0f, panel.width - 24.0f, panel.height - 24.0f },
        treatment_);
    EndScissorMode();

    Ui::Text("F10 closes and saves", (int)panel.x + 12, (int)(panel.y + panel.height - 16.0f), 10,
             Ui::TEXT_DIM);
}
