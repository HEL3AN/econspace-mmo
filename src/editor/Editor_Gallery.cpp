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

#include <algorithm>
#include "render/Modules.h"

#include "core/ArchetypeEdit.h"
#include "render/TreatmentPanel.h"
#include "ui/Controls.h"
#include "ui/UiTheme.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
const float kZoomMin = 0.25f;
const float kZoomMax = 16.0f;

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

void Editor::FocusGallery(const std::string& which, float zoom)
{
    EnterGalleryMode(true);
    const std::vector<Archetype>& all = Archetypes::All();
    int                           index = -1;
    if (!which.empty() && which.find_first_not_of("0123456789") == std::string::npos)
        index = std::atoi(which.c_str());
    else
        for (int i = 0; i < (int)all.size(); i++)
            if (all[(size_t)i].id == which)
                index = i;
    if (index < 0 || index >= (int)all.size())
    {
        TraceLog(LOG_WARNING, "Gallery: no archetype '%s' (%d in the registry)", which.c_str(),
                 (int)all.size());
        return;
    }
    gallerySelected_ = index;
    galleryFocus_ = true;
    galleryZoom_ = Clamp(zoom, kZoomMin, kZoomMax);
    galleryPan_ = { 0.0f, 0.0f };
}

void Editor::UseShapes()
{
    backend_ = &shapeBackend_;
}

void Editor::EnterGalleryMode(bool on)
{
    mode_ = on ? Mode::Gallery : Mode::System;
    galleryFocus_ = false;
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
    if (galleryFocus_)
        return -1;  // the grid is not on the screen
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
    it.id = gallerySeed_;
    return it;
}

void Editor::HandleGalleryInput()
{
    const Vector2 m = GetMousePosition();
    const bool    overPanel = m.x > (float)screenWidth_ - kPanelW;

    const int count = (int)Archetypes::All().size();

    if (galleryFocus_)
    {
        if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || gallerySelected_ < 0 ||
            gallerySelected_ >= count)
        {
            galleryFocus_ = false;
            return;
        }
        // Stepping keeps the zoom: the question is usually how the same detail reads on
        // the next object, at the same size on the screen.
        if (IsKeyPressed(KEY_RIGHT) && gallerySelected_ + 1 < count)
        {
            gallerySelected_++;
            galleryPan_ = { 0.0f, 0.0f };
        }
        if (IsKeyPressed(KEY_LEFT) && gallerySelected_ > 0)
        {
            gallerySelected_--;
            galleryPan_ = { 0.0f, 0.0f };
        }
        if (IsKeyPressed(KEY_HOME))
        {
            galleryZoom_ = 1.0f;
            galleryPan_ = { 0.0f, 0.0f };
        }
        if (IsKeyPressed(KEY_RIGHT_BRACKET))
            gallerySeed_++;
        if (IsKeyPressed(KEY_LEFT_BRACKET))
            gallerySeed_--;
        if (IsKeyPressed(KEY_K))
            gallerySockets_ = !gallerySockets_;
        if (overPanel || m.y < kGridTop)
            return;  // the panel and the zoom slider take their own mouse

        // The wheel zooms about the cursor, so a part can be walked into rather than
        // zoomed past and then dragged back.
        const Archetype& a = Archetypes::All()[(size_t)gallerySelected_];
        const float      wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
        {
            const Vector2 under = GetScreenToWorld2D(m, GalleryFocusCamera(a));
            galleryZoom_ = Clamp(galleryZoom_ * powf(1.15f, wheel), kZoomMin, kZoomMax);
            const Vector2 now = GetScreenToWorld2D(m, GalleryFocusCamera(a));
            galleryPan_ = Vector2Add(galleryPan_, Vector2Subtract(under, now));
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
        {
            const float z = GalleryFocusCamera(a).zoom;
            galleryPan_ = Vector2Subtract(galleryPan_, Vector2Scale(GetMouseDelta(), 1.0f / z));
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        EnterGalleryMode(false);
        return;
    }
    if (IsKeyPressed(KEY_ENTER) && gallerySelected_ >= 0 && gallerySelected_ < count)
    {
        galleryFocus_ = true;
        galleryPan_ = { 0.0f, 0.0f };
        return;
    }

    if (!overPanel)
    {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            galleryScroll_ -= wheel * 48.0f;

        // Never scroll past the last row: an empty screen looks like a crash.
        const float lastBottom =
            count > 0 ? GalleryCardRect(count - 1).y + galleryScroll_ + kCardH : kGridTop;
        const float maxScroll = fmaxf(0.0f, lastBottom + 16.0f - (float)screenHeight_);
        galleryScroll_ = Clamp(galleryScroll_, 0.0f, maxScroll);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            const int hit = GalleryHit(m);
            activeField_.clear();
            // A second click on the selected card opens it large (#194): the first click
            // already said "this one", so the second can only mean "closer".
            if (hit >= 0 && hit == gallerySelected_)
            {
                galleryFocus_ = true;
                galleryPan_ = { 0.0f, 0.0f };
            }
            else
                gallerySelected_ = hit;
        }
    }
}

