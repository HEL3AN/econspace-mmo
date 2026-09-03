#pragma once

#include "raylib.h"
#include "core/WorldLoader.h"
#include "entities/Entity.h"
#include "render/GlyphBackend.h"
#include "render/Treatment.h"
#include "core/Archetype.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <memory>
#include <string>
#include <map>
#include <set>

// World editor: a standalone app for visually editing data/.
// The source of truth is the system JSON in memory (systemJson_); the entities
// (entities_) are rebuilt from it for rendering. Each entity has a "handle"
// (category + index in the JSON) used for selection and editing.
class Editor
{
public:
    Editor();
    ~Editor();

    void Run();

    // Opens straight into the gallery (#118). `worldeditor gallery` is how a look gets
    // tuned, and the tool for judging one should not make you cross a star system first.
    void OpenGallery();

    // Starts on the shape backend instead of glyphs. F2 still switches; this is for
    // opening straight into the one you meant to look at.
    void UseShapes();

private:
    // Reference to a JSON element: array category and index (star uses index=-1).
    struct ObjHandle
    {
        std::string category;
        int         index;
    };

    void LoadSystemAt(int index);  // loads the system at that index in universe_
    void RebuildEntities();        // rebuilds entities_ and handles_ from systemJson_

    int  HitTest(Vector2 worldMouse) const;  // index of the entity under the cursor, or -1
    void MoveSelected(Vector2 desiredPos);   // places the selected object at a position

    void HandleInput();
    void DrawWorld();
    void DrawHud();

    // Object-creation palette (on the left) and deletion.
    Rectangle               PaletteRect() const;
    void                    DrawPalette();
    void                    DrawEntityPreview(Rectangle box, const std::string& archetypeId);
    std::unique_ptr<Entity> MakeEntity(const std::string& archetypeId, Vector2 pos) const;
    void                    AddObject(const std::string& archetypeId, Vector2 pos);
    void                    DeleteSelected();

    Rectangle SaveButtonRect() const;
    void      SaveCurrentSystem();  // writes systemJson_ back to the system's source file

    // Galaxy mode: editing universe.json (system nodes and links).
    Rectangle ModeButtonRect() const;
    void      EnterGalaxyMode(bool on);
    void      HandleGalaxyInput();
    void      DrawGalaxy();       // nodes and links in world coordinates
    void      DrawGalaxyPanel();  // right-hand panel for the selected system
    void      AddSystem();
    void      DeleteSystem(int index);
    void      ToggleLink(const std::string& a, const std::string& b);
    bool      HasLink(const std::string& a, const std::string& b) const;
    void      RefreshUniverseStruct();  // rebuild universe_ from universeJson_
    void      SaveUniverse();
    void      OpenSystem(int index);  // leave the galaxy for the selected system
    // Creates/removes a gate in system sysId's file leading to destId (positioned
    // toward destId on the galaxy map).
    void SyncGate(const std::string& sysId, const std::string& destId, bool add);

    // Gallery mode: every archetype in the registry on one screen (#118). A look is
    // judged by eye, and the loop it replaces was build, serve, connect, fly, look.
    Rectangle GalleryButtonRect() const;
    void      EnterGalleryMode(bool on);
    void      HandleGalleryInput();
    void      DrawGallery();       // the grid of cards
    void      DrawGalleryPanel();  // state sliders, and the look of the selected archetype
    Rectangle GalleryCardRect(int index) const;
    int       GalleryHit(Vector2 p) const;  // card under a point, or -1
    // What the state sliders say is happening to this object right now. Built through the
    // same Render::FromArchetype the game's entities go through, so the gallery cannot
    // show a picture the world would not.
    Render::Item GalleryItem(const Archetype& a, Vector2 pos, float size) const;
    // Writes the edited look fields back into data/archetypes.json, in place.
    void SaveArchetypes();
    // Records that a key of an archetype was changed, so the save touches only those.
    void NoteLookEdit(const std::string& id, const std::string& key);

    // The screen treatment (#120), applied to the cards and not to the panels: the
    // gallery is where the treatment is tuned, and a settings screen seen through the
    // effect it is adjusting cannot be read while adjusting it.
    Render::Treatment treatment_;
    bool              treatmentPanelOpen_ = false;
    void              DrawTreatmentSettings();

    // The gallery's own light (#119). A card has no system around it, so lighting is
    // tuned against a synthetic source whose angle and reach are on sliders -- which is
    // the whole reason the gallery came first in this milestone.
    Render::Lighting GalleryLighting(Vector2 at, float size) const;

    bool  galleryLit_ = true;
    float galleryLightAngle_ = 2.4f;     // radians, where the light comes from
    float galleryLightStrength_ = 1.0f;  // 0..1 at the object
    float galleryAmbient_ = 0.38f;       // the floor nothing goes below
    bool  gallerySecondLight_ = false;   // a second star, opposite and cooler

