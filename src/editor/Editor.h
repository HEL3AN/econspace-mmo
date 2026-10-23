#pragma once

#include "raylib.h"
#include "core/WorldLoader.h"
#include "entities/Entity.h"
#include "render/GlyphBackend.h"
#include "render/MaterialLibrary.h"
#include "render/Treatment.h"
#include "render/TreatmentPanel.h"
#include "core/Archetype.h"
#include "gen/Pins.h"
#include "gen/Survey.h"
#include <nlohmann/json.hpp>
#include <future>
#include <utility>
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
    // `worldeditor modules [pack P] [seeds N] [zoom Z]`: the module library as a sheet of
    // cards (#240) -- one pack of it, and each variant at N seeds, which is how a range or a
    // palette is judged: one drawing says nothing about a family.
    void OpenModules(float zoom, const std::string& pack, int seeds)
    {
        mode_ = Mode::Modules;
        modulesZoom_ = zoom;
        modulesPack_ = pack;
        modulesSeeds_ = seeds < 1 ? 1 : seeds;
    }
    void        DrawModules(bool labels);
    void        HandleModulesInput();
    float       modulesZoom_ = 1.0f;
    std::string modulesPack_;
    int         modulesSeeds_ = 1;
    float       modulesScroll_ = 0.0f;   // pixels the sheet is scrolled down
    float       modulesContent_ = 0.0f;  // how tall the sheet is, for clamping the scroll
    std::string modulesFocus_;           // one module, large, at many seeds; empty: the sheet
    std::string modulesHover_;           // the module under the mouse, found while drawing

    // Starts on the shape backend instead of glyphs. F2 still switches; this is for
    // opening straight into the one you meant to look at.
    void UseShapes();

    // Opens with the screen treatment's settings showing (F10), which is where a shader
    // that failed on this machine is named (#120, #190). Synthetic key presses do not reach
    // a raylib window, so this is how a screenshot of that panel is taken.
    void OpenTreatmentSettings() { treatmentPanelOpen_ = true; }

    // `shot FILE [frames N]`: saves what the window shows after N frames and exits. Read
    // back from a texture of its own rather than copied from the desktop, so it works when
    // the screen is off or covered -- a desktop capture then comes back white.
    // `perf [notreat]`: run N frames uncapped and print what they cost (#296).
    void MeasurePerf(int frames, bool treated);
    // `noshaders`: no material compiled, so every object is drawn as it is on a driver that
    // refused them all -- how a stand-in composition is judged (#426).
    void DisableMaterials()
    {
        materials_.SetDisabled(true);
        materials_.Load(dataDir_);
    }
    // `nopaint`: only the painting materials off, `hull` kept -- a driver that refuses the
    // shaders that paint a body and nothing else, the likelier failure (#426).
    void DisablePainting()
    {
        materials_.SetPaintingDisabled(true);
        materials_.Load(dataDir_);
    }
    void TakeShot(const std::string& path, int frames, bool treated = false)
    {
        shotPath_ = path;
        shotFrames_ = frames;
        shotTreated_ = treated;
    }
    int             perfFrames_ = -1;  // `perf`: frames left to measure; -1 when not
    int             perfTotal_ = 0;
    std::string     shotPath_;
    bool            shotTreated_ = false;  // `treated`: the shot goes through the treatment
    int             shotFrames_ = 30;
    RenderTexture2D shotTarget_ = {};

    // Opens straight into the survey (#141): the regions of consecutive seeds from `seed`,
    // every system on one screen. `card` (>= 0) opens that card enlarged, which is how a
    // screenshot of one is taken without a click.
    void OpenSurvey(uint64_t seed, int card = -1);

    // Opens a system of the region `seed` makes for editing (#237): what the generator made,
    // with every pin on top, exactly as the server builds it. Saving writes only the
    // difference, as a pin in data/pins.json -- a generated system has no file of its own.
    // An empty `systemId` opens the system through the wormhole. False if there is no such
    // system (and the log says which there are).
    bool OpenGenerated(uint64_t seed, const std::string& systemId);

    // Opens the map of the whole region `seed` makes (#237): its systems where the generator
    // put them, the gates between them, which ones a pin touches. A click opens a system for
    // editing; `focus` (a system id) is ringed, which is where the map was opened from.
    void OpenRegionMap(uint64_t seed, const std::string& focus = "");
    // ...the region of skeleton slot `slot` instead, opened or not (#342 S8).
    void OpenRegionSlot(uint64_t seed, int slot);

    // The world's galaxy (#342 S8): every slot of the skeleton the seed lays out, which are
    // open and how, which a pin waits on. A click opens the region of a slot -- the first of
    // the F5 levels, galaxy -> region -> system.
    void OpenWorldMap(uint64_t seed);
    // Makes the editor open regions from a saved world -- world.json, or the folder it is in --
    // instead of the world a new server would make: its seed, its centres and its cap, its
    // opening log, and each region it opened read from its seal in regions/ beside it. False,
    // said in the log, when the file is not a world.
    bool     OpenWorldFile(const std::string& path);
    uint64_t WorldSeed() const { return world_.seed; }

    // Packs (#342 S8). `packs DIR` writes and opens them somewhere other than data/packs/,
    // which is what a server weaves into a new world.
    void SetPacksDir(const std::string& dir);
    // Opens the pack `id` for editing on the map of its systems, or begins one (`create`) from
    // a single system with a station. False, said in the log, when it cannot.
    bool OpenPack(const std::string& id, bool create);
    // Writes the region on the region map as the pack `id` (Gen::RegionAsPack) and opens it.
    bool ExportRegion(const std::string& id);

    // Opens the gallery on one archetype drawn large (#194), by registry index or by id,
    // at `zoom` times the fitted size. `worldeditor gallery card trade_hub zoom 3` is how a
    // screenshot of the fine detail is taken without a click. Unknown: the grid, and a log
    // line saying so.
    void FocusGallery(const std::string& which, float zoom = 1.0f);
    // `gallery card ID seed N [sockets]`: which object of the type, and its sockets shown.
    void SetGallerySeed(int seed) { gallerySeed_ = seed; }
    void ShowGallerySockets() { gallerySockets_ = true; }
    void SetGalleryThrusting() { galleryThrusting_ = true; }
    // Every card at one scale, so sizes compare -- the panel's toggle, from the command line.
    void SetGalleryTrueScale() { galleryTrueScale_ = true; }

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
    // A satellite of a planet, or no longer one (#210). Attaching keeps the object where it
    // stands: the orbit is measured from the nearest planet as the editor shows it.
    void AttachSelectedToNearestPlanet();
    void DetachSelected();
    // Where the planet with this index in the "planets" array is drawn, or nullptr.
    const Entity* PlanetEntity(int planetIndex) const;

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
    // The large single-object view (#194): one archetype over the whole grid, with a zoom.
    Rectangle GalleryFocusRect() const;
    Camera2D  GalleryFocusCamera(const Archetype& a) const;
    void      DrawGalleryFocus();
    void      DrawGalleryFocusBar();  // the zoom slider and what it means, in the header
    // What the state sliders say is happening to this object right now. Built through the
    // same Render::FromArchetype the game's entities go through, so the gallery cannot
    // show a picture the world would not.
    Render::Item GalleryItem(const Archetype& a, Vector2 pos, float size) const;
    // Writes the edited look fields back into data/archetypes.json, in place.
    void SaveArchetypes();
    // Records that a key of an archetype was changed, so the save touches only those.
    void NoteLookEdit(const std::string& id, const std::string& key);

    // Survey mode (#141): many generated systems at once, so a rule is judged on its
    // distribution rather than its best case. The counting is Gen::Analyse (no window);
    // this half draws it.
    struct SurveyCard
    {
        Gen::SurveySystem         system;
        std::vector<Render::Item> items;   // described once, when the batch is generated
        std::vector<std::string>  labels;  // parallel to items, for the enlarged view
    };
    Rectangle SurveyButtonRect() const;
    void      EnterSurveyMode(bool on);
    void      RegenerateSurvey();  // generates the batch of seeds from surveySeed_
    void      HandleSurveyInput();
    // The grid, or the enlarged card over it. Drawn twice: the pictures inside the screen
    // treatment, then the labels and flags after it, so a tool stays literal (as the HUD).
    void      DrawSurvey(bool labels);
    void      DrawSurveyPanel();  // the distributions
    Rectangle SurveyGridRect() const;
    Rectangle SurveyCardRect(int index) const;
    int       SurveyHit(Vector2 p) const;
    // One system fitted into `box`. `detail` is the enlarged view: names on everything.
    void DrawSurveySystem(const SurveyCard& card, Rectangle box, bool detail, bool labels);

    // The region a server would generate from a seed starts from this: the same home system,
    // the same map around it. `homeDoc` holds the home system's document the params point at.
    // The region `systemId` is in, in the world a seed makes, unpinned -- made as the server
    // makes it (#342 S2): every centre and a frontier beside each, or any other slot on its
    // own. An empty id is the first centre, and becomes its heart. `names` gets every
    // system's name in the world; `homeName`/`homeMap` say where a frontier's wormhole opens
    // from (empty for a centre). False when the seed's world has no such system.
    bool WorldRegion(uint64_t seed, std::string& systemId, Gen::Region& out,
                     std::map<std::string, std::string>* names = nullptr,
                     std::string* homeName = nullptr, Vector2* homeMap = nullptr) const;
    // ...the region of skeleton slot `slot` (-1: the first centre's). A slot the world has not
    // opened is made as it would open from its lowest-numbered open neighbour (#342 S8).
    bool WorldRegionAt(uint64_t seed, int slot, Gen::Region& out,
                       std::map<std::string, std::string>* names = nullptr,
                       std::string* homeName = nullptr, Vector2* homeMap = nullptr) const;

    // The world regions are opened from (#342 S8): the one a new server makes from a seed
    // (without admin packs, which only a server weaves), or a saved one. A saved world applies
    // only to its own seed: PgDn on the region map leaves it for the seeds around it.
    struct WorldSource
    {
        uint64_t    seed = 1;
        std::string path;     // world.json; empty: a new world of the seed
        std::string sealDir;  // regions/ beside it
        int         centres = Gen::DEFAULT_CENTRES;
        int         cap = Gen::DEFAULT_REGION_CAP;
        // The opening log in the order it opened; archived regions left out.
        struct Entry
        {
            int         slot = 0;
            int         parent = -1;
            std::string state = "open", by = "world", why;
        };
        std::vector<Entry>         log;
        std::map<int, std::string> packs;  // slot -> the pack woven into it
        // world.json's `galaxy`: the name each system was given, and who found it (#431).
        std::map<std::string, std::string> names, found;
    };
    WorldSource world_;
    bool        Saved(uint64_t seed) const { return !world_.path.empty() && world_.seed == seed; }
    Gen::WorldParams WorldParamsFor(uint64_t seed) const;
    // Every region open in the world of `seed`, in the order it opened: a saved world's from
    // their seals, a new world's generated. Kept for the last seed asked about.
    const std::vector<Gen::Region>&  OpenRegions(uint64_t seed) const;
    mutable uint64_t                 openSeed_ = 0;
    mutable bool                     openSaved_ = false;
    mutable std::vector<Gen::Region> openRegions_;

    // The generated system being edited (#237). While `open`, systemJson_ is the edit and
    // `base` what it is measured against: the generator's system with every pin the editor
    // does not own. `origins` follows each object of systemJson_ back to `base`, which is
    // how a moved planet -- it has no name -- is saved as "that one, moved".
    struct GeneratedEdit
    {
        bool           open = false;
        uint64_t       seed = 0;
        std::string    id;
        std::string    name;
        nlohmann::json base;
        Gen::Origins   origins;
        int            problems = 0;  // pins that could not apply, said in the log
        // The systems of the region, and their names: where its gates lead, which is the
        // region's topology and not the editor's to change.
        std::set<std::string>              systems;
        std::map<std::string, std::string> names;
    };
    GeneratedEdit generated_;
    void          SavePin();  // the difference from base, into data/pins.json
    // What an object of the open generated system is: as generated, or a pin's.
    enum class Provenance
    {
        Generated,
        Changed,  // a generated object a pin changes
        Added     // an object only a pin has
    };
    Provenance ProvenanceOf(const ObjHandle& h) const;
    // The marks of a pin's work: warm and green, so they never read as the accent a
    // selection is drawn in.
    static constexpr Color PIN_CHANGED = { 236, 180, 84, 255 };
    static constexpr Color PIN_ADDED = { 120, 214, 140, 255 };

    // A line under the HUD for a few seconds: why something was refused, or what went with
    // it. Also written to the log.
    void        Notice(const std::string& text);
    std::string notice_;
    double      noticeUntil_ = 0.0;

    std::vector<SurveyCard> surveyCards_;
    Gen::SurveyResult       survey_;
    uint64_t                surveySeed_ = 1;     // the first seed of the batch
    int                     surveyRegions_ = 3;  // seeds per batch; a region is 18 systems
    int                     surveySelected_ = -1;
    float                   surveyScroll_ = 0.0f;
    bool                    surveyMarkers_ = true;  // M: belts, wrecks, gates marked

    // The screen treatment (#120), applied to the cards and not to the panels: the
    // gallery is where the treatment is tuned, and a settings screen seen through the
    // effect it is adjusting cannot be read while adjusting it.
    Render::MaterialLibrary materials_;  // the shaders a material names (#121)
    Render::Treatment       treatment_;
    Render::TreatmentPanel  lookPanel_;
    bool                    treatmentPanelOpen_ = false;
    void                    DrawTreatmentSettings();

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
    // The focused view (#194). A card is at most ~77 px across, below the threshold of
    // every part with a larger minPixels -- the parts most in need of judging by eye.
    bool    galleryFocus_ = false;
    float   galleryZoom_ = 1.0f;           // 1 = the object fitted to the view
    Vector2 galleryPan_ = { 0.0f, 0.0f };  // world units, from the object's centre
    // Which object of the type is drawn ([ and ] step it): a kit and every range decide by
    // the seed, so one seed says nothing about whether the rules make sense (#240).
    int gallerySeed_ = 0;
    // Draw the sockets a type's sections offer, coloured by kind (K): how a kit's choices
    // are judged -- why a hatch is where it is.
    bool gallerySockets_ = false;
    bool archetypesDirty_ = false;
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
        System,   // one star system, the objects in it
        Galaxy,   // an index -- a pack's pack.json, or universe.json: nodes and links
        Gallery,  // every archetype at once, for judging a look (#118)
        Survey,   // many generated systems at once, for judging the rules (#141)
        Modules,  // the module library, every variant side by side (#240)
        Region,   // the whole region of one seed, as a map (#237)
        World     // the world's galaxy: every slot of the skeleton (#342 S8)
    };
    Mode mode_ = Mode::System;

    // The region map (#237). Generated, then pinned, exactly as the server builds it, so a
    // system's summary on the map is what opening it shows.
    struct RegionMap
    {
        uint64_t                        seed = 0;
        Gen::Region                     region;
        std::vector<Gen::SystemSummary> summaries;     // parallel to region.systems
        std::map<std::string, int>      pins;          // system id -> pins for exactly this seed
        std::set<std::string>           everySeed;     // systems a seedless pin touches
        int                             problems = 0;  // pins that could not apply
        std::string                     homeName;
        Vector2                         homeMap = { 0.0f, 0.0f };
        std::string                     focus;                // the system the map was opened from
        Mode                            back = Mode::System;  // where Esc goes
        std::string                     notice;               // why a click did nothing
        Rectangle                       bounds = { 0, 0, 1, 1 };  // every node, in map units
    };
    RegionMap regionMap_;
    // Moderation of a saved world's names (#431 N3): R on a system of the region map types a
    // name ("-" clears it), which goes to the server running that world as its console's
    // `rename`, through the admin channel -- the server names it, so it is news and its
    // discoverer is told. Asked off the frame loop; the answer is the map's notice.
    struct Rename
    {
        bool                                      typing = false;
        std::string                               id, text;
        std::future<std::pair<bool, std::string>> reply;  // valid while the server is asked
    };
    Rename    rename_;
    void      HandleRenameInput();
    void      DrawRename();
    void      BuildRegionMap(uint64_t seed, std::string focus, int slot = -1);
    void      HandleRegionInput();
    void      DrawRegionMap();
    void      DrawRegionPanel();
    Rectangle RegionMapRect() const;
    Rectangle RegionButtonRect() const;
    Vector2   RegionToScreen(Vector2 map) const;
    int       RegionHit(Vector2 p) const;  // index in region.systems, -1 none, -2 home
    void      ToggleRegionMap();           // the button: from where you are, and back
    void      LevelUp();                   // F5: system -> region -> galaxy -> back
    // Tab: the next region the world has open after `slot`; -1 when it has none.
    int NextOpenSlot(uint64_t seed, int slot) const;

    // The world's galaxy (#342 S8): the skeleton of slots and what the world made of each.
    struct SlotView
    {
        std::string state;  // empty: not open; else the log's (open, unstable, anchored...)
        int         parent = -1;
        std::string by, why;
        std::string title;  // a centre's owner, a frontier's natives, a pack's id
        std::string pack;
        int         systems = 0;
        int         pins = 0;       // pins for exactly this seed, applied or waiting
        int         everySeed = 0;  // pins for every seed
    };
    struct WorldMap
    {
        uint64_t              seed = 0;
        Gen::GalaxySkeleton   skeleton;
        std::vector<SlotView> slots;  // parallel to skeleton.slots
        Mode                  back = Mode::System;
        Rectangle             bounds = { 0, 0, 1, 1 };
        std::string           notice;
    };
    WorldMap worldMap_;
    void     BuildWorldMap(uint64_t seed);
    void     HandleWorldInput();
    void     DrawWorldMap();
    void     DrawWorldPanel();
    Vector2  WorldToScreen(double x, double y) const;
    int      WorldHit(Vector2 p) const;  // the slot under a point, or -1

    // A pack being edited (#342 S8): its pack.json on the index map (Mode::Galaxy), its
    // system files in the system view.
    std::string packsDir_;    // data/packs/ unless `packs DIR`
    std::string packId_;      // empty: the index is universe.json
    std::string indexPath_;   // the index file Mode::Galaxy edits
    std::string systemsDir_;  // where the index's system files are
    void        CheckPack();  // LoadPack on it: every problem to the log, the first on screen
    // data/pins.json, an empty object if there is none, null if it is not valid JSON.
    static nlohmann::json ReadPins(const std::string& path);

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