// ---- The focused view (#194) --------------------------------------------------------
//
// A card draws an object at most ~77 px across, and a part with a larger minPixels -- a
// station's lamps, a mast -- never appears on one. This is the same object over the whole
// grid, with a zoom, so the detail that is decided by size can be judged at every size.

Rectangle Editor::GalleryFocusRect() const
{
    return { kGridLeft, kGridTop, (float)screenWidth_ - kPanelW - kGridLeft * 2.0f,
             (float)screenHeight_ - kGridTop - 16.0f };
}

// Fitted the way a card is -- to what the object reaches, not to its radius -- so zoom 1
// is the card's picture at the size of the screen, and the zoom is a multiple of that.
// "True scale" does not apply: one object has nothing to be compared with.
Camera2D Editor::GalleryFocusCamera(const Archetype& a) const
{
    const Rectangle box = GalleryFocusRect();
    const float     size = a.defaultSize > 0.0f ? a.defaultSize : 100.0f;
    const float     reach = size * Render::Extent(a.visual.shape);
    Camera2D        cam{};
    cam.offset = { box.x + box.width / 2.0f, box.y + box.height / 2.0f };
    cam.target = galleryPan_;
    cam.zoom = (fminf(box.width, box.height) * 0.45f) / fmaxf(reach, 1e-3f) * galleryZoom_;
    return cam;
}

void Editor::DrawGalleryFocus()
{
    const Archetype& a = Archetypes::All()[(size_t)gallerySelected_];
    const Rectangle  box = GalleryFocusRect();
    const float      size = a.defaultSize > 0.0f ? a.defaultSize : 100.0f;
    const Camera2D   cam = GalleryFocusCamera(a);

    DrawRectangleRec(box, Fade(Ui::PANEL_BG, 0.75f));
    BeginClip(box);
    BeginMode2D(cam);
    Render::Present({ GalleryItem(a, { 0.0f, 0.0f }, size) }, GalleryLighting({ 0.0f, 0.0f }, size),
                    cam, *backend_);
    if (gallerySockets_)
    {
        // Where the sections offer a place, coloured by kind, with a tick for which way it
        // faces. Ranges are drawn at their low end, as the module box is measured.
        std::vector<Render::Part> sections;
        for (const Render::Part& p : a.visual.shape.parts)
            if (p.section)
                sections.push_back(p);
        const float px = 1.0f / cam.zoom;
        for (const Render::Socket& s : Render::Sockets(sections))
        {
            const Color   c = s.type == "edge"     ? Color{ 90, 200, 255, 255 }
                              : s.type == "end"    ? Color{ 255, 170, 60, 255 }
                              : s.type == "top"    ? Color{ 140, 255, 140, 255 }
                              : s.type == "middle" ? Color{ 255, 255, 255, 255 }
                                                   : Color{ 230, 120, 255, 255 };
            const Vector2 at = { s.pos.x * size, s.pos.y * size };
            DrawCircleV(at, 3.0f * px, c);
            DrawLineEx(at,
                       { at.x + cosf(s.angle * DEG2RAD) * 9.0f * px,
                         at.y + sinf(s.angle * DEG2RAD) * 9.0f * px },
                       1.5f * px, c);
        }
    }
    EndMode2D();
    EndScissorMode();
}

