#pragma once

#include "raylib.h"
#include "core/WorldLoader.h"
#include "entities/Entity.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <memory>
#include <string>

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
    Rectangle                     PaletteRect() const;
    void                          DrawPalette();
    void                          DrawEntityPreview(Rectangle box, const std::string& category);
    std::unique_ptr<Entity>       MakeEntity(const std::string& category, Vector2 pos) const;
    void                          AddObject(const std::string& category, Vector2 pos);
    void                          DeleteSelected();

    Rectangle SaveButtonRect() const;
    void      SaveCurrentSystem();  // writes systemJson_ back to the system's source file

    // Galaxy mode: editing universe.json (system nodes and links).
    Rectangle ModeButtonRect() const;
    void      EnterGalaxyMode(bool on);
    void      HandleGalaxyInput();
    void      DrawGalaxy();        // nodes and links in world coordinates
    void      DrawGalaxyPanel();   // right-hand panel for the selected system
    void      AddSystem();
    void      DeleteSystem(int index);
    void      ToggleLink(const std::string& a, const std::string& b);
    bool      HasLink(const std::string& a, const std::string& b) const;
    void      RefreshUniverseStruct();  // rebuild universe_ from universeJson_
    void      SaveUniverse();
    void      OpenSystem(int index);    // leave the galaxy for the selected system
    // Creates/removes a gate in system sysId's file leading to destId (positioned
    // toward destId on the galaxy map).
    void      SyncGate(const std::string& sysId, const std::string& destId, bool add);

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

    // Galaxy mode.
    bool           galaxyMode_ = false;
    nlohmann::json universeJson_;          // the galaxy index being edited
    int            gSelected_ = -1;        // selected system (index)
    bool           universeDirty_ = false;
    bool           gGrabbed_ = false;      // node pressed (potential drag)
    bool           gDragging_ = false;     // node actually moving (threshold passed)
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
    std::vector<ObjHandle>               handles_;     // parallel to entities_

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
    std::string placeCategory_;            // placement mode: which type we place on click
    bool        paletteOpen_ = false;      // whether the creation palette is expanded

    // The open dropdown (field key) and its deferred rendering.
    std::string              openDropdown_;
    bool                     anyDropdownOpen_ = false;
    nlohmann::json*          dropObj_ = nullptr;
    std::string              dropKey_;
    std::vector<std::string> dropOpts_;
    std::vector<std::string> dropLabels_;
    Rectangle                dropAnchor_ = { 0, 0, 0, 0 };
};
