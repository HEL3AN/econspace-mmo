#include "Editor.h"

#include "core/World.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "entities/AsteroidField.h"
#include "entities/Nebula.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "render/Textures.h"
#include "ui/UiTheme.h"
#include "ui/Button.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;

// Descriptions of object types for the creation palette (shared by palette and hints).
namespace
{
struct PaletteType
{
    const char* category;
    const char* title;
    const char* desc;
};
const PaletteType kPalette[] = {
    { "planets", "Planet", "orbit, type, deposit" },
    { "stations", "Station", "faction, role, market" },
    { "asteroidFields", "Asteroid Belt", "resource, ore" },
    { "nebulae", "Nebula", "hides from pirates" },
    { "derelicts", "Derelict", "lootable wreck" },
    { "gates", "Jump Gate", "links to a system" },
};
const int   kPaletteN = (int)(sizeof(kPalette) / sizeof(kPalette[0]));
const float kPaletteRowH = 54.0f;
const float kPaletteW = 210.0f;
const float kPaletteBtnH = 30.0f;
}  // namespace

Editor::Editor()
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screenWidth_, screenHeight_, "EconSpace — World Editor");
    SetWindowMinSize(960, 600);
    SetTargetFPS(60);
    Ui::LoadAssets();

#ifdef EDITOR_DATA_DIR
    dataDir_ = EDITOR_DATA_DIR;  // the repository's source data/ folder
#else
    dataDir_ = std::string(GetApplicationDirectory()) + "data/";
#endif
    Factions::Load(dataDir_ + "factions.json");  // faction properties/relations
    if (!Archetypes::Load(dataDir_ + "archetypes.json"))
        TraceLog(LOG_ERROR, "Archetypes: %s", Archetypes::Error().c_str());
    universe_ = WorldLoader::LoadUniverse(dataDir_ + "universe.json");
    {
        std::ifstream uf(dataDir_ + "universe.json");
        if (uf.is_open())
        {
            universeJson_ = json::parse(uf, nullptr, false);
            if (universeJson_.is_discarded())
                universeJson_ = json::object();
        }
    }

    currentSystem_ = 0;
    for (size_t i = 0; i < universe_.systems.size(); i++)
        if (universe_.systems[i].id == universe_.startId)
        {
            currentSystem_ = (int)i;
            break;
        }

    camera_ = {};
    camera_.target = { 0.0f, 0.0f };
    camera_.offset = { screenWidth_ / 2.0f, screenHeight_ / 2.0f };
    camera_.rotation = 0.0f;
    camera_.zoom = 0.03f;

    if (!universe_.systems.empty())
        LoadSystemAt(currentSystem_);
}

Editor::~Editor()
{
    Tex::Unload();
    Ui::UnloadAssets();
    CloseWindow();
}

void Editor::LoadSystemAt(int index)
{
    if (index < 0 || index >= (int)universe_.systems.size())
        return;
    currentSystem_ = index;
    selected_ = -1;
    dirty_ = false;
    activeField_.clear();
    openDropdown_.clear();

    std::string   path = dataDir_ + "systems/" + universe_.systems[index].file;
    std::ifstream file(path);
    if (file.is_open())
    {
        systemJson_ = json::parse(file, nullptr, false);
        if (systemJson_.is_discarded())
            systemJson_ = json::object();
    }
    else
    {
        systemJson_ = json::object();
    }

    RebuildEntities();
}

void Editor::RebuildEntities()
{
    entities_ = WorldLoader::BuildSystem(systemJson_);

    // Handles in the same order as the entities in BuildSystem.
    handles_.clear();
    if (systemJson_.contains("star"))
        handles_.push_back({ "star", -1 });  // matches the star in BuildSystem
    const char* arrays[] = { "planets", "stations",  "asteroidFields",
                             "nebulae", "derelicts", "gates" };
    for (const char* key : arrays)
        if (systemJson_.contains(key))
            for (int i = 0; i < (int)systemJson_[key].size(); i++)
                handles_.push_back({ key, i });
}

int Editor::HitTest(Vector2 worldMouse) const
{
    // From the end (topmost objects) to the front; the star is not selectable.
    for (int i = (int)entities_.size() - 1; i >= 0; i--)
    {
        if (i < (int)handles_.size() && handles_[i].category == "star")
            continue;
        if (CheckCollisionPointCircle(worldMouse, entities_[i]->GetPosition(),
                                      entities_[i]->GetSize()))
            return i;
    }
    return -1;
}

void Editor::MoveSelected(Vector2 desiredPos)
{
    if (selected_ < 0 || selected_ >= (int)handles_.size())
        return;
    const ObjHandle& h = handles_[selected_];

    if (h.category == "planets")
    {
        // A planet is defined by its orbit: radius and angle from the system center.
        float r = sqrtf(desiredPos.x * desiredPos.x + desiredPos.y * desiredPos.y);
        float a = atan2f(desiredPos.y, desiredPos.x);
        systemJson_["planets"][h.index]["orbitRadius"] = (int)roundf(r);
        systemJson_["planets"][h.index]["angle"] = a;
    }
    else if (h.category != "star")
    {
        systemJson_[h.category][h.index]["pos"] =
            json::array({ (int)roundf(desiredPos.x), (int)roundf(desiredPos.y) });
    }

    dirty_ = true;
    RebuildEntities();
}

void Editor::Run()
{
    while (!WindowShouldClose())
    {
        screenWidth_ = GetScreenWidth();
        screenHeight_ = GetScreenHeight();
        camera_.offset = { screenWidth_ / 2.0f, screenHeight_ / 2.0f };

        HandleInput();

        BeginDrawing();
        ClearBackground(Color{ 8, 9, 14, 255 });
        DrawWorld();
        DrawHud();
        DrawPalette();
        DrawPropertyPanel();
        EndDrawing();
    }
}