// Drawn after the treatment, as the HUD is: a number that is being read must stay literal.
void Editor::DrawGalleryFocusBar()
{
    const std::vector<Archetype>& all = Archetypes::All();
    if (!galleryFocus_ || gallerySelected_ < 0 || gallerySelected_ >= (int)all.size())
        return;
    const Archetype& a = all[(size_t)gallerySelected_];
    const Rectangle  box = GalleryFocusRect();
    const float      size = a.defaultSize > 0.0f ? a.defaultSize : 100.0f;

    DrawRectangleLinesEx(box, 1.0f, Ui::PANEL_BORDER);
    Ui::LogSlider({ 16.0f, 96.0f, 300.0f, 28.0f }, "zoom (x fitted)", galleryZoom_, kZoomMin,
                  kZoomMax, "%.2f");

    // What minPixels is compared against (Silhouette.cpp): the object's diameter on the
    // screen. Shown as a number, with what it hides, because a part missing at one size and
    // present at the next is exactly what this view exists to make visible.
    const float pixels = size * 2.0f * GalleryFocusCamera(a).zoom;
    int         hidden = 0;
    float       nextAt = 0.0f;
    for (const Render::Part& p : a.visual.shape.parts)
        if (p.minPixels > 0.0f && pixels < p.minPixels)
        {
            hidden++;
            nextAt = nextAt > 0.0f ? fminf(nextAt, p.minPixels) : p.minPixels;
        }
    std::string line = TextFormat("%s   %.0f px across", a.id.c_str(), (double)pixels);
    if (hidden > 0)
        line += TextFormat("   %d part(s) hidden below minPixels, next at %.0f px", hidden,
                           (double)nextAt);
    else if (!a.visual.shape.parts.empty())
        line += "   every part drawn";
    Ui::Text(line.c_str(), 332, 103, 13, hidden > 0 ? Ui::ACCENT : Ui::TEXT);

    Ui::Text(TextFormat("%d of %d   <- / -> step   wheel: zoom   right-drag: pan   Home: reset   "
                        "Esc: back to the grid",
                        gallerySelected_ + 1, (int)all.size()),
             (int)box.x + 10, (int)(box.y + box.height - 22.0f), 12, Ui::TEXT_DIM);
}

void Editor::DrawGallery()
{
    const std::vector<Archetype>& all = Archetypes::All();

    if (galleryFocus_ && gallerySelected_ >= 0 && gallerySelected_ < (int)all.size())
    {
        DrawGalleryFocus();
        return;
    }

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

        // Fitted to what the object actually reaches, not to its radius: a docking ring at
        // 1.55 radii is the point of a docking ring, and a card that framed the radius
        // would cut it off (#122).
        const float reach = size * Render::Extent(a.visual.shape);

        Camera2D cam{};
        cam.offset = { box.x + box.width / 2.0f, box.y + box.height / 2.0f };
        cam.target = { 0.0f, 0.0f };
        cam.zoom = (fminf(box.width, box.height) * 0.42f) / (galleryTrueScale_ ? largest : reach);

        BeginClip(Clip(grid, box));
        BeginMode2D(cam);
        // Through Present rather than Draw, so the card is lit by the same path the world
        // is -- and so a card can never inherit the lights of whatever drew last.
        Render::Present({ GalleryItem(a, { 0.0f, 0.0f }, size) },
                        GalleryLighting({ 0.0f, 0.0f }, size), cam, *backend_);
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
    if (Ui::Slider({ x, y, w, 28.0f }, "size (world units)", size, 4.0f, 200000.0f, "%.0f"))
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
    if (Ui::Slider({ x, y, w, 28.0f }, "light reach", lightR, 0.0f, 4000000.0f, "%.0f"))
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

    // Which material shades it (#121). A row of buttons rather than a text field: the set
    // is small, the ids are exact, and a typo here is silent -- the object just draws plain.
    Ui::Text("material", (int)x, (int)y + 5, 12, Ui::TEXT_DIM);
    {
        Rectangle mr{ x + 70.0f, y, 62.0f, 26.0f };
        auto      pick = [&](const std::string& id, const char* label)
        {
            const bool on = (a->visual.material == id);
            if (Ui::SmallButton(mr, label, on) && !on)
            {
                a->visual.material = id;
                NoteLookEdit(a->id, "material");
            }
            mr.x += mr.width + 4.0f;
        };
        pick("", "none");
        for (const Render::Material& m : Render::Materials::All())
            pick(m.id, m.id.c_str());
    }
    y += 34.0f;

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
    if (key == "material")
        return "\"" + a.visual.material + "\"";
    if (key == "light")
    {
        std::ostringstream os;
        // A fixed two decimals rather than %g, so the text a save produces is the text
        // the file already holds and re-saving an unchanged light changes nothing.
        os << "{ \"radius\": " << (long long)llroundf(a.visual.lightRadius)
           << ", \"intensity\": " << TextFormat("%.2f", (double)a.visual.lightIntensity);
        if (a.visual.lightSelf)
            os << ", \"self\": true";  // written only when set, like the file holds it
        os << " }";
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
    const float h = Render::TreatmentPanelHeight(treatment_, &materials_) + 24.0f;
    Rectangle   panel{ 16.0f, 60.0f, w, fminf(h, (float)screenHeight_ - 80.0f) };

    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);

    BeginScissorMode((int)panel.x, (int)panel.y, (int)panel.width, (int)panel.height);
    Render::DrawTreatmentPanel(
        { panel.x + 12.0f, panel.y + 12.0f, panel.width - 24.0f, panel.height - 24.0f }, treatment_,
        &materials_);
    EndScissorMode();

    Ui::Text("F10 closes and saves", (int)panel.x + 12, (int)(panel.y + panel.height - 16.0f), 10,
             Ui::TEXT_DIM);
}

