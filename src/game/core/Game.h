#pragma once

#include "raylib.h"
#include "entities/Entity.h"
#include "entities/Ship.h"
#include "missions/MissionSystem.h"
#include "core/WorldLoader.h"
#include "sim/PlayerStep.h"
#include "sim/Protocol.h"
#include "net/Transport.h"
#include "net/Tcp.h"
#include "render/GlyphBackend.h"  // ShapeBackend
#include "render/MaterialLibrary.h"
#include "render/CameraRig.h"
#include "sim/Overview.h"
#include "sim/Sensor.h"
#include "sim/WorldClock.h"
#include "render/Treatment.h"
#include "player/Player.h"
#include <string>
#include "ui/ContextMenu.h"
#include "ui/Desk.h"
#include "ui/Layout.h"
#include "core/RangePicker.h"
#include <deque>
#include <map>
#include <vector>
#include <memory>

class Station;  // used only as pointers
class AsteroidField;
class NpcShip;
enum class NpcRole;  // NPC role (defined in entities/NpcShip.h)

// Game mode: flying in space, or the docked-station screen.
enum class GameMode
{
    Flying,
    Docked
};

// A weapon beam for the current frame (for rendering).
struct Beam
{
    Vector2 a;
    Vector2 b;
    Color   color;
};

// A background star for the parallax sky: base position within the tile,
// depth (fraction of camera shift), and brightness.
struct BgStar
{
    Vector2       base;
    float         depth;
    unsigned char shade;
};

// Owns the game state and runs the main loop.
// RAII: the constructor opens the window, the destructor closes it.
class Game
{
public:
    // The client's fixed step: prediction and input numbering both run on it, and it must
    // match the server's SIM_DT or one input stops meaning one tick -- so it is not a
    // second copy of the number, it is the same one.
    static constexpr float SIM_DT = Sim::SIM_DT;

    // Takes an already-established connection to an econserver host (main() dials it).
    // There is no offline mode: without a connection there is no world to render.
    explicit Game(std::unique_ptr<Net::TcpConnection> conn);

    // Where the camera starts (--zoom): for looking at a scale without a wheel, which is
    // also how a screenshot of one is taken -- synthetic input does not reach the window.
    void SetStartZoom(float zoom) { rig_.SetZoom(zoom); }
    // Order a warp to a point shortly after joining (--warp X Y): for watching a warp, and
    // screenshotting one, without a hand on the controls.
    void StartWithWarp(Vector2 target)
    {
        startWarpTarget_ = target;
        startWarpFrames_ = 120;  // two seconds: the first snapshots have placed the ship
    }
    // Start with the galaxy map open (--map): for seeing the index the server sent without
    // a hand on the keyboard.
    void StartOnMap() { desk_.SetOpen(WIN_MAP, true); }
    // The interface at exactly this scale, whatever the display asks for (--uiscale): for
    // pictures that look the same on every machine. Not saved.
    void SetUiScale(float s)
    {
        Ui::SetUserScale(s);
        Ui::OverrideDisplayScale(1.0f);
        uiScaleLocked_ = true;
    }
    // Start with the sensor screen open (--sensor), at a range if one is given: the same
    // reason as --map, for a picture of the instrument.
    void StartOnSensor(float range)
    {
        desk_.SetOpen(WIN_SENSOR, true);
        if (range > 0.0f)
            sensorRange_ = Sensor::StepRange(range, 0);
    }
    // Save the frame drawn after N frames to a PNG and exit (--shot FILE --frames N): the
    // editor's mechanism, for pictures of the game itself. The frame is drawn into a
    // texture of its own, because with the screen off the window's pixels read back blank,
    // and without the screen treatment, which keeps a target of its own.
    // `treated` (--treated) puts the shot through the screen treatment, as the player sees it.
    void TakeShot(const std::string& path, int frames, bool treated = false)
    {
        shotPath_ = path;
        shotFrames_ = frames;
        shotTreated_ = treated;
    }
    // Draw the world alone, without the HUD (--nohud): for a picture of the place rather
    // than of the instruments. Nothing else changes; the windows still exist.
    void HideHud() { hudHidden_ = true; }
    // Measure what frames cost (--perf, #296): uncapped, the GPU waited for at every phase,
    // and the averages over the second half of `frames` printed to stderr before exiting --
    // the first half is the connection settling. With --shot the shot frame ends the run.
    void MeasurePerf(int frames);
    // Start the screen treatment off (--notreat) without touching look.json: for measuring
    // what the passes cost.
    void DisableTreatment() { treatment_.Config().enabled = false; }
    // Open at a given size (--size W H): a measurement at the resolution of the machine it
    // is about, rather than at the default window's.
    void SetResolution(int w, int h) { ApplyResolution(w, h); }
    // Who this client is logged in as: a discoverer may name what they found (#145), and
    // the window layout is kept per account (#297).
    void SetPilotName(const std::string& n);
    ~Game();