void Editor::HandleInput()
{
    // Ctrl+S saving works in both modes.
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_S))
    {
        if (galaxyMode_)
            SaveUniverse();
        else
            SaveCurrentSystem();
    }

    if (galaxyMode_)
    {
        HandleGalaxyInput();
        return;
    }

    // Over the property panel and palette the world doesn't react (clicks go to the UI).
    bool overPanel = (selected_ >= 0) && CheckCollisionPointRec(GetMousePosition(), PanelRect());
    bool overPalette = CheckCollisionPointRec(GetMousePosition(), PaletteRect());
    bool overSave = CheckCollisionPointRec(GetMousePosition(), SaveButtonRect());
    bool overMode = CheckCollisionPointRec(GetMousePosition(), ModeButtonRect());
    bool overUi = overPanel || overPalette || overSave || overMode;

    // Cancel placement mode.
    if (!placeCategory_.empty() &&
        (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)))
        placeCategory_.clear();

    // Delete the selected object (not while typing into a field).
    if (IsKeyPressed(KEY_DELETE) && selected_ >= 0 && activeField_.empty())
        DeleteSelected();

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !overUi)
        camera_.zoom = Clamp(camera_.zoom * (1.0f + wheel * 0.12f), 0.01f, 2.0f);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        activeField_.clear();  // drop field focus (FieldRow restores it on a click in the field)

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUi)
    {
        Vector2 wm = GetScreenToWorld2D(GetMousePosition(), camera_);
        if (!placeCategory_.empty())
        {
            // Placement mode: place the object at the click point (we don't reset the
            // mode — several can be placed; exit with Esc / RMB).
            AddObject(placeCategory_, wm);
        }
        else
        {
            openDropdown_.clear();  // a click on the world closes an open dropdown
            int hit = HitTest(wm);
            if (hit >= 0)
            {
                selected_ = hit;
                objectGrabbed_ = true;
                objectDragging_ = false;
                Vector2 op = entities_[hit]->GetPosition();
                grabAnchor_ = { op.x - wm.x, op.y - wm.y };  // center offset from the grab point
                pressPos_ = GetMousePosition();
            }
            else
            {
                selected_ = -1;
                panning_ = true;
                dragLast_ = GetMousePosition();
            }
        }
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (objectGrabbed_)
        {
            Vector2 m = GetMousePosition();
            if (!objectDragging_ && fabsf(m.x - pressPos_.x) + fabsf(m.y - pressPos_.y) > 4.0f)
                objectDragging_ = true;  // threshold passed — start dragging
            if (objectDragging_)
            {
                Vector2 wm = GetScreenToWorld2D(m, camera_);
                MoveSelected({ wm.x + grabAnchor_.x, wm.y + grabAnchor_.y });
            }
        }
        else if (panning_)
        {
            Vector2 m = GetMousePosition();
            camera_.target.x -= (m.x - dragLast_.x) / camera_.zoom;
            camera_.target.y -= (m.y - dragLast_.y) / camera_.zoom;
            dragLast_ = m;
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        objectGrabbed_ = false;
        objectDragging_ = false;
        panning_ = false;
    }
}

void Editor::DrawWorld()
{
    if (galaxyMode_)
    {
        DrawGalaxy();
        return;
    }

    BeginMode2D(camera_);

    // Just the map boundary (we don't draw security zones/rings).
    DrawCircleLines(0, 0, World::SYSTEM_RADIUS, Fade(Ui::PANEL_BORDER, 0.5f));

    for (const auto& e : entities_)
        e->Draw();

    // Highlight the selected object.
    if (selected_ >= 0 && selected_ < (int)entities_.size())
    {
        Vector2 p = entities_[selected_]->GetPosition();
        float   r = entities_[selected_]->GetSize() + 12.0f;
        DrawCircleLines(p.x, p.y, r, Ui::ACCENT);
        DrawCircleLines(p.x, p.y, r + 4.0f, Fade(Ui::ACCENT, 0.5f));
    }

    // Ghost of the object being placed, under the cursor.
    if (!placeCategory_.empty() && !CheckCollisionPointRec(GetMousePosition(), PaletteRect()))
    {
        Vector2                 wp = GetScreenToWorld2D(GetMousePosition(), camera_);
        std::unique_ptr<Entity> ghost = MakeEntity(placeCategory_, wp);
        if (ghost)
        {
            ghost->Draw();
            DrawCircleLines(wp.x, wp.y, ghost->GetSize() + 12.0f, Fade(Ui::ACCENT, 0.7f));
        }
    }

    EndMode2D();
}