// ---- The module library (#240) ----------------------------------------------------------
//
// Every variant of every module on one sheet, lit as the gallery lights an object, so the
// library is judged as a library: does a hatch read as a hatch, do its variants differ
// enough, does one style hold across all of them. `labels` is the second pass, drawn after
// the screen treatment like every tool.
// The packs in load order, "every pack" first.
static std::vector<std::string> ModulePacks()
{
    std::vector<std::string> packs{ "" };
    for (const Render::Module& m : Render::Modules::All())
        if (std::find(packs.begin(), packs.end(), m.pack) == packs.end())
            packs.push_back(m.pack);
    return packs;
}

void Editor::HandleModulesInput()
{
    if (IsKeyPressed(KEY_TAB))
    {
        const std::vector<std::string> packs = ModulePacks();
        const int                      n = (int)packs.size();
        const auto                     at = std::find(packs.begin(), packs.end(), modulesPack_);
        const int                      k = at == packs.end() ? 0 : (int)(at - packs.begin());
        modulesPack_ = packs[(k + (IsKeyDown(KEY_LEFT_SHIFT) ? n - 1 : 1)) % n];
        modulesFocus_.clear();
        modulesScroll_ = 0.0f;
    }
    // More or fewer seeds of each variant.
    if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_EQUAL))
        modulesSeeds_ = std::min(8, modulesSeeds_ + 1);
    if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_MINUS))
        modulesSeeds_ = std::max(1, modulesSeeds_ - 1);

    // The wheel scrolls and, with Ctrl held, sizes the cards. Dragging with the right or
    // middle button scrolls too, and the keys do what they do on any long page.
    const float wheel = GetMouseWheelMove();
    const bool  ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (wheel != 0.0f && ctrl)
        modulesZoom_ = fmaxf(0.4f, fminf(4.0f, modulesZoom_ * (wheel > 0 ? 1.15f : 1.0f / 1.15f)));
    else if (wheel != 0.0f)
        modulesScroll_ -= wheel * 120.0f;
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
        modulesScroll_ -= GetMouseDelta().y;
    const float page = (float)screenHeight_ - 120.0f;
    if (IsKeyPressed(KEY_PAGE_DOWN) || IsKeyPressed(KEY_SPACE))
        modulesScroll_ += page;
    if (IsKeyPressed(KEY_PAGE_UP))
        modulesScroll_ -= page;
    if (IsKeyDown(KEY_DOWN))
        modulesScroll_ += 14.0f;
    if (IsKeyDown(KEY_UP))
        modulesScroll_ -= 14.0f;
    if (IsKeyPressed(KEY_HOME))
        modulesScroll_ = 0.0f;
    if (IsKeyPressed(KEY_END))
        modulesScroll_ = 1e9f;
    const float most = fmaxf(0.0f, modulesContent_ - ((float)screenHeight_ - 90.0f));
    modulesScroll_ = fmaxf(0.0f, fminf(most, modulesScroll_));

    // A click opens a module large; Esc, Backspace or a click on nothing goes back.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (modulesFocus_.empty() && !modulesHover_.empty())
        {
            modulesFocus_ = modulesHover_;
            modulesScroll_ = 0.0f;
        }
        else if (!modulesFocus_.empty() && modulesHover_.empty())
            modulesFocus_.clear();
    }
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE))
        modulesFocus_.clear();
}