    void Run();

private:
    void HandleInput(float dt);  // client: input → player command (cmd_)
    void DrawWorld();
    void DrawStarfield();  // parallax star background (screen coordinates)
    void DrawHud();
    void DrawStationScreen();
    void DrawMissionBoard(int x, int y, int w);  // mission board at the station

    void SetupWindows();                         // puts every window, screen and popup on the desk
    void HandleMenuBar();                        // menu bar input: a button toggles its window
    void DrawMenuBar();                          // vertical menu bar on the left
    void ApplyResolution(int w, int h);          // changes the window size
    void DrawSettingsContent(Rectangle area);    // contents of the settings window
    void OpenContextMenu(Entity* target);        // right-click action menu on an object
    void OpenContextMenuAt(Vector2 worldPoint);  // right-click menu on empty space
    // Navigation orders (client → command; applied by the server in StepPlayerShip).
    void OrderAutopilot(Vector2 target, float stopDist);  // fly to a point
    void OrderWarp(Vector2 target, float dropDist);       // warp to a point
    void DrawStatusContent(const Ui::Frame& f);           // contents of the status window
    void DrawTargetContent(Rectangle area);               // contents of the selected-target window
    void DrawOverviewContent(const Ui::Frame& f);         // list of objects in the system
    void DrawRadarContent(const Ui::Frame& f);            // system radar minimap
    void DrawMissionsContent(Rectangle area);             // log of active missions
    void DrawGalaxyMap();                                 // full-screen star map
    void DrawSensorScreen();                              // the sensor grid (#123)
    // The sensor cell under a screen point, from the last picture drawn: the id of what is
    // in it (0 if nothing) and the world point at its centre. False off the grid.
    bool             SensorPick(Vector2 screen, int& id, Vector2& world) const;
    Sensor::Standing ViewerStanding() const;  // this pilot's standing, for the instruments
    bool CanNameHere() const;  // in a system this pilot found and nobody has named (#145)
    void HandleNaming();       // typing a name: every key goes to the name until Enter/Esc
    void HandleEscape();       // Esc closes whatever is on top, and never quits
    void SaveTreatment();      // writes what was tuned in F10's panel

    void Undock();

    const WorldLoader::SystemInfo* CurrentSystemInfo() const;  // record of the current system
    bool                           HostileToPlayerFaction(
        FactionId f) const;  // is the faction hostile to the player (reputation/wanted)

    // M4c: the client renders from the snapshot. The snapshot is built every frame
    // (world from the server + player state); windows/rendering read it, not the live objects
    // directly.
    void
    BuildClientSnapshot();  // client: receives snapshot/layout from the transport + player view
    void ApplyLayout(const Proto::SystemLayout& lay);  // client: accept the layout of a new system
    void ApplyLayoutDelta(const Proto::LayoutDelta& d);  // client: the system changed (#38)
    std::unique_ptr<Entity>
            MakeProxyFromLayout(const Proto::EntityLayout& el);  // proxy from layout
    Entity* FindEntityById(int id) const;  // live entity in the active system by id
    void ReconcileClientWorld();  // builds/updates the client's proxy entities from layout+snapshot
    void
    ApplyTradeAcks(const Proto::Snapshot& s);  // net: credit revenue from the server's sale acks
    void BuildNetworkBeams();  // net: combat beams from the snapshot (server computes combat)

    Station* StationById(int id) const;             // station by stable id (for missions)
    void     FlashMessage(const std::string& msg);  // short HUD notification

    int screenWidth_ = 1280;
    int screenHeight_ = 720;

    // Galaxy index: system names, security, map positions and gate links. The server sends
    // it as a "universe" message at login, before the first layout, and again whenever it
    // changes (#206) -- a generated region exists only on the server, so the client's own
    // data/universe.json would not know it. Empty until that message arrives; everything
    // that reads it must cope. GalaxyState carries only per-system statistics.
    WorldLoader::Universe universe_;

    Entity* selected_ = nullptr;

    // The client's PREDICTION of the player ship. The authoritative one lives on the
    // server; this copy is stepped with the same Sim::StepPlayerShip and corrected by
    // every snapshot, then unacknowledged inputs are replayed on top of it.
    std::unique_ptr<Ship> playerShip_;
    Player                player_;
    MissionSystem         missions_;

    std::string dataDir_;  // folder with world data (universe/systems)
    // This machine's UI settings (the scale), and whether they may be written: not when the
    // file came from a newer build, nor when --uiscale set the scale for one run.
    std::string uiSettingsPath_;
    bool        uiSettingsWritable_ = true;
    bool        uiScaleLocked_ = false;