void Editor::DrawHud()
{
    Ui::Text("WORLD EDITOR", 16, 14, 26, Ui::ACCENT);

    // Info and hints in the top-left corner.
    if (galaxyMode_)
    {
        Ui::Text(TextFormat("GALAXY   systems: %d", (int)(universeJson_.contains("systems")
                                                              ? universeJson_["systems"].size()
                                                              : 0)),
                 16, 50, 16, Ui::TEXT);
        Ui::Text("drag: move  ·  click: select  ·  double-click: open system", 16, 74, 13,
                 Ui::TEXT_DIM);
    }
    else if (!universe_.systems.empty())
    {
        const WorldLoader::SystemInfo& s = universe_.systems[currentSystem_];
        Ui::Text(TextFormat("System:  %s  (%s)", s.name.c_str(), s.id.c_str()), 16, 50, 16,
                 Ui::TEXT);
        Ui::Text(TextFormat("Objects: %d", (int)entities_.size()), 16, 72, 14, Ui::TEXT_DIM);

        if (!placeCategory_.empty())
        {
            const char* title = placeCategory_.c_str();
            for (const PaletteType& t : kPalette)
                if (placeCategory_ == t.category)
                    title = t.title;
            Ui::Text(TextFormat("Placing: %s", title), 16, 96, 14, Ui::ACCENT);
            Ui::Text("click in space to place  ·  Esc / RMB to cancel", 16, 114, 13, Ui::TEXT_DIM);
        }
        else if (selected_ >= 0)
            Ui::Text("drag: move  ·  Del: delete  ·  edit on the right", 16, 96, 13, Ui::TEXT_DIM);
        else
            Ui::Text("click: select  ·  drag empty: pan  ·  wheel: zoom", 16, 96, 13, Ui::TEXT_DIM);
    }

    // Mode toggle button (System / Galaxy).
    Rectangle mb = ModeButtonRect();
    bool      overMb = CheckCollisionPointRec(GetMousePosition(), mb);
    DrawRectangleRec(mb, overMb ? Fade(Ui::ACCENT, 0.2f) : Ui::TITLE_BG);
    DrawRectangleLinesEx(mb, 1.0f, Ui::PANEL_BORDER);
    Ui::Text(galaxyMode_ ? "→ System view" : "→ Galaxy map", (int)mb.x + 10, (int)mb.y + 8, 14,
             Ui::TEXT);
    if (overMb && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        EnterGalaxyMode(!galaxyMode_);

    // Save button (top center) with an indicator of unsaved edits.
    bool      d = galaxyMode_ ? universeDirty_ : dirty_;
    Rectangle sb = SaveButtonRect();
    bool      overSb = CheckCollisionPointRec(GetMousePosition(), sb);
    DrawRectangleRec(sb, overSb ? Fade(Ui::ACCENT, 0.2f) : Ui::TITLE_BG);
    DrawRectangleLinesEx(sb, 1.0f, d ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(d ? "Save *  [Ctrl+S]" : "Saved   [Ctrl+S]", (int)sb.x + 12, (int)sb.y + 8, 14,
             d ? Ui::ACCENT : Ui::TEXT_DIM);
    if (overSb && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (galaxyMode_)
            SaveUniverse();
        else
            SaveCurrentSystem();
    }
}

Rectangle Editor::PanelRect() const
{
    return { (float)(screenWidth_ - 300), 0.0f, 300.0f, (float)screenHeight_ };
}

// The field's current value as a string (for display and buffer initialization).
static std::string FieldValueStr(const nlohmann::json& obj, const char* key, bool numeric,
                                 bool asInt)
{
    if (!obj.contains(key))
        return "";
    const nlohmann::json& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number())
        return numeric && asInt ? std::to_string((long long)llround(v.get<double>()))
                                : TextFormat("%g", v.get<double>());
    return "";
}

// A field with manual entry: click to focus, type on the keyboard. For numbers the
// value is parsed and written "live"; empty/incomplete input doesn't overwrite the old.
bool Editor::FieldRow(Rectangle r, const char* label, nlohmann::json& obj, const char* key,
                      bool numeric, bool asInt, double step)
{
    Ui::Text(label, (int)r.x, (int)r.y + 5, 13, Ui::TEXT_DIM);
    // Numbers have -/+ buttons on the right, so the box is narrower.
    float     boxW = numeric ? (r.width - 110 - 62) : (r.width - 110);
    Rectangle box{ r.x + 110, r.y, boxW, 24 };

    // Focus on click (unless covered by an open dropdown).
    if (!anyDropdownOpen_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), box))
    {
        activeField_ = key;
        editBuffer_ = FieldValueStr(obj, key, numeric, asInt);
        caretPos_ = (int)editBuffer_.size();
    }

    bool        active = (activeField_ == key);
    std::string disp = active ? editBuffer_ : FieldValueStr(obj, key, numeric, asInt);
    bool        changed = false;

    if (active)
    {
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (caretPos_ > (int)editBuffer_.size())
            caretPos_ = (int)editBuffer_.size();

        // Insert characters at the caret position.
        int c = GetCharPressed();
        while (c > 0)
        {
            bool ok = numeric ? ((c >= '0' && c <= '9') || c == '-' || (!asInt && c == '.'))
                              : (c >= 32 && c < 127);
            if (ok && editBuffer_.size() < 60)
            {
                editBuffer_.insert(editBuffer_.begin() + caretPos_, (char)c);
                caretPos_++;
            }
            c = GetCharPressed();
        }

        // Delete the word to the left of the caret.
        auto deleteWordLeft = [&]()
        {
            int i = caretPos_;
            while (i > 0 && editBuffer_[i - 1] == ' ')
                i--;
            while (i > 0 && editBuffer_[i - 1] != ' ')
                i--;
            editBuffer_.erase(editBuffer_.begin() + i, editBuffer_.begin() + caretPos_);
            caretPos_ = i;
        };

        if (IsKeyPressed(KEY_BACKSPACE))
        {
            if (ctrl)
                deleteWordLeft();
            else if (caretPos_ > 0)
            {
                editBuffer_.erase(editBuffer_.begin() + (caretPos_ - 1));
                caretPos_--;
            }
        }
        if (IsKeyPressed(KEY_DELETE) && caretPos_ < (int)editBuffer_.size())
            editBuffer_.erase(editBuffer_.begin() + caretPos_);
        if (IsKeyPressed(KEY_LEFT))
            caretPos_ = ctrl ? 0 : (caretPos_ > 0 ? caretPos_ - 1 : 0);
        if (IsKeyPressed(KEY_RIGHT))
            caretPos_ = ctrl ? (int)editBuffer_.size()
                             : (caretPos_ < (int)editBuffer_.size() ? caretPos_ + 1 : caretPos_);
        if (IsKeyPressed(KEY_HOME))
            caretPos_ = 0;
        if (IsKeyPressed(KEY_END))
            caretPos_ = (int)editBuffer_.size();
        if (IsKeyPressed(KEY_ENTER))
            activeField_.clear();

        // Write the value into the model.
        if (numeric)
        {
            try
            {
                if (asInt)
                    obj[key] = (int)std::stoll(editBuffer_);
                else
                    obj[key] = std::stod(editBuffer_);
                changed = true;
            }
            catch (...)
            {
            }  // empty / "-" / "." — don't write
        }
        else
        {
            obj[key] = editBuffer_;
            changed = true;
        }
        disp = editBuffer_;
    }

    DrawRectangleRec(box, Fade(Ui::TITLE_BG, 0.8f));
    DrawRectangleLinesEx(box, 1.0f, active ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(disp.c_str(), (int)box.x + 6, (int)box.y + 5, 14, Ui::TEXT);
    if (active)
    {
        int cp = caretPos_ < (int)disp.size() ? caretPos_ : (int)disp.size();
        int cx = (int)box.x + 6 + Ui::TextWidth(disp.substr(0, cp).c_str(), 14) + 1;
        DrawRectangle(cx, (int)box.y + 5, 2, 14, Ui::ACCENT);
    }

    // -/+ buttons for numbers (increment step). We take the current value from the
    // buffer (if the field is focused) or from the model.
    if (numeric)
    {
        double v = (obj.contains(key) && obj[key].is_number()) ? obj[key].get<double>() : 0.0;
        if (active)
            try
            {
                v = std::stod(editBuffer_);
            }
            catch (...)
            {
            }

        auto apply = [&](double nv)
        {
            if (nv < 0.0)
                nv = 0.0;
            if (asInt)
                obj[key] = (int)llround(nv);
            else
                obj[key] = nv;
            if (active)  // sync the input buffer
            {
                editBuffer_ = asInt ? std::to_string((long long)llround(nv))
                                    : std::string(TextFormat("%g", nv));
                caretPos_ = (int)editBuffer_.size();
            }
            changed = true;
        };
        // The buttons are active only if not covered by an open dropdown.
        Rectangle bm{ r.x + r.width - 58, r.y, 26, 24 };
        Rectangle bp{ r.x + r.width - 28, r.y, 26, 24 };
        if (!anyDropdownOpen_)
        {
            Button minus(bm, "-", [&]() { apply(v - step); });
            Button plus(bp, "+", [&]() { apply(v + step); });
            minus.Process();
            plus.Process();
        }
        else  // draw statically, without reacting
        {
            for (Rectangle b : { bm, bp })
            {
                DrawRectangleRec(b, Ui::TITLE_BG);
                DrawRectangleLinesEx(b, 1.0f, Ui::PANEL_BORDER);
            }
            Ui::Text("-", (int)bm.x + 11, (int)bm.y + 4, 16, Ui::TEXT_DIM);
            Ui::Text("+", (int)bp.x + 9, (int)bp.y + 4, 16, Ui::TEXT_DIM);
        }
    }
    return changed;
}

// Enumeration: a button with the current value; a click expands the list (drawn
// deferred, on top of everything else — see DrawPropertyPanel).
void Editor::DropdownRow(Rectangle r, const char* label, nlohmann::json& obj, const char* key,
                         const std::vector<std::string>& opts,
                         const std::vector<std::string>& labels)
{
    bool hasLabels = (!labels.empty() && labels.size() == opts.size());
    auto labelFor = [&](const std::string& val) -> std::string
    {
        if (hasLabels)
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i] == val)
                    return labels[i];
        return val;
    };

    std::string cur = (obj.contains(key) && obj[key].is_string())
                          ? obj[key].get<std::string>()
                          : (opts.empty() ? std::string() : opts[0]);
    Ui::Text(label, (int)r.x, (int)r.y + 5, 13, Ui::TEXT_DIM);

    Rectangle btn{ r.x + 110, r.y, r.width - 110, 24 };
    bool      isOpen = (openDropdown_ == key);
    bool      overBtn = CheckCollisionPointRec(GetMousePosition(), btn);

    // Toggle open only if no other dropdown is open (or it's this one).
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overBtn && (!anyDropdownOpen_ || isOpen))
        openDropdown_ = isOpen ? std::string() : key;

    DrawRectangleRec(btn, Fade(Ui::TITLE_BG, 0.8f));
    DrawRectangleLinesEx(btn, 1.0f, isOpen ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(labelFor(cur).c_str(), (int)btn.x + 6, (int)btn.y + 5, 14, Ui::TEXT);
    Ui::Text("v", (int)(btn.x + btn.width) - 16, (int)btn.y + 5, 14, Ui::TEXT_DIM);

    // Remember the open dropdown for deferred rendering on top of everything else.
    if (openDropdown_ == key)
    {
        dropObj_ = &obj;
        dropKey_ = key;
        dropOpts_ = opts;
        dropLabels_ = hasLabels ? labels : std::vector<std::string>{};
        dropAnchor_ = btn;
    }
}

