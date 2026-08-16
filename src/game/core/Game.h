#pragma once

#include "raylib.h"
#include "entities/Entity.h"
#include "entities/Ship.h"
#include "missions/MissionSystem.h"
#include "core/WorldLoader.h"
#include "sim/Protocol.h"
#include "net/Transport.h"
#include "net/Tcp.h"
#include "render/GlyphBackend.h"
#include "player/Player.h"
#include <string>
#include "ui/Window.h"
#include "ui/ContextMenu.h"
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
    // match the server's SIM_DT or one input stops meaning one tick.
    static constexpr float SIM_DT = 1.0f / 60.0f;

    // Takes an already-established connection to an econserver host (main() dials it).
    // There is no offline mode: without a connection there is no world to render.
    explicit Game(std::unique_ptr<Net::TcpConnection> conn);
    ~Game();

    void Run();

private:
    void HandleInput(float dt);  // client: input → player command (cmd_)
    void DrawWorld();
    void DrawStarfield();  // parallax star background (screen coordinates)
    void DrawHud();
    void DrawStationScreen();
    void DrawMissionBoard(int x, int y, int w);  // mission board at the station

    void SetupWindows();                         // creates the UI windows
    void ResetWindowLayout();                    // arranges windows at their default positions
    bool HandleWindows();                        // window input; true — mouse captured by the UI
    bool HandleMenuBar();                        // menu bar input; true — mouse over the bar
    void DrawMenuBar();                          // vertical menu bar on the left
    void ApplyResolution(int w, int h);          // changes the window size
    void DrawSettingsContent(Rectangle area);    // contents of the settings window
    void OpenContextMenu(Entity* target);        // right-click action menu on an object
    void OpenContextMenuAt(Vector2 worldPoint);  // right-click menu on empty space
    // Navigation orders (client → command; applied by the server in StepPlayerShip).
    void OrderAutopilot(Vector2 target, float stopDist);  // fly to a point
    void OrderWarp(Vector2 target, float dropDist);       // warp to a point
    void DrawStatusContent(Rectangle area);               // contents of the status window
    void DrawTargetContent(Rectangle area);               // contents of the selected-target window
    void DrawOverviewContent(Rectangle area);             // list of objects in the system
    void DrawRadarContent(Rectangle area);                // system radar minimap
    void DrawMissionsContent(Rectangle area);             // log of active missions
    void DrawGalaxyMap();                                 // full-screen star map

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

    // Galaxy index (data/universe.json), loaded locally: system names, security, map
    // positions and gate links. GalaxyState carries only per-system statistics, so the
    // client still needs this file to label the map and the HUD.
    WorldLoader::Universe universe_;

    Entity* selected_ = nullptr;

    // The client's PREDICTION of the player ship. The authoritative one lives on the
    // server; this copy is stepped with the same Sim::StepPlayerShip and corrected by
    // every snapshot, then unacknowledged inputs are replayed on top of it.
    std::unique_ptr<Ship> playerShip_;
    Player                player_;
    MissionSystem         missions_;

    std::string dataDir_;  // folder with world data (universe/systems)

    float simAccumulator_ = 0.0f;  // accumulator for the fixed simulation step

    Camera2D camera_;
    bool     cameraSnap_ = true;  // snap instead of lerp on the next frame (system change)

    std::vector<BgStar> bgStars_;  // parallax-background stars

    // How the world is presented (#35). Glyphs are the game's look (#36); shapes remain
    // reachable with F2, as the alternative backend the sprite path hangs off.
    Render::GlyphBackend glyphBackend_;
    Render::ShapeBackend shapeBackend_;
    Render::IBackend*    backend_ = &glyphBackend_;

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

    int               currentShipIndex_ = 0;  // index of the current ship in the catalog
    std::vector<bool> ownedShips_;            // which ships the player owns

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
    // are updated from the snapshot by id (M4d-3c). The client does not clone the live sim_.
    std::vector<std::unique_ptr<Entity>> clientWorld_;
    std::map<int, Proto::EntityLayout>   layoutById_;  // static layout of the current system by id
    Proto::GalaxyState galaxyState_;  // net: per-system stats for the galaxy map (M4e-3c)
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

    // UI. The order in windows_ sets the z-order (last — on top).
    std::vector<std::unique_ptr<Window>> windows_;
    Window*                              statusWin_ = nullptr;
    Window*                              targetWin_ = nullptr;
    Window*                              overviewWin_ = nullptr;
    Window*                              radarWin_ = nullptr;
    Window*                              missionsWin_ = nullptr;
    Window*                              settingsWin_ = nullptr;

    bool galaxyMapOpen_ = false;  // full-screen galaxy map

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
};