    float simAccumulator_ = 0.0f;  // accumulator for the fixed simulation step
    // The own ship as drawn: between its last two simulation steps, by how far the clock
    // has got towards the next one. Drawn at the latest step instead, it moved by zero
    // ticks in one frame and two in the next whenever frames and ticks drifted apart --
    // a couple of pixels of shudder at cruising speed, all the time.
    Vector2 shipPrevPos_ = { 0.0f, 0.0f };
    float   shipPrevHeading_ = 0.0f;
    Vector2 shipDrawPos_ = { 0.0f, 0.0f };
    float   shipDrawHeading_ = 0.0f;
    void    UpdateShipDrawPose();

    // The player's camera (#158). `camera_` is what it produced this frame -- kept as a plain
    // Camera2D because every draw call and every screen-to-world conversion already takes
    // one, and they have no business knowing how it was decided.
    Render::CameraRig rig_;
    Camera2D          camera_;
    bool    cameraSnap_ = true;  // snap instead of glide on the next frame (system change)
    Vector2 panLast_ = { 0.0f, 0.0f };
    void    DrawScaleBar();

    std::vector<BgStar> bgStars_;  // parallax-background stars
    // How far the sky has scrolled, in screen pixels, and where the camera was last frame.
    // Accumulated from the camera's motion on screen rather than computed from its world
    // position (#160): at a million units a position-based sky raced past whenever the
    // whole system was in view, and jumped whenever the zoom changed.
    Vector2    skyScroll_ = { 0.0f, 0.0f };
    Vector2    skyLastTarget_ = { 0.0f, 0.0f };
    bool       skyPrimed_ = false;
    Vector2    startWarpTarget_ = { 0.0f, 0.0f };
    int        startWarpFrames_ = -1;  // counts down to the --warp order; -1 when there is none
    WorldClock worldClock_;            // the server's clock, eased (#192)

    // --shot: where the frame goes (empty when not shooting), and how many frames are left.
    std::string     shotPath_;
    int             shotFrames_ = 60;
    RenderTexture2D shotTarget_ = {};
    bool            shotTreated_ = false;  // --treated
    bool            hudHidden_ = false;    // --nohud
    int             perfFrames_ = -1;      // --perf: frames left to measure; -1 when not
    int             perfTotal_ = 0;        // ...out of this many
    bool            perfOverlay_ = false;  // F9: the frame's cost on screen
    double          perfRolled_ = 0.0;     // when the overlay last closed a window
    void            DrawPerfOverlay();

    // How the world is presented (#35): the generated look is the only one (#123). Glyphs
    // live on in the sensor screen, which draws its own grid rather than through a backend.
    Render::ShapeBackend shapeBackend_;

    // The screen treatment (#120). The world goes through it; the HUD does too only if the
    // player says so, because it carries numbers they fly by. F10 opens its settings, and
    // closing them writes the file -- a look someone tuned and lost on exit is a look they
    // will not tune twice.
    // The shaders a material names (#121). Shared by the backends; null on a machine
    // whose driver refused them, which draws everything the way it drew before.
    Render::MaterialLibrary materials_;

    Render::Treatment treatment_;

    // Where the object the ship is holding station on (#157) currently is, from the
    // client's own proxies -- or null if it is holding station on nothing, or on something
    // this client cannot see. Interpolated, so it lags the server's answer by the render
    // delay; that is what the comment on StepPlayerShip is about. Its velocity comes from
    // the snapshots, for a follow (#298).
    const Sim::HoldTarget* HoldTarget() const;
    // Standing orders: hold station on an object at a distance until released (#157).
    void                    OrderHold(int mode, int targetId, float range);
    void                    ReleaseHold();
    mutable Sim::HoldTarget holdTarget_;
    void                    DrawTreatmentSettings();
    Rectangle               TreatmentPanelRect() const;

    // Radar state: zoom and absolute view center (does not follow the player).
    float   radarZoom_ = 1.0f;
    Vector2 radarCenter_ = { 0.0f, 0.0f };  // world point at the radar center
    bool    radarInit_ = false;             // center set to the player on first display
    bool    radarDragging_ = false;
    Vector2 radarDragLast_ = { 0.0f, 0.0f };
    Vector2 radarPressPos_ = { 0.0f, 0.0f };
    bool    radarDragMoved_ = false;

    GameMode mode_ = GameMode::Flying;
    Station* dockedStation_ = nullptr;  // station we're docked to
    Station* nearbyStation_ = nullptr;  // station within docking range (for the prompt)

    // Ore mining (extraction is server-side; here only the field for the beam render).
    AsteroidField* miningBeamField_ = nullptr;  // field currently being mined

    // The hangar is server-owned (#5): these read the snapshot rather than remembering
    // anything, so a purchase that the server refused does not show as a ship you have.
    int  CurrentShipIndex() const { return snapshot_.player.shipIndex; }
    bool OwnsShip(int catalogIndex) const;