// Property panel for the selected object: the set of fields depends on its category.
void Editor::DrawPropertyPanel()
{
    if (galaxyMode_)
    {
        DrawGalaxyPanel();
        return;
    }
    if (selected_ < 0 || selected_ >= (int)handles_.size())
        return;

    anyDropdownOpen_ = !openDropdown_.empty();
    dropObj_ = nullptr;  // filled in if an open dropdown is encountered among the rows

    Rectangle panel = PanelRect();
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawLineEx({ panel.x, 0.0f }, { panel.x, (float)screenHeight_ }, 1.0f, Ui::PANEL_BORDER);

    const ObjHandle& h = handles_[selected_];
    nlohmann::json&  obj =
        (h.category == "star") ? systemJson_["star"] : systemJson_[h.category][h.index];

    int x = (int)panel.x + 14;
    int y = 16;
    Ui::Text("PROPERTIES", x, y, 18, Ui::ACCENT);
    y += 26;
    Ui::Text(h.category.c_str(), x, y, 14, Ui::TEXT_DIM);
    y += 26;

    float w = panel.width - 28.0f;
    bool  changed = false;
    auto  row = [&](float rh) -> Rectangle
    {
        Rectangle r{ (float)x, (float)y, w, rh };
        y += (int)rh + 8;
        return r;
    };

    if (h.category == "star")
    {
        DropdownRow(row(24), "type", obj, "type", { "Yellow", "Red", "Blue" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
    }
    else if (h.category == "planets")
    {
        DropdownRow(row(24), "type", obj, "type", { "Rocky", "Gas", "Ice", "Lava", "Oceanic" });
        DropdownRow(row(24), "deposit", obj, "deposit", { "Iron", "Ice", "Crystal" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        changed |= FieldRow(row(24), "orbitSpeed", obj, "orbitSpeed", true, true, 20);
        changed |= FieldRow(row(24), "orbitRadius", obj, "orbitRadius", true, true, 200);
    }
    else if (h.category == "stations")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        // The faction list comes from the registry (lawful, no pirates): id as the
        // value, name as the label.
        std::vector<std::string> facIds, facNames;
        for (int fi = 0; fi < Factions::Count(); fi++)
        {
            FactionId f = (FactionId)fi;
            if (!Factions::IsLawful(f))
                continue;
            facIds.push_back(Factions::Id(f));
            facNames.push_back(FactionName(f));
        }
        DropdownRow(row(24), "faction", obj, "faction", facIds, facNames);
        DropdownRow(row(24), "role", obj, "role",
                    { "TradeHub", "MiningOutpost", "Shipyard", "Military" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
    }
    else if (h.category == "asteroidFields")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        DropdownRow(row(24), "resource", obj, "resource", { "Iron", "Ice", "Crystal" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        changed |= FieldRow(row(24), "ore", obj, "ore", true, true, 20);
    }
    else if (h.category == "nebulae")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        changed |= FieldRow(row(24), "radius", obj, "radius", true, true, 200);
    }
    else if (h.category == "derelicts")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        changed |= FieldRow(row(24), "reward", obj, "reward", true, true, 100);
    }
    else if (h.category == "gates")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        std::vector<std::string> ids, names;
        for (const auto& s : universe_.systems)
        {
            ids.push_back(s.id);
            names.push_back(s.name);
        }
        DropdownRow(row(24), "dest", obj, "destination", ids, names);
    }

    // Deferred rendering of the open dropdown on top of everything + selection handling.
    if (dropObj_ != nullptr)
    {
        float     ih = 24.0f;
        Rectangle list{ dropAnchor_.x, dropAnchor_.y + dropAnchor_.height, dropAnchor_.width,
                        ih * dropOpts_.size() };
        DrawRectangleRec(list, Ui::PANEL_BG);
        DrawRectangleLinesEx(list, 1.0f, Ui::ACCENT);

        Vector2 m = GetMousePosition();
        for (size_t i = 0; i < dropOpts_.size(); i++)
        {
            Rectangle ir{ list.x, list.y + ih * i, list.width, ih };
            bool      hov = CheckCollisionPointRec(m, ir);
            if (hov)
                DrawRectangleRec(ir, Fade(Ui::ACCENT, 0.2f));
            const std::string& shown =
                (dropLabels_.size() == dropOpts_.size()) ? dropLabels_[i] : dropOpts_[i];
            Ui::Text(shown.c_str(), (int)ir.x + 6, (int)ir.y + 5, 14, hov ? Ui::ACCENT : Ui::TEXT);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                (*dropObj_)[dropKey_] = dropOpts_[i];
                openDropdown_.clear();
                changed = true;
            }
        }
        // A click outside the list and the button closes it.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(m, list) &&
            !CheckCollisionPointRec(m, dropAnchor_))
            openDropdown_.clear();
    }

    if (changed)
    {
        dirty_ = true;
        RebuildEntities();
    }

    // Delete button (the star can't be deleted). The deletion itself happens after
    // the panel, so we don't touch obj once the array has changed.
    if (h.category != "star" && !anyDropdownOpen_)
    {
        Button del(Rectangle{ panel.x + 14, (float)(screenHeight_ - 44), panel.width - 28, 30 },
                   "Delete object  [Del]", [this]() { deleteRequested_ = true; });
        del.Process();
    }
    if (deleteRequested_)
    {
        deleteRequested_ = false;
        DeleteSelected();
    }
}

// The palette's label button at the bottom-left.
static Rectangle PaletteButtonRect(int screenHeight)
{
    return { 6.0f, (float)screenHeight - kPaletteBtnH - 8.0f, kPaletteW, kPaletteBtnH };
}

Rectangle Editor::PaletteRect() const
{
    Rectangle btn = PaletteButtonRect(screenHeight_);
    if (!paletteOpen_)
        return btn;
    float listH = kPaletteN * kPaletteRowH;
    float top = btn.y - 6.0f - listH;
    return { btn.x, top, kPaletteW, (btn.y + btn.height) - top };
}

// Object preview: draw the real entity in a small box with the scale fitted.
void Editor::DrawEntityPreview(Rectangle box, const std::string& category)
{
    std::unique_ptr<Entity> e = MakeEntity(category, { 0.0f, 0.0f });
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
    e->Draw();
    EndMode2D();
    EndScissorMode();
}

// Creation palette at the bottom-left: the CREATE label button expands the list
// upward. Each card is a preview, a name, a description. A click enters placement mode.
void Editor::DrawPalette()
{
    if (galaxyMode_)
        return;  // in galaxy mode there's no object palette

    Vector2   m = GetMousePosition();
    Rectangle btn = PaletteButtonRect(screenHeight_);

    if (paletteOpen_)
    {
        float     listH = kPaletteN * kPaletteRowH;
        Rectangle list{ btn.x, btn.y - 6.0f - listH, kPaletteW, listH };
        DrawRectangleRec(list, Ui::PANEL_BG);
        DrawRectangleLinesEx(list, 1.0f, Ui::PANEL_BORDER);

        for (int i = 0; i < kPaletteN; i++)
        {
            const PaletteType& t = kPalette[i];
            Rectangle          row{ list.x + 6, list.y + 4 + i * kPaletteRowH, list.width - 12,
                                    kPaletteRowH - 8 };
            bool               active = (placeCategory_ == t.category);
            bool               hover = CheckCollisionPointRec(m, row);
            DrawRectangleRec(row,
                             active ? Fade(Ui::ACCENT, 0.22f)
                                    : (hover ? Fade(Ui::ACCENT, 0.10f) : Fade(Ui::TITLE_BG, 0.6f)));
            DrawRectangleLinesEx(row, 1.0f, active ? Ui::ACCENT : Ui::PANEL_BORDER);

            Rectangle icon{ row.x + 4, row.y + 4, 42, row.height - 8 };
            DrawRectangleRec(icon, Fade(BLACK, 0.4f));
            DrawEntityPreview(icon, t.category);

            Ui::Text(t.title, (int)icon.x + 52, (int)row.y + 7, 16, active ? Ui::ACCENT : Ui::TEXT);
            Ui::Text(t.desc, (int)icon.x + 52, (int)row.y + 26, 12, Ui::TEXT_DIM);

            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                placeCategory_ = active ? std::string() : t.category;  // repeat click cancels
        }
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
            placeCategory_.clear();  // collapsed — cancel placement mode
    }
}

// Creates an entity of the category with default values at position pos
// (for previews pos = {0,0}). Used for the ghost and the preview.
std::unique_ptr<Entity> Editor::MakeEntity(const std::string& category, Vector2 pos) const
{
    if (category == "planets")
    {
        float r = sqrtf(pos.x * pos.x + pos.y * pos.y);
        return std::make_unique<Planet>(r, 300.0f, atan2f(pos.y, pos.x), 120.0f,
                                        PlanetTypeColor(PlanetType::Rocky), ResourceType::Iron,
                                        PlanetType::Rocky);
    }
    if (category == "stations")
        return std::make_unique<Station>(pos, 80.0f, "New Station", FactionId::Independent,
                                         StationRole::TradeHub);
    if (category == "asteroidFields")
        return std::make_unique<AsteroidField>(pos, 400.0f, "New Belt", ResourceType::Iron, 200);
    if (category == "nebulae")
        return std::make_unique<Nebula>(pos, 2000.0f, "New Nebula");
    if (category == "derelicts")
        return std::make_unique<Derelict>(pos, 30.0f, "New Derelict", 500.0);
    if (category == "gates")
        return std::make_unique<JumpGate>(pos, 150.0f, "New Gate", "");
    return nullptr;
}

// Adds an object of the category at position pos with default values and selects it.
void Editor::AddObject(const std::string& category, Vector2 pos)
{
    int  px = (int)roundf(pos.x), py = (int)roundf(pos.y);
    json o = json::object();

    if (category == "planets")
    {
        float r = sqrtf(pos.x * pos.x + pos.y * pos.y);
        if (r < 500.0f)
            r = 4000.0f;
        o = { { "orbitRadius", (int)roundf(r) },
              { "orbitSpeed", 300 },
              { "angle", atan2f(pos.y, pos.x) },
              { "size", 120 },
              { "type", "Rocky" },
              { "deposit", "Iron" } };
    }
    else if (category == "stations")
        o = { { "name", "New Station" },
              { "pos", { px, py } },
              { "size", 80 },
              { "faction", "Independent" },
              { "role", "TradeHub" } };
    else if (category == "asteroidFields")
        o = { { "name", "New Belt" },
              { "pos", { px, py } },
              { "size", 400 },
              { "resource", "Iron" },
              { "ore", 200 } };
    else if (category == "nebulae")
        o = { { "name", "New Nebula" }, { "pos", { px, py } }, { "radius", 2000 } };
    else if (category == "derelicts")
        o = {
            { "name", "New Derelict" }, { "pos", { px, py } }, { "size", 30 }, { "reward", 500 }
        };
    else if (category == "gates")
        o = { { "name", "New Gate" },
              { "pos", { px, py } },
              { "size", 150 },
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
Rectangle Editor::SaveButtonRect() const
{
    return { screenWidth_ / 2.0f + 11.0f, 12.0f, 140.0f, 30.0f };  // right one in the group
}

// Writes the current system back to its source JSON file (with indentation).
void Editor::SaveCurrentSystem()
{
    if (universe_.systems.empty())
        return;
    std::string   path = dataDir_ + "systems/" + universe_.systems[currentSystem_].file;
    std::ofstream out(path);
    if (!out.is_open())
    {
        TraceLog(LOG_WARNING, "Editor: failed to write %s", path.c_str());
        return;
    }
    out << systemJson_.dump(4) << "\n";
    dirty_ = false;
    TraceLog(LOG_INFO, "Editor: saved %s", path.c_str());
}

// ===== Galaxy mode =========================================================

Rectangle Editor::ModeButtonRect() const
{
    return { screenWidth_ / 2.0f - 151.0f, 12.0f, 150.0f, 30.0f };  // left one in the group
}

void Editor::EnterGalaxyMode(bool on)
{
    galaxyMode_ = on;
    selected_ = -1;
    gSelected_ = -1;
    gLinksOwner_.clear();
    placeCategory_.clear();
    activeField_.clear();
    openDropdown_.clear();
    panning_ = false;
    gGrabbed_ = false;

    if (on)
    {
        camera_.target = { 0.0f, 0.0f };
        camera_.zoom = 1.5f;  // map coordinates are on the order of hundreds
    }
    else
    {
        RefreshUniverseStruct();
        camera_.target = { 0.0f, 0.0f };
        camera_.zoom = 0.03f;
        if (currentSystem_ >= (int)universe_.systems.size())
            currentSystem_ = 0;
        if (!universe_.systems.empty())
            LoadSystemAt(currentSystem_);
    }
}

// A system's map position from the JSON.
static Vector2 MapPos(const nlohmann::json& sys)
{
    if (sys.contains("map") && sys["map"].is_array() && sys["map"].size() >= 2)
        return { (float)sys["map"][0].get<double>(), (float)sys["map"][1].get<double>() };
    return { 0.0f, 0.0f };
}

bool Editor::HasLink(const std::string& a, const std::string& b) const
{
    if (!universeJson_.contains("links"))
        return false;
    for (const auto& l : universeJson_["links"])
        if (l.is_array() && l.size() >= 2)
        {
            std::string la = l[0], lb = l[1];
            if ((la == a && lb == b) || (la == b && lb == a))
                return true;
        }
    return false;
}

void Editor::ToggleLink(const std::string& a, const std::string& b)
{
    if (!universeJson_.contains("links") || !universeJson_["links"].is_array())
        universeJson_["links"] = json::array();
    json& links = universeJson_["links"];
    for (int i = 0; i < (int)links.size(); i++)
    {
        if (!links[i].is_array() || links[i].size() < 2)
            continue;
        std::string la = links[i][0], lb = links[i][1];
        if ((la == a && lb == b) || (la == b && lb == a))
        {
            links.erase(i);  // link existed — remove it
            return;
        }
    }
    links.push_back(json::array({ a, b }));  // didn't exist — add it
}

void Editor::HandleGalaxyInput()
{
    Vector2 m = GetMousePosition();
    bool    overPanel = CheckCollisionPointRec(m, PanelRect());
    bool    overSave = CheckCollisionPointRec(m, SaveButtonRect());
    bool    overMode = CheckCollisionPointRec(m, ModeButtonRect());
    bool    overUi = overPanel || overSave || overMode;

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !overUi)
        camera_.zoom = Clamp(camera_.zoom * (1.0f + wheel * 0.12f), 0.2f, 6.0f);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        activeField_.clear();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUi && universeJson_.contains("systems"))
    {
        Vector2     wm = GetScreenToWorld2D(m, camera_);
        int         hit = -1;
        float       pick = 14.0f / camera_.zoom;
        const json& sys = universeJson_["systems"];
        for (int i = 0; i < (int)sys.size(); i++)
            if (CheckCollisionPointCircle(wm, MapPos(sys[i]), pick))
                hit = i;

        if (hit >= 0)
        {
            // Double-click a node — open the system in edit mode.
            double t = GetTime();
            if (hit == gLastClickNode_ && (t - gLastClickTime_) < 0.35)
            {
                OpenSystem(hit);
                return;
            }
            gLastClickTime_ = t;
            gLastClickNode_ = hit;

            gSelected_ = hit;
            gGrabbed_ = true;
            gDragging_ = false;
            pressPos_ = m;
            Vector2 p = MapPos(sys[hit]);
            gGrabAnchor_ = { p.x - wm.x, p.y - wm.y };
        }
        else
        {
            gSelected_ = -1;
            panning_ = true;
            dragLast_ = m;
        }
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (gGrabbed_ && gSelected_ >= 0)
        {
            // Movement starts only after the cursor actually moves — the first
            // click just selects the system (and doesn't mark it "unsaved").
            if (!gDragging_ && fabsf(m.x - pressPos_.x) + fabsf(m.y - pressPos_.y) > 4.0f)
                gDragging_ = true;
            if (gDragging_)
            {
                Vector2 wm = GetScreenToWorld2D(m, camera_);
                universeJson_["systems"][gSelected_]["map"] = json::array(
                    { (int)roundf(wm.x + gGrabAnchor_.x), (int)roundf(wm.y + gGrabAnchor_.y) });
                universeDirty_ = true;
            }
        }
        else if (panning_)
        {
            camera_.target.x -= (m.x - dragLast_.x) / camera_.zoom;
            camera_.target.y -= (m.y - dragLast_.y) / camera_.zoom;
            dragLast_ = m;
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        gGrabbed_ = false;
        gDragging_ = false;
        panning_ = false;
    }
}

void Editor::DrawGalaxy()
{
    if (!universeJson_.contains("systems"))
        return;
    const json& sys = universeJson_["systems"];

    BeginMode2D(camera_);

    // Links.
    if (universeJson_.contains("links"))
        for (const auto& l : universeJson_["links"])
        {
            if (!l.is_array() || l.size() < 2)
                continue;
            std::string a = l[0], b = l[1];
            Vector2     pa{ 0, 0 }, pb{ 0, 0 };
            bool        fa = false, fb = false;
            for (const auto& s : sys)
            {
                if (s.value("id", std::string()) == a)
                {
                    pa = MapPos(s);
                    fa = true;
                }
                if (s.value("id", std::string()) == b)
                {
                    pb = MapPos(s);
                    fb = true;
                }
            }
            if (fa && fb)
                DrawLineEx(pa, pb, 1.5f / camera_.zoom, Fade(Ui::PANEL_BORDER, 0.9f));
        }

    // Nodes.
    float r = 8.0f / camera_.zoom;
    for (int i = 0; i < (int)sys.size(); i++)
    {
        Vector2 p = MapPos(sys[i]);
        bool    cur = (i == gSelected_);
        DrawCircleV(p, r, cur ? Ui::ACCENT : Ui::TEXT_DIM);
        if (cur)
            DrawCircleLines((int)p.x, (int)p.y, r + 6.0f / camera_.zoom, Ui::ACCENT);
    }
    EndMode2D();

    // System labels — in screen coordinates (constant size).
    for (const auto& s : sys)
    {
        Vector2 sp = GetWorldToScreen2D(MapPos(s), camera_);
        Ui::Text(s.value("name", std::string("?")).c_str(), (int)sp.x + 12, (int)sp.y - 8, 14,
                 Ui::TEXT);
    }
}

void Editor::DrawGalaxyPanel()
{
    anyDropdownOpen_ = !openDropdown_.empty();
    dropObj_ = nullptr;

    Rectangle panel = PanelRect();
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawLineEx({ panel.x, 0.0f }, { panel.x, (float)screenHeight_ }, 1.0f, Ui::PANEL_BORDER);

    int   x = (int)panel.x + 14;
    float w = panel.width - 28.0f;
    int   y = 16;
    Ui::Text("GALAXY", x, y, 18, Ui::ACCENT);
    y += 30;

    Button add(Rectangle{ panel.x + 14, (float)y, w, 28 }, "+ Add system",
               [this]() { AddSystem(); });
    add.Process();
    y += 40;

    if (gSelected_ < 0 || !universeJson_.contains("systems") ||
        gSelected_ >= (int)universeJson_["systems"].size())
    {
        Ui::Text("Select a system node", x, y, 14, Ui::TEXT_DIM);
        return;
    }

    json& obj = universeJson_["systems"][gSelected_];
    bool  changed = false;
    auto  row = [&](float rh) -> Rectangle
    {
        Rectangle r{ (float)x, (float)y, w, rh };
        y += (int)rh + 8;
        return r;
    };

    // Open the system in object-editing mode.
    if (!anyDropdownOpen_)
    {
        int    sel = gSelected_;
        Button open(Rectangle{ (float)x, (float)y, w, 26.0f }, "Open in system view  (dbl-click)",
                    [this, sel]() { OpenSystem(sel); });
        open.Process();
    }
    y += 34;

    changed |= FieldRow(row(24), "name", obj, "name", false, false);

    // The id drives the file name. We remember the old values on focus, and when
    // editing ends we rename the file on disk and update references to the old id.
    bool idActive = (activeField_ == "id");
    if (idActive && !gIdEditing_)
    {
        gIdEditing_ = true;
        gOldId_ = obj.value("id", std::string());
        gOldFile_ = obj.value("file", std::string());
    }
    if (FieldRow(row(24), "id", obj, "id", false, false))
    {
        obj["file"] = obj.value("id", std::string()) + ".json";  // auto file name
        changed = true;
    }
    if (!idActive && gIdEditing_)
    {
        gIdEditing_ = false;
        std::string newId = obj.value("id", std::string());
        std::string newFile = obj.value("file", std::string());

        // Rename the system's file on disk (no orphaned copies).
        if (newFile != gOldFile_ && !gOldFile_.empty())
        {
            std::string op = dataDir_ + "systems/" + gOldFile_;
            std::string np = dataDir_ + "systems/" + newFile;
            if (FileExists(op.c_str()) && !FileExists(np.c_str()))
                std::rename(op.c_str(), np.c_str());
        }
        // Update references to the old id (links and the start system).
        if (newId != gOldId_ && !gOldId_.empty())
        {
            if (universeJson_.contains("links"))
                for (auto& l : universeJson_["links"])
                    for (auto& e : l)
                        if (e == gOldId_)
                            e = newId;
            if (universeJson_.value("start", std::string()) == gOldId_)
                universeJson_["start"] = newId;
            gLinksOwner_.clear();
        }
    }

    // The file name is read-only (managed through the id).
    Ui::Text("file", x, y + 5, 13, Ui::TEXT_DIM);
    Ui::Text(obj.value("file", std::string()).c_str(), x + 110, y + 5, 14, Ui::TEXT_DIM);
    y += 32;

    // Security level 0..1 (affects pirate spawning in the game).
    changed |= FieldRow(row(24), "security", obj, "security", true, false, 0.1);

    Vector2 mp = MapPos(obj);
    Ui::Text(TextFormat("map: %d, %d", (int)mp.x, (int)mp.y), x, y, 13, Ui::TEXT_DIM);
    y += 26;

    Ui::Text("LINKS", x, y, 14, Ui::TEXT_DIM);
    y += 22;

    std::string selfId = obj.value("id", std::string());
    const json& sys = universeJson_["systems"];

    // We rebuild the selected system's link list when the system changes; we always
    // show at least one (empty) row.
    if (gLinksOwner_ != selfId)
    {
        gLinksJson_ = json::object();
        int k = 0;
        if (universeJson_.contains("links"))
            for (const auto& l : universeJson_["links"])
                if (l.is_array() && l.size() >= 2)
                {
                    std::string la = l[0], lb = l[1];
                    if (la == selfId)
                        gLinksJson_[std::to_string(k++)] = lb;
                    else if (lb == selfId)
                        gLinksJson_[std::to_string(k++)] = la;
                }
        if (k == 0)
            gLinksJson_["0"] = "";
        gLinksOwner_ = selfId;
    }

    // The current value of a link row by index.
    auto curVal = [&](int kk) -> std::string
    {
        std::string key = std::to_string(kk);
        return (gLinksJson_.contains(key) && gLinksJson_[key].is_string())
                   ? gLinksJson_[key].get<std::string>()
                   : std::string();
    };

    int  count = (int)gLinksJson_.size();
    int  removeIdx = -1;
    bool linksChanged = false;  // touch the global links only on a real edit
    for (int k = 0; k < count; k++)
    {
        std::string key = std::to_string(k);
        if (!gLinksJson_.contains(key))
            continue;

        // Options: other systems not yet used by OTHER link rows
        // (one system can't be picked in several links).
        std::vector<std::string> ids, names;
        for (const auto& s : sys)
        {
            std::string oid = s.value("id", std::string());
            if (oid == selfId)
                continue;
            bool used = false;
            for (int j = 0; j < count; j++)
                if (j != k && curVal(j) == oid)
                    used = true;
            if (used)
                continue;
            ids.push_back(oid);
            names.push_back(s.value("name", oid));
        }

        DropdownRow(Rectangle{ (float)x, (float)y, w - 30.0f, 24.0f }, "", gLinksJson_, key.c_str(),
                    ids, names);
        Rectangle rb{ (float)x + w - 26.0f, (float)y, 24.0f, 24.0f };
        if (!anyDropdownOpen_)
        {
            Button rm(rb, "x", [&removeIdx, k]() { removeIdx = k; });
            rm.Process();
        }
        else
        {
            DrawRectangleRec(rb, Ui::TITLE_BG);
            DrawRectangleLinesEx(rb, 1.0f, Ui::PANEL_BORDER);
            Ui::Text("x", (int)rb.x + 8, (int)rb.y + 4, 14, Ui::TEXT_DIM);
        }
        y += 28;
    }

    if (!anyDropdownOpen_)
    {
        Button addl(Rectangle{ (float)x, (float)y, w, 24.0f }, "+ link", [this]()
                    { gLinksJson_[std::to_string((int)gLinksJson_.size())] = std::string(); });
        addl.Process();
    }
    y += 30;

    // Deferred rendering of the open dropdown (for the link/enum selections above).
    if (dropObj_ != nullptr)
    {
        float     ih = 24.0f;
        Rectangle list{ dropAnchor_.x, dropAnchor_.y + dropAnchor_.height, dropAnchor_.width,
                        ih * dropOpts_.size() };
        DrawRectangleRec(list, Ui::PANEL_BG);
        DrawRectangleLinesEx(list, 1.0f, Ui::ACCENT);
        Vector2 mm = GetMousePosition();
        for (size_t i = 0; i < dropOpts_.size(); i++)
        {
            Rectangle ir{ list.x, list.y + ih * i, list.width, ih };
            bool      hov = CheckCollisionPointRec(mm, ir);
            if (hov)
                DrawRectangleRec(ir, Fade(Ui::ACCENT, 0.2f));
            const std::string& shown =
                (dropLabels_.size() == dropOpts_.size()) ? dropLabels_[i] : dropOpts_[i];
            Ui::Text(shown.c_str(), (int)ir.x + 6, (int)ir.y + 5, 14, hov ? Ui::ACCENT : Ui::TEXT);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                (*dropObj_)[dropKey_] = dropOpts_[i];
                openDropdown_.clear();
                linksChanged = true;
            }
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mm, list) &&
            !CheckCollisionPointRec(mm, dropAnchor_))
            openDropdown_.clear();
    }

    // Deleting a link row (with re-indexing of keys).
    if (removeIdx >= 0)
    {
        json rebuilt = json::object();
        int  nk = 0;
        for (int k = 0; k < count; k++)
            if (k != removeIdx)
            {
                std::string key = std::to_string(k);
                if (gLinksJson_.contains(key))
                    rebuilt[std::to_string(nk++)] = gLinksJson_[key];
            }
        gLinksJson_ = rebuilt;
        linksChanged = true;
    }

    // We rebuild the global links ONLY on a real edit (otherwise merely selecting a
    // system would normalize the order and falsely mark it "unsaved").
    if (linksChanged)
    {
        std::vector<std::string> partners;
        for (int k = 0; k < (int)gLinksJson_.size(); k++)
        {
            std::string key = std::to_string(k);
            if (!gLinksJson_.contains(key) || !gLinksJson_[key].is_string())
                continue;
            std::string p = gLinksJson_[key];
            if (p.empty() || p == selfId)
                continue;
            bool dup = false;
            for (const std::string& q : partners)
                if (q == p)
                    dup = true;
            if (!dup)
                partners.push_back(p);
        }
        json oldLinks = universeJson_.contains("links") ? universeJson_["links"] : json::array();

        // This system's old partners (before the edit) — for diffing the gates.
        std::vector<std::string> oldPartners;
        for (const auto& l : oldLinks)
            if (l.is_array() && l.size() >= 2)
            {
                std::string la = l[0], lb = l[1];
                if (la == selfId)
                    oldPartners.push_back(lb);
                else if (lb == selfId)
                    oldPartners.push_back(la);
            }
        auto has = [](const std::vector<std::string>& v, const std::string& s)
        {
            for (const std::string& q : v)
                if (q == s)
                    return true;
            return false;
        };

        // Added links → gates in both systems; removed links → remove the gates.
        for (const std::string& p : partners)
            if (!has(oldPartners, p))
            {
                SyncGate(selfId, p, true);
                SyncGate(p, selfId, true);
            }
        for (const std::string& p : oldPartners)
            if (!has(partners, p))
            {
                SyncGate(selfId, p, false);
                SyncGate(p, selfId, false);
            }

        json newLinks = json::array();
        for (const auto& l : oldLinks)
            if (l.is_array() && l.size() >= 2 && l[0] != selfId && l[1] != selfId)
                newLinks.push_back(l);
        for (const std::string& p : partners)
            newLinks.push_back(json::array({ selfId, p }));

        if (newLinks != oldLinks)
        {
            universeJson_["links"] = newLinks;
            universeDirty_ = true;
        }
    }

    if (changed)
        universeDirty_ = true;

    // Deleting the system (deferred).
    if (!anyDropdownOpen_)
    {
        Button del(Rectangle{ panel.x + 14, (float)(screenHeight_ - 44), w, 30 }, "Delete system",
                   [this]() { deleteRequested_ = true; });
        del.Process();
    }
    if (deleteRequested_)
    {
        deleteRequested_ = false;
        DeleteSystem(gSelected_);
    }
}

