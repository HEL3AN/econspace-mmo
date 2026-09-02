// Creating objects: the palette on the left, the preview in it, and what placing an
// entry writes into systemJson_. One translation unit of Editor (#17).

#include "Editor.h"

#include "ui/UiTheme.h"
#include "ui/Button.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

#include "entities/Planet.h"
#include "entities/Station.h"
#include "entities/AsteroidField.h"
#include "entities/Nebula.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"

namespace
{
// The creation palette comes from the archetype registry, not from a list kept here
// (#37). Adding an archetype to data/archetypes.json puts it in the editor with no
// editor change at all, which is the property that makes the world data-driven rather
// than merely stored as data.
std::vector<const Archetype*> PlaceableArchetypes()
{
    std::vector<const Archetype*> out;
    for (const Archetype& a : Archetypes::All())
        if (a.Placeable())
            out.push_back(&a);
    return out;
}

// What an archetype can do, as a line of palette text. Derived rather than written down,
// so it cannot go stale the way the hand-written descriptions did.
std::string ComponentSummary(const Archetype& a)
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

const float kPaletteRowH = 40.0f;
const float kPaletteW = 232.0f;
const float kPaletteBtnH = 30.0f;
const int   kPaletteMaxRows = 12;  // beyond this the list scrolls rather than leaving the screen
}  // namespace

static Rectangle PaletteButtonRect(int screenHeight)
{
    return { 6.0f, (float)screenHeight - kPaletteBtnH - 8.0f, kPaletteW, kPaletteBtnH };
}

Rectangle Editor::PaletteRect() const
{
    Rectangle btn = PaletteButtonRect(screenHeight_);
    if (!paletteOpen_)
        return btn;
    int rows = (int)PlaceableArchetypes().size();
    if (rows > kPaletteMaxRows)
        rows = kPaletteMaxRows;
    float listH = rows * kPaletteRowH;
    float top = btn.y - 6.0f - listH;
    return { btn.x, top, kPaletteW, (btn.y + btn.height) - top };
}

// Object preview: draw the real entity in a small box with the scale fitted.
void Editor::DrawEntityPreview(Rectangle box, const std::string& archetypeId)
{
    std::unique_ptr<Entity> e = MakeEntity(archetypeId, { 0.0f, 0.0f });
    if (!e)
        return;
    float s = e->GetSize();
    if (s < 1.0f)
        s = 1.0f;

    Camera2D cam{};
    cam.offset = { box.x + box.width / 2.0f, box.y + box.height / 2.0f };
    cam.target = { 0.0f, 0.0f };
    cam.zoom = (fminf(box.width, box.height) * 0.42f) / s;

    BeginScissorMode((int)box.x, (int)box.y, (int)box.width, (int)box.height);
    BeginMode2D(cam);
    backend_->Draw(e->Describe());
    EndMode2D();
    EndScissorMode();
}

// Creation palette at the bottom-left: the CREATE label button expands the list
// upward. Each card is a preview, a name, a description. A click enters placement mode.
void Editor::DrawPalette()
{
    if (mode_ == Mode::Galaxy)
        return;  // in galaxy mode there's no object palette

    Vector2   m = GetMousePosition();
    Rectangle btn = PaletteButtonRect(screenHeight_);

    if (paletteOpen_)
    {
        const std::vector<const Archetype*> all = PlaceableArchetypes();
        const int                           total = (int)all.size();
        const int   shown = total < kPaletteMaxRows ? total : kPaletteMaxRows;
        const float listH = shown * kPaletteRowH;
        Rectangle   list{ btn.x, btn.y - 6.0f - listH, kPaletteW, listH };
        DrawRectangleRec(list, Ui::PANEL_BG);
        DrawRectangleLinesEx(list, 1.0f, Ui::PANEL_BORDER);

        // The registry can hold more archetypes than fit on screen, and will once players
        // can build. Scroll rather than clip, so a new archetype is never simply absent.
        const int maxScroll = total - shown;
        if (CheckCollisionPointRec(m, list))
        {
            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f)
                paletteScroll_ -= (int)wheel;
        }
        if (paletteScroll_ > maxScroll)
            paletteScroll_ = maxScroll;
        if (paletteScroll_ < 0)
            paletteScroll_ = 0;

        for (int i = 0; i < shown; i++)
        {
            const Archetype& a = *all[(size_t)(i + paletteScroll_)];
            Rectangle        row{ list.x + 6, list.y + 3 + i * kPaletteRowH, list.width - 12,
                                  kPaletteRowH - 6 };
            bool             active = (placeArchetype_ == a.id);
            bool             hover = CheckCollisionPointRec(m, row);
            DrawRectangleRec(row,
                             active ? Fade(Ui::ACCENT, 0.22f)
                                    : (hover ? Fade(Ui::ACCENT, 0.10f) : Fade(Ui::TITLE_BG, 0.6f)));
            DrawRectangleLinesEx(row, 1.0f, active ? Ui::ACCENT : Ui::PANEL_BORDER);

            Rectangle icon{ row.x + 4, row.y + 3, 28, row.height - 6 };
            DrawRectangleRec(icon, Fade(BLACK, 0.4f));
            DrawEntityPreview(icon, a.id);

            Ui::Text(a.name.c_str(), (int)icon.x + 36, (int)row.y + 4, 15,
                     active ? Ui::ACCENT : Ui::TEXT);
            Ui::Text(ComponentSummary(a).c_str(), (int)icon.x + 36, (int)row.y + 21, 11,
                     Ui::TEXT_DIM);

            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                placeArchetype_ = active ? std::string() : a.id;  // repeat click cancels
        }

        if (maxScroll > 0)
            Ui::Text(TextFormat("%d/%d", paletteScroll_ + shown, total),
                     (int)(list.x + list.width - 42), (int)(list.y + listH - 15), 11, Ui::TEXT_DIM);
    }

    // The label button (always shown). A click collapses/expands.
    bool overBtn = CheckCollisionPointRec(m, btn);
    DrawRectangleRec(btn, overBtn ? Fade(Ui::ACCENT, 0.18f) : Ui::TITLE_BG);
    DrawRectangleLinesEx(btn, 1.0f, paletteOpen_ ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(TextFormat("CREATE  %s", paletteOpen_ ? "[-]" : "[+]"), (int)btn.x + 10,
             (int)btn.y + 7, 16, Ui::ACCENT);
    if (overBtn && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        paletteOpen_ = !paletteOpen_;
        if (!paletteOpen_)
            placeArchetype_.clear();  // collapsed — cancel placement mode
    }
}