    Proto::Command  cmd_;       // client: the player's intent this frame (from input)
    Proto::Snapshot snapshot_;  // snapshot of the player's system for rendering/UI (M4c)
    // Client↔server transport: netConn_ is the TCP link to the econserver host, and
    // clientLink_ is the end the client sends commands on and receives snapshots/layout from.
    std::unique_ptr<Net::TcpConnection> netConn_;
    bool        protocolMismatchReported_ = false;  // say it once, not every frame
    ITransport* clientLink_ = nullptr;
    // Client prediction/reconciliation of the own ship (M4e, per Gambetta):
    // inputs are numbered and kept until the server acks them, so unacked ones can be
    // replayed over the authoritative state (without snapping backward).
    unsigned int                inputSeq_ = 0;
    std::vector<Proto::Command> pendingInputs_;
    // Client-side proxy world entities: rendered instead of the server's live objects.
    // Statics are built from the received layout, dynamics (NPCs) from the snapshot; positions
    // are updated from the snapshot by id (M4d-3c). There is no local simulation to clone.
    std::vector<std::unique_ptr<Entity>> clientWorld_;
    // The static layer of the current system: the layout sent on entry, kept current by
    // the deltas that follow it (#38).
    Proto::LayoutMirror layout_;
    Proto::GalaxyState  galaxyState_;  // net: per-system stats for the galaxy map (M4e-3c)
    // Buffer of timestamped snapshots for interpolating non-own entities (M4e-2):
    // we draw them "in the past" (render delay), interpolating between two snapshots.
    struct InterpSnap
    {
        double                             t;
        std::vector<Proto::EntitySnapshot> ents;
    };
    std::deque<InterpSnap> snapBuffer_;

    // Combat (damage/cooldown are server-side; here only the weapon toggle and beam render).
    bool              weaponOn_ = false;
    std::vector<Beam> beams_;  // weapon beams for the current frame

    // UI (#297). Every window, screen and popup is on the desk, which decides which is in
    // front, who the mouse belongs to this frame and what Esc closes. These are their ids;
    // the ones on the menu bar are registered in the bar's order.
    static constexpr const char* WIN_STATUS = "status";
    static constexpr const char* WIN_TARGET = "target";
    static constexpr const char* WIN_OVERVIEW = "overview";
    static constexpr const char* WIN_RADAR = "radar";
    static constexpr const char* WIN_MISSIONS = "missions";
    static constexpr const char* WIN_MAP = "map";        // the full-screen galaxy map
    static constexpr const char* WIN_SENSOR = "sensor";  // the sensor screen (#123)
    static constexpr const char* WIN_SETTINGS = "settings";
    static constexpr const char* WIN_STATION = "station";  // the docked screen
    static constexpr const char* WIN_MENUBAR = "menubar";
    static constexpr const char* WIN_CONTEXT = "context";  // the right-click menu
    static constexpr const char* WIN_RANGE = "range";      // the range picker (#298)
    static constexpr const char* WIN_LOOK = "look";        // F10's treatment panel (#120)
    Ui::Desk                     desk_;
    // The status window, laid out by Ui::Layout rather than by hand (#297): the first one.
    Ui::Layout statusLayout_;
    // The overview's tab and sort (#157), kept across frames and windows being reopened.
    Overview::Filter overviewFilter_ = Overview::Filter::All;
    Overview::Sort   overviewSort_ = Overview::Sort::Distance;

    std::string pilotName_;       // the account this client logged in as
    bool        naming_ = false;  // the name field is open (#145)
    std::string nameBuf_;

    // The sensor screen (#123): V opens it, the wheel steps its range. It covers the world
    // view and the windows while open, and the flight keys keep working, because flying by
    // instruments is the point of having them. The picture and where it was drawn are kept
    // for the next frame's clicks.
    float           sensorRange_ = Sensor::DefaultRange();
    Sensor::Picture sensorPicture_;
    Vector2         sensorOrigin_ = { 0.0f, 0.0f };  // screen point of cell (0, 0)'s corner
    float           sensorCellPx_ = 0.0f;
    Vector2         sensorCentre_ = { 0.0f, 0.0f };  // world point the picture is centred on

    // Short notification (saved/loaded).
    std::string flashMsg_;
    float       flashTimer_ = 0.0f;
    // Map interactivity: zoom and view center (in mapPos coordinates).
    float   galaxyZoom_ = 1.0f;
    Vector2 galaxyCenter_ = { 0.0f, 0.0f };
    bool    galaxyInit_ = false;
    bool    galaxyDragging_ = false;
    Vector2 galaxyDragLast_ = { 0.0f, 0.0f };

    ContextMenu contextMenu_;  // right-click action menu on an object
    RangePicker rangePicker_;  // a hold at a distance of the player's choosing (#298)
};