void Editor::AddSystem()
{
    if (!universeJson_.contains("systems") || !universeJson_["systems"].is_array())
        universeJson_["systems"] = json::array();

    // Unique id.
    auto idExists = [&](const std::string& id)
    {
        for (const auto& s : universeJson_["systems"])
            if (s.value("id", std::string()) == id)
                return true;
        return false;
    };
    std::string id;
    for (int n = 1;; n++)
    {
        id = "sys" + std::to_string(n);
        if (!idExists(id))
            break;
    }

    json s = { { "id", id },
               { "name", "New System" },
               { "file", id + ".json" },
               { "map", { (int)roundf(camera_.target.x), (int)roundf(camera_.target.y) } } };
    universeJson_["systems"].push_back(s);

    // Create a minimal system file if it doesn't exist yet.
    std::string path = dataDir_ + "systems/" + id + ".json";
    if (!FileExists(path.c_str()))
    {
        std::ofstream o(path);
        if (o.is_open())
        {
            json def = { { "star", { { "type", "Yellow" }, { "size", 500 } } } };
            o << def.dump(4) << "\n";
        }
    }

    universeDirty_ = true;
    gSelected_ = (int)universeJson_["systems"].size() - 1;
}

void Editor::DeleteSystem(int index)
{
    if (!universeJson_.contains("systems") || index < 0 ||
        index >= (int)universeJson_["systems"].size())
        return;
    std::string id = universeJson_["systems"][index].value("id", std::string());

    // Partners of the system being deleted — remove the gates leading to it from them.
    std::vector<std::string> partners;
    if (universeJson_.contains("links"))
        for (const auto& l : universeJson_["links"])
            if (l.is_array() && l.size() >= 2)
            {
                std::string la = l[0], lb = l[1];
                if (la == id)
                    partners.push_back(lb);
                else if (lb == id)
                    partners.push_back(la);
            }
    for (const std::string& p : partners)
        SyncGate(p, id, false);  // remove the partner's gate to the deleted system

    // Remove links involving this system.
    if (universeJson_.contains("links") && universeJson_["links"].is_array())
    {
        json kept = json::array();
        for (const auto& l : universeJson_["links"])
            if (!(l.is_array() && l.size() >= 2 && (l[0] == id || l[1] == id)))
                kept.push_back(l);
        universeJson_["links"] = kept;
    }

    universeJson_["systems"].erase(index);
    gSelected_ = -1;
    gLinksOwner_.clear();
    universeDirty_ = true;
}

