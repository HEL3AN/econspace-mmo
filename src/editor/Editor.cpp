// The editor's spine: the window, the input loop, and the system view.
//
// The rest of it lives in sibling translation units of the same class (#17):
// Editor_Palette (creating objects), Editor_Panel (editing the selected one),
// Editor_Galaxy (the map view) and Editor_Universe (mutating universe.json).

#include "Editor.h"

#include "core/World.h"
#include "render/Textures.h"
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

#include <fstream>

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
        if (IsKeyPressed(KEY_F2))  // shapes ↔ glyphs, the same key as in the game
            backend_ = (backend_ == &shapeBackend_) ? (Render::IBackend*)&glyphBackend_
                                                    : (Render::IBackend*)&shapeBackend_;
        if (IsKeyPressed(KEY_F3))  // the gallery, from wherever you are
            EnterGalleryMode(mode_ != Mode::Gallery);

        BeginDrawing();
        ClearBackground(Color{ 8, 9, 14, 255 });
        if (mode_ == Mode::Gallery)
        {
            DrawGallery();
            DrawHud();
            DrawGalleryPanel();
        }
        else
        {
            DrawWorld();
            DrawHud();
            DrawPalette();
            DrawPropertyPanel();
        }
        EndDrawing();
    }
}

void Editor::HandleInput()
{
    // Ctrl+S saves whatever the current view edits.
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_S))
    {
        if (mode_ == Mode::Galaxy)
            SaveUniverse();
        else if (mode_ == Mode::Gallery)
            SaveArchetypes();
        else
            SaveCurrentSystem();
    }

    if (mode_ == Mode::Galaxy)
    {
        HandleGalaxyInput();
        return;
    }
    if (mode_ == Mode::Gallery)
    {
        HandleGalleryInput();
        return;
    }

    // Over the property panel and palette the world doesn't react (clicks go to the UI).
    bool overPanel = (selected_ >= 0) && CheckCollisionPointRec(GetMousePosition(), PanelRect());
    bool overPalette = CheckCollisionPointRec(GetMousePosition(), PaletteRect());
    bool overSave = CheckCollisionPointRec(GetMousePosition(), SaveButtonRect());
    bool overMode = CheckCollisionPointRec(GetMousePosition(), ModeButtonRect());
    bool overUi = overPanel || overPalette || overSave || overMode;

    // Cancel placement mode.
    if (!placeArchetype_.empty() &&
        (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)))
        placeArchetype_.clear();

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
        if (!placeArchetype_.empty())
        {
            // Placement mode: place the object at the click point (we don't reset the
            // mode — several can be placed; exit with Esc / RMB).
            AddObject(placeArchetype_, wm);
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
    if (mode_ == Mode::Galaxy)
    {
        DrawGalaxy();
        return;
    }

    BeginMode2D(camera_);

    // Just the map boundary (we don't draw security zones/rings).
    DrawCircleLines(0, 0, World::SYSTEM_RADIUS, Fade(Ui::PANEL_BORDER, 0.5f));

    // The editor presents the world through the same backend the game does (#35), so
    // what it shows is what a player will see rather than a second drawing path that
    // drifts from it.
    //
    // Lit by the system's own star, exactly as the game lights it (#119) -- the lights
    // outlive the scene here because the placement ghost below is lit by them too.
    Render::Lighting lights;
    {
        std::vector<Render::Item> scene;
        scene.reserve(entities_.size());
        for (const auto& e : entities_)
            scene.push_back(e->Describe());
        lights = Render::LightsFrom(scene);
        Render::Present(std::move(scene), lights, *backend_);
    }

    // Highlight the selected object.
    if (selected_ >= 0 && selected_ < (int)entities_.size())
    {
        Vector2 p = entities_[selected_]->GetPosition();
        float   r = entities_[selected_]->GetSize() + 12.0f;
        DrawCircleLines(p.x, p.y, r, Ui::ACCENT);
        DrawCircleLines(p.x, p.y, r + 4.0f, Fade(Ui::ACCENT, 0.5f));
    }

    // Ghost of the object being placed, under the cursor.
    if (!placeArchetype_.empty() && !CheckCollisionPointRec(GetMousePosition(), PaletteRect()))
    {
        Vector2                 wp = GetScreenToWorld2D(GetMousePosition(), camera_);
        std::unique_ptr<Entity> ghost = MakeEntity(placeArchetype_, wp);
        if (ghost)
        {
            // Lit like the system it is about to be placed in, not like the last thing
            // drawn: the ghost is a preview of the world, so it uses the world's lights.
            Render::Present({ ghost->Describe() }, lights, *backend_);
            DrawCircleLines(wp.x, wp.y, ghost->GetSize() + 12.0f, Fade(Ui::ACCENT, 0.7f));
        }
    }

    EndMode2D();
}

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