    int   gallerySelected_ = -1;
    float galleryScroll_ = 0.0f;      // pixels the card grid is scrolled by
    bool  galleryTrueScale_ = false;  // one scale for all cards, so sizes compare
    float galleryIntensity_ = 1.0f;   // hull left, ore left, a wreck already looted
    float galleryHeading_ = 0.0f;     // radians, for the things that point somewhere
    bool  galleryThrusting_ = false;
    bool  archetypesDirty_ = false;
    // Archetype id -> the look keys edited since the last save.
    std::map<std::string, std::set<std::string>> lookEdits_;

    // Property panel for the selected object (on the right).
    Rectangle PanelRect() const;
    void      DrawPropertyPanel();
    // A field with manual entry (name or number) plus -/+ buttons for numbers.
    // Returns true if the value changed.
    bool FieldRow(Rectangle r, const char* label, nlohmann::json& obj, const char* key,
                  bool numeric, bool asInt, double step = 1.0);
    // A dropdown (enumeration). opts — the values written out; labels (if given)
    // — what's shown in their place (e.g. a system name instead of its id).
    void DropdownRow(Rectangle r, const char* label, nlohmann::json& obj, const char* key,
                     const std::vector<std::string>& opts,
                     const std::vector<std::string>& labels = {});

    int screenWidth_ = 1280;
    int screenHeight_ = 720;

    std::string           dataDir_;  // path to the source data/ folder
    WorldLoader::Universe universe_;
    int                   currentSystem_ = 0;
    bool                  dirty_ = false;  // there are unsaved edits to the system

    // Which of the three views is up. An enum rather than a flag per view, because two
    // flags can both be true and there is no such screen.
    enum class Mode
    {
        System,  // one star system, the objects in it
        Galaxy,  // universe.json: the nodes and the links between them
        Gallery  // every archetype at once, for judging a look (#118)
    };
    Mode mode_ = Mode::System;

    // Galaxy mode.
    nlohmann::json universeJson_;    // the galaxy index being edited
    int            gSelected_ = -1;  // selected system (index)
    bool           universeDirty_ = false;
    bool           gGrabbed_ = false;   // node pressed (potential drag)
    bool           gDragging_ = false;  // node actually moving (threshold passed)
    Vector2        gGrabAnchor_ = { 0, 0 };

    // The selected system's links as an editable list (keys "0".."n" → partner id).
    nlohmann::json gLinksJson_;
    std::string    gLinksOwner_;  // which system gLinksJson_ was built for

    double gLastClickTime_ = -1.0;  // for recognizing a double-click on a node
    int    gLastClickNode_ = -1;

    // Renaming a system: we remember the id/file when id editing starts, so that
    // when it ends we can rename the file on disk and update references.
    bool        gIdEditing_ = false;
    std::string gOldId_;
    std::string gOldFile_;

    nlohmann::json                       systemJson_;  // the model being edited
    std::vector<std::unique_ptr<Entity>> entities_;    // for rendering
    // The same presentation seam the game uses (#35). F2 switches; the editor showing a
    // different picture from the game is exactly what #37 is about avoiding.
    Render::GlyphBackend   glyphBackend_;
    Render::ShapeBackend   shapeBackend_;
    Render::IBackend*      backend_ = &glyphBackend_;
    std::vector<ObjHandle> handles_;  // parallel to entities_

    int selected_ = -1;

    Camera2D camera_;
    bool     panning_ = false;
    Vector2  dragLast_ = { 0.0f, 0.0f };

    // Dragging an object: "grabbed" (pressed) and "actually dragging" (moved).
    bool    objectGrabbed_ = false;
    bool    objectDragging_ = false;
    Vector2 grabAnchor_ = { 0.0f, 0.0f };  // offset of the object center from the grab point
    Vector2 pressPos_ = { 0.0f, 0.0f };    // screen point of the press (drag threshold)

    // Property entry: the active text/number field, its buffer and caret position.
    std::string activeField_;
    std::string editBuffer_;
    int         caretPos_ = 0;
    bool        deleteRequested_ = false;  // delete the selected one after drawing the panel
    std::string placeArchetype_;           // placement mode: the archetype id placed on click
    bool        paletteOpen_ = false;      // whether the creation palette is expanded
    int         paletteScroll_ = 0;        // first visible row; the registry outgrows the screen

    // The open dropdown (field key) and its deferred rendering.
    std::string              openDropdown_;
    bool                     anyDropdownOpen_ = false;
    nlohmann::json*          dropObj_ = nullptr;
    std::string              dropKey_;
    std::vector<std::string> dropOpts_;
    std::vector<std::string> dropLabels_;
    Rectangle                dropAnchor_ = { 0, 0, 0, 0 };
};