void Editor::RefreshUniverseStruct()
{
    universe_.systems.clear();
    universe_.links.clear();
    universe_.startId = universeJson_.value("start", std::string());

    if (universeJson_.contains("systems"))
        for (const auto& s : universeJson_["systems"])
        {
            WorldLoader::SystemInfo info;
            info.id = s.value("id", std::string());
            info.name = s.value("name", info.id);
            info.file = s.value("file", info.id + ".json");
            info.mapPos = MapPos(s);
            universe_.systems.push_back(info);
        }
    if (universeJson_.contains("links"))
        for (const auto& l : universeJson_["links"])
            if (l.is_array() && l.size() >= 2)
                universe_.links.push_back(WorldLoader::SystemLink{ l[0], l[1] });

    if (universe_.startId.empty() && !universe_.systems.empty())
        universe_.startId = universe_.systems.front().id;
}

void Editor::SaveUniverse()
{
    std::ofstream out(dataDir_ + "universe.json");
    if (!out.is_open())
    {
        TraceLog(LOG_WARNING, "Editor: failed to write universe.json");
        return;
    }
    out << universeJson_.dump(4) << "\n";
    universeDirty_ = false;
    RefreshUniverseStruct();
    TraceLog(LOG_INFO, "Editor: saved universe.json");
}