// Creates an entity of the archetype with default values at position pos (for previews
// pos = {0,0}). Used for the ghost and the palette preview.
//
// Size and subtype come from the archetype; only the C++ constructor to call still comes
// from a switch here, and that is the part #34 deletes last.
std::unique_ptr<Entity> Editor::MakeEntity(const std::string& archetypeId, Vector2 pos) const
{
    const Archetype* a = Archetypes::Find(archetypeId);
    if (a == nullptr)
        return nullptr;
    const float size = a->defaultSize > 0.0f ? a->defaultSize : 100.0f;

    switch (a->kind)
    {
        case EntityKind::Planet:
        {
            float      r = sqrtf(pos.x * pos.x + pos.y * pos.y);
            PlanetType type = PlanetTypeFromString(a->worldSubType);
            return std::make_unique<Planet>(r, 300.0f, atan2f(pos.y, pos.x), size,
                                            PlanetTypeColor(type), ResourceType::Iron, type);
        }
        case EntityKind::Station:
            return std::make_unique<Station>(pos, size, "New " + a->name, FactionId::Independent,
                                             StationRoleFromString(a->worldSubType));
        case EntityKind::Field:
            return std::make_unique<AsteroidField>(pos, size, "New Belt", ResourceType::Iron, 200);
        case EntityKind::Nebula: return std::make_unique<Nebula>(pos, size, "New Nebula");
        case EntityKind::Derelict:
            return std::make_unique<Derelict>(pos, size, "New Derelict", 500.0);
        case EntityKind::Gate: return std::make_unique<JumpGate>(pos, size, "New Gate", "");
        case EntityKind::Star:
        case EntityKind::Npc:
        case EntityKind::PlayerShip:
        case EntityKind::Unknown: return nullptr;  // not placed by hand; see Archetype::Placeable
    }
    return nullptr;
}

// Adds an object of the archetype at position pos with default values and selects it.
// The JSON written is the same the runtime reads -- that property is not negotiable, and
// the archetype supplies the size and the type/role rather than a constant here.
void Editor::AddObject(const std::string& archetypeId, Vector2 pos)
{
    const Archetype* a = Archetypes::Find(archetypeId);
    if (a == nullptr || !a->Placeable())
        return;

    const std::string category = a->worldCategory;
    const int         px = (int)roundf(pos.x), py = (int)roundf(pos.y);
    const int         size = (int)roundf(a->defaultSize > 0.0f ? a->defaultSize : 100.0f);
    json              o = json::object();

    if (category == "planets")
    {
        float r = sqrtf(pos.x * pos.x + pos.y * pos.y);
        if (r < 500.0f)
            r = 4000.0f;
        o = { { "orbitRadius", (int)roundf(r) }, { "orbitSpeed", 300 },
              { "angle", atan2f(pos.y, pos.x) }, { "size", size },
              { "type", a->worldSubType },       { "deposit", "Iron" } };
    }
    else if (category == "stations")
        o = { { "name", "New " + a->name },
              { "pos", { px, py } },
              { "size", size },
              { "faction", "Independent" },
              { "role", a->worldSubType } };
    else if (category == "asteroidFields")
        o = { { "name", "New Belt" },
              { "pos", { px, py } },
              { "size", size },
              { "resource", "Iron" },
              { "ore", 200 } };
    else if (category == "nebulae")
        o = { { "name", "New Nebula" }, { "pos", { px, py } }, { "radius", size } };
    else if (category == "derelicts")
        o = {
            { "name", "New Derelict" }, { "pos", { px, py } }, { "size", size }, { "reward", 500 }
        };
    else if (category == "gates")
        o = { { "name", "New Gate" },
              { "pos", { px, py } },
              { "size", size },
              { "destination", universe_.systems.empty() ? "" : universe_.systems[0].id } };
    else
        return;

    if (!systemJson_.contains(category) || !systemJson_[category].is_array())
        systemJson_[category] = json::array();
    systemJson_[category].push_back(o);

    dirty_ = true;
    RebuildEntities();

    // Select the one just added (the last handle of this category).
    selected_ = -1;
    for (int i = (int)handles_.size() - 1; i >= 0; i--)
        if (handles_[i].category == category)
        {
            selected_ = i;
            break;
        }
    activeField_.clear();
    openDropdown_.clear();
}

void Editor::DeleteSelected()
{
    if (selected_ < 0 || selected_ >= (int)handles_.size())
        return;
    const ObjHandle h = handles_[selected_];
    if (h.category == "star")
        return;  // we don't delete the star

    if (systemJson_.contains(h.category) && systemJson_[h.category].is_array() &&
        h.index < (int)systemJson_[h.category].size())
        systemJson_[h.category].erase(h.index);

    dirty_ = true;
    RebuildEntities();
    selected_ = -1;
    activeField_.clear();
    openDropdown_.clear();
}

// The top buttons (mode + save) are centered as a single group.