void Editor::DrawModules(bool labels)
{
    const bool focused = !modulesFocus_.empty();
    auto       shown = [&](const Render::Module& m)
    { return focused ? m.id == modulesFocus_ : modulesPack_.empty() || m.pack == modulesPack_; };
    // Opened, one module's variants down the page and six seeds of each across it.
    const int seedsShown = focused ? 6 : modulesSeeds_;

    const float left = 24.0f, top = 84.0f;
    const float width = (float)screenWidth_ - left * 2.0f - 12.0f;
    const float cell =
        fmaxf(48.0f, (focused ? fminf(260.0f, width / (float)seedsShown) : 132.0f) * modulesZoom_);
    const int columns = (int)fmaxf(1.0f, width / cell);

    struct Card
    {
        std::string module, label;
        int         seed;
        Vector2     centre;
    };
    std::vector<Card>          cards;
    std::vector<Render::Shape> shapes;  // kept alive for the frame: an item borrows its shape
    int                        slot = 0;
    for (const Render::Module& m : Render::Modules::All())
        if (shown(m))
            for (const Render::ModuleVariant& v : m.variants)
            {
                // A variant starts a new row when its seeds fit on one, so the eye reads
                // across one family rather than across a seam between two.
                if (seedsShown <= columns && slot % columns + seedsShown > columns)
                    slot += columns - slot % columns;
                for (int seed = 1; seed <= seedsShown; seed++, slot++)
                {
                    Render::Part p;
                    p.module = m.id;
                    p.variant = v.id;
                    p.scale = 1.0f;
                    Render::Shape sh;
                    sh.parts.push_back(p);
                    shapes.push_back(sh);
                    const int col = slot % columns, row = slot / columns;
                    cards.push_back(
                        { m.id,
                          seed == 1 ? m.id + " / " + v.id : std::string(TextFormat("#%d", seed)),
                          seed,
                          { left + cell * ((float)col + 0.5f),
                            top + cell * ((float)row + 0.5f) - modulesScroll_ } });
                }
            }
    modulesContent_ = cell * (float)((slot + columns - 1) / columns) + 20.0f;

    if (labels)
        modulesHover_.clear();
    const Vector2 mouse = GetMousePosition();
    Camera2D      cam{};
    cam.zoom = 1.0f;
    for (size_t i = 0; i < cards.size(); i++)
    {
        const Card& c = cards[i];
        const float half = cell * 0.47f;
        // Off the page, or partly under the header: a card is drawn whole or not at all.
        if (c.centre.y - half < top - 1.0f || c.centre.y - half > (float)screenHeight_)
            continue;
        const Rectangle box{ c.centre.x - half, c.centre.y - half, half * 2.0f, half * 2.0f };
        if (labels)
        {
            const bool hover = CheckCollisionPointRec(mouse, box);
            if (hover)
                modulesHover_ = c.module;
            DrawRectangleLinesEx(box, 1.0f, hover ? Ui::ACCENT : Fade(Ui::PANEL_BORDER, 0.6f));
            Ui::Text(c.label.c_str(), (int)(c.centre.x - cell * 0.44f),
                     (int)(c.centre.y + cell * (c.label[0] == '#' ? -0.44f : 0.36f)),
                     cell > 180.0f ? 16 : 12, Ui::TEXT_DIM);
            continue;
        }
        Render::Item it;
        it.pos = c.centre;
        it.size = cell * 0.3f;
        it.color = { 168, 168, 176, 255 };
        it.material = "hull";
        it.shape = &shapes[i];
        it.heading = 0.0f;
        it.id = c.seed;
        Render::Present({ it }, GalleryLighting(c.centre, it.size), cam, *backend_);
    }
    if (!labels)
        return;

    Ui::Text("MODULES", (int)left, 14, 22, Ui::ACCENT);
    if (focused)
    {
        Ui::Text(modulesFocus_.c_str(), (int)left + 140, 18, 18, Ui::TEXT);
        Ui::Text("6 seeds of each variant   click outside / Esc: back   wheel: scroll   "
                 "Ctrl+wheel: size",
                 (int)left, 52, 13, Ui::TEXT_DIM);
    }
    else
    {
        // Every pack named, the one shown lit: Tab steps through them.
        int x = (int)left + 140;
        for (const std::string& p : ModulePacks())
        {
            const char* name = p.empty() ? "all" : p.c_str();
            Ui::Text(name, x, 20, 14, p == modulesPack_ ? Ui::ACCENT : Ui::TEXT_DIM);
            x += MeasureText(name, 14) + 18;
        }
        Ui::Text(TextFormat("Tab: next pack   wheel / right-drag: scroll   Ctrl+wheel: size   "
                            "[ ]: seeds (%d)   click: open a module   F2: backend",
                            modulesSeeds_),
                 (int)left, 52, 13, Ui::TEXT_DIM);
    }
    const float view = (float)screenHeight_ - top;
    if (modulesContent_ > view)
    {
        // Where on the page you are.
        const float h = fmaxf(30.0f, view * view / modulesContent_);
        const float y = top + (view - h) * modulesScroll_ / fmaxf(1.0f, modulesContent_ - view);
        DrawRectangle(screenWidth_ - 10, (int)y, 4, (int)h, Fade(Ui::ACCENT, 0.6f));
    }
}