// Switch from galaxy mode into the selected system (by index in universeJson_).
void Editor::OpenSystem(int index)
{
    RefreshUniverseStruct();
    if (index < 0 || index >= (int)universe_.systems.size())
        return;
    currentSystem_ = index;
    EnterGalaxyMode(false);  // switches to system mode and loads currentSystem_
}

// Creates/removes in system sysId's file a gate leading to destId. The gate is
// positioned toward destId on the galaxy map, at the edge of the system. The file
// is read and rewritten in place (the "gate" change is applied immediately).
void Editor::SyncGate(const std::string& sysId, const std::string& destId, bool add)
{
    const json* sysEntry = nullptr;
    const json* destEntry = nullptr;
    if (universeJson_.contains("systems"))
        for (const auto& s : universeJson_["systems"])
        {
            if (s.value("id", std::string()) == sysId)
                sysEntry = &s;
            if (s.value("id", std::string()) == destId)
                destEntry = &s;
        }
    if (sysEntry == nullptr)
        return;

    std::string file = sysEntry->value("file", sysId + ".json");
    std::string path = dataDir_ + "systems/" + file;

    json data = json::object();
    {
        std::ifstream f(path);
        if (f.is_open())
        {
            data = json::parse(f, nullptr, false);
            if (data.is_discarded())
                data = json::object();
        }
    }
    if (!data.contains("gates") || !data["gates"].is_array())
        data["gates"] = json::array();

    if (add)
    {
        for (const auto& g : data["gates"])  // already there — do nothing
            if (g.value("destination", std::string()) == destId)
                return;

        Vector2 sp = MapPos(*sysEntry);
        Vector2 dp = (destEntry != nullptr) ? MapPos(*destEntry) : Vector2{ sp.x + 1.0f, sp.y };
        float   dx = dp.x - sp.x, dy = dp.y - sp.y;
        float   len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f)
        {
            dx = 1.0f;
            dy = 0.0f;
            len = 1.0f;
        }
        float       reach = World::SYSTEM_RADIUS * 0.85f;
        int         gx = (int)roundf(dx / len * reach);
        int         gy = (int)roundf(dy / len * reach);
        std::string destName = (destEntry != nullptr) ? destEntry->value("name", destId) : destId;
        data["gates"].push_back(json{ { "name", "Gate to " + destName },
                                      { "pos", { gx, gy } },
                                      { "size", 150 },
                                      { "destination", destId } });
    }
    else
    {
        json kept = json::array();
        for (const auto& g : data["gates"])
            if (g.value("destination", std::string()) != destId)
                kept.push_back(g);
        data["gates"] = kept;
    }

    std::ofstream o(path);
    if (o.is_open())
        o << data.dump(4) << "\n";
}
