// Game -- the galaxy map: the full-screen star map, and the routes planned on it (#432).
//
// The itinerary is planned here with the same Route::FindVia the server's route order uses,
// over the same index, and flown by that order: the map never moves the ship or uses a gate
// itself. It is kept per server and pilot in ui_navigation.json beside the executable -- a
// preference of this machine's, like ui_layout.json, never part of the account.
//
// Part of the Game class; see Game.cpp.

#include "core/Game.h"
#include "core/Archetype.h"
#include "core/ShipDesign.h"
#include "economy/Resource.h"
#include "sim/Observation.h"
#include "sim/Orders.h"
#include "sim/SaveFile.h"
#include "ui/Desk.h"
#include "ui/Input.h"
#include "ui/Paint.h"
#include "ui/UiTheme.h"
#include "ui/Units.h"
#include "raymath.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The menu bar's width, as GameHud.cpp has it: the map starts to its right.
static float MenuBarW()
{
    return Ui::Px(Ui::CurrentTheme().metrics.menuBar);
}

namespace
{
constexpr int NAVIGATION_FILE_VERSION = 1;

std::string NavigationPath()
{
    return std::string(GetApplicationDirectory()) + "ui_navigation.json";
}

// The file as this build writes it, or false: a file of another shape is left alone.
bool NavigationFileOk(const nlohmann::json& j)
{
    return j.is_object() && j.value("version", 0) == NAVIGATION_FILE_VERSION &&
           j.contains("pilots") && j["pilots"].is_object();
}
}  // namespace

std::string Game::MapSystemName(const std::string& id) const
{
    for (const auto& system : universe_.systems)
        if (system.id == id)
            return system.name;
    return id;
}

// What the ship weighs, for the mouths and holes a route crosses: the design it flies, as the
// server weighs it (Simulation::ShipMass). Worked out once per design -- deriving a design is
// costly -- and only when the index has a crossing that limits mass at all.
double Game::NavigationMass() const
{
    bool limited = false;
    for (const auto& l : universe_.links)
        limited = limited || l.maxMass > 0.0f;
    if (!limited)
        return 0.0;
    std::string              design = playerShip_->GetStats().design;
    const Proto::PlayerView& p = snapshot_.player;
    for (size_t i = 0; i < p.ownedShips.size() && i < p.shipDesigns.size(); ++i)
        if (p.ownedShips[i] == p.shipIndex)
            design = p.shipDesigns[i];
    if (design != navMassDesign_)
    {
        const Ships::Design* d = Archetypes::FindDesign(design);
        Ships::Stats         stats;
        std::string          why;
        navMass_ = d != nullptr && Ships::Derive(Archetypes::ShipCatalogue(), *d, stats, why)
                       ? stats.mass
                       : 0.0;
        navMassDesign_ = design;
    }
    return navMass_;
}

void Game::LoadNavigation(const std::string& server)
{
    navigationKey_ = server + "/" + pilotName_;
    std::ifstream file(NavigationPath());
    if (!file)
    {
        navigationWritable_ = !std::filesystem::exists(NavigationPath());
        return;
    }
    const nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    navigationWritable_ = NavigationFileOk(j);
    if (navigationWritable_ && j["pilots"].contains(navigationKey_))
        navigationWritable_ = navigation_.FromJson(j["pilots"][navigationKey_]);
    if (!navigationWritable_)
        mapNotice_ = "The saved route could not be read; that file is left as it is.";
}

void Game::SaveNavigation()
{
    if (!navigationWritable_ || navigationKey_.empty())
        return;
    nlohmann::json j = { { "version", NAVIGATION_FILE_VERSION },
                         { "pilots", nlohmann::json::object() } };
    {
        std::ifstream file(NavigationPath());
        if (file)
        {
            j = nlohmann::json::parse(file, nullptr, false);
            if (!NavigationFileOk(j))
            {
                navigationWritable_ = false;
                FlashMessage("The saved routes file changed shape; it is not overwritten.");
                return;
            }
        }
    }
    j["pilots"][navigationKey_] = navigation_.ToJson();
    if (!Save::WriteFileAtomic(NavigationPath(), j.dump(2) + "\n", 1))
        FlashMessage("Could not save the route.");
}

// A changed itinerary that the autopilot was flying is flown as changed: the order is sent
// again, which replaces the running one on the server.
void Game::MapRouteChanged(bool wasActive)
{
    mapNotice_.clear();
    if (wasActive)
        StartMapRoute();
    SaveNavigation();
}

bool Game::PlotMapRoute(const std::string& system)
{
    std::string why;
    const bool  active = navigation_.Active();
    if (!navigation_.SetDestination(universe_, snapshot_.systemId, system, NavigationMass(),
                                    snapshot_.time, why))
    {
        mapNotice_ = "No route: " + why;
        FlashMessage(mapNotice_);
        return false;
    }
    mapTab_ = 0;
    MapRouteChanged(active);
    return true;
}

bool Game::AddMapWaypoint(const std::string& system)
{
    std::string why;
    const bool  active = navigation_.Active();
    if (!navigation_.AddWaypoint(universe_, snapshot_.systemId, system, NavigationMass(),
                                 snapshot_.time, why))
    {
        mapNotice_ = "No route: " + why;
        FlashMessage(mapNotice_);
        return false;
    }
    mapTab_ = 0;
    MapRouteChanged(active);
    return true;
}

void Game::StartMapRoute()
{
    if (navigation_.Destination().empty())
        return;
    std::string why;
    if (!navigation_.Refresh(universe_, snapshot_.systemId, NavigationMass(), snapshot_.time, why))
    {
        mapNotice_ = "Cannot travel: " + why;
        FlashMessage(mapNotice_);
        SaveNavigation();
        return;
    }
    if (navigation_.Waypoints().empty() && snapshot_.systemId == navigation_.Destination())
    {
        FlashMessage("You are already there.");
        return;
    }
    if (mode_ == GameMode::Docked)
        Undock();  // its own command, sent now: the order below goes out after it
    // One order for the whole journey, on the next numbered command; the server plans each
    // hop again as it goes, by the same rule and over the same index.
    cmd_.navMode = 0;
    cmd_.thrust = cmd_.brake = false;
    cmd_.turn = 0.0f;
    cmd_.abortOrder = false;
    cmd_.orderKind = (int)Orders::Kind::Route;
    cmd_.orderDestSystem = navigation_.Destination();
    cmd_.orderWaypoints = navigation_.Waypoints();
    cmd_.orderPreference = (int)navigation_.Preference();
    cmd_.orderWarp = true;
    navigation_.Begin(snapshot_.player.orderId);
    mapNotice_.clear();
    SaveNavigation();
}

void Game::StopMapRoute()
{
    cmd_.orderKind = 0;
    cmd_.abortOrder = true;
    navigation_.Pause("Stopped. The route is kept: travel to go on.");
    SaveNavigation();
}

void Game::FocusMapSystem(const std::string& id)
{
    for (const auto& system : universe_.systems)
        if (system.id == id)
        {
            galaxyCenter_ = system.mapPos;
            galaxyInit_ = true;
            galaxyDragging_ = false;
            return;
        }
    mapNotice_ = MapSystemName(id) + " is not on the map now.";
}

// A star's menu (#432), the same popup as the world's: what can be done about that system.
void Game::OpenMapMenu(const std::string& system, Vector2 at)
{
    std::vector<ContextMenu::Item> items;
    if (system != snapshot_.systemId)
    {
        items.push_back({ "Plot route", [this, system] { PlotMapRoute(system); } });
        items.push_back(
            { "Travel", [this, system]
              {
                  if ((navigation_.Destination() == system && navigation_.Waypoints().empty()) ||
                      PlotMapRoute(system))
                      StartMapRoute();
              } });
        if (!navigation_.Destination().empty() && navigation_.Destination() != system)
            items.push_back({ "Add waypoint", [this, system] { AddMapWaypoint(system); } });
    }
    items.push_back({ navigation_.Favorite(system) ? "Remove from favourites" : "Add to favourites",
                      [this, system]
                      {
                          navigation_.ToggleFavorite(system);
                          SaveNavigation();
                      } });
    contextMenu_.Open(at, std::move(items), MapSystemName(system));
}

// The map's sidebar: the route, the favourites, and the legend with the news.
void Game::DrawMapSidebar(Ui::Layout& L)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    const TextStyle  note = TextStyle::Small().Wrap();
    L.Tabs("mapTabs", { "Route", "Favourites", "Legend" }, mapTab_);
    if (!mapNotice_.empty())
        L.Text(mapNotice_, TextStyle::Small().Tint(t.colors.warn).Wrap());

    if (mapTab_ == 0)
    {
        // How the hops are weighed, as EVE has it; a route already plotted is planned again.
        int pref = (int)navigation_.Preference();
        if (L.Tabs("routePref", { "Shortest", "Safer", "Less secure" }, pref))
        {
            std::string why;
            const bool  active = navigation_.Active();
            if (navigation_.SetPreference(universe_, snapshot_.systemId, (Route::Preference)pref,
                                          NavigationMass(), snapshot_.time, why))
                MapRouteChanged(active);
            else
                mapNotice_ = "No route: " + why;
        }
        if (navigation_.Destination().empty())
        {
            L.Text("No destination", TextStyle::Title());
            L.Text("Right-click a star: plot a route to it, or travel there at once. Add "
                   "waypoints the same way to pass through other systems on the way.",
                   TextStyle::Body().Wrap());
            L.Text("Routes keep to the gates you know: charted systems and the ones a gate "
                   "from them leads to.",
                   note);
            return;
        }
        const std::vector<std::string>& path = navigation_.Path();
        const size_t                    at = navigation_.Reached();
        const size_t                    left = path.size() > at + 1 ? path.size() - 1 - at : 0;
        L.Text(MapSystemName(navigation_.Destination()), TextStyle::Title().Wrap());
        L.Field("Status", navigation_.Label(),
                navigation_.State() == Navigation::Status::Blocked ? t.colors.warn
                                                                   : t.colors.accent);
        L.Field("Jumps left", std::to_string(left), t.colors.text);
        if (!navigation_.Detail().empty())
            L.Text(navigation_.Detail(), note);
        L.Row(Box().GrowX().Gap(t.metrics.rowGap),
              [&]
              {
                  if (navigation_.Active())
                  {
                      if (L.Button("stopRoute", "Stop"))
                          StopMapRoute();
                  }
                  else if (L.Button("startRoute", "Travel", true, left > 0))
                      StartMapRoute();
                  if (L.Button("clearRoute", "Clear"))
                  {
                      if (navigation_.Active())
                          StopMapRoute();
                      navigation_.Clear();
                      SaveNavigation();
                  }
              });
        if (!navigation_.Waypoints().empty())
        {
            L.Text("WAYPOINTS", TextStyle::Label());
            const std::vector<std::string> waypoints = navigation_.Waypoints();
            for (size_t i = 0; i < waypoints.size(); ++i)
                L.Row(
                    Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
                    [&]
                    {
                        L.Column(Box().GrowX(),
                                 [&]
                                 {
                                     L.Text(std::to_string(i + 1) + ".  " +
                                                MapSystemName(waypoints[i]),
                                            TextStyle::Body().Wrap());
                                 });
                        if (L.Button("dropWaypoint" + std::to_string(i), "Remove"))
                        {
                            std::string why;
                            const bool  active = navigation_.Active();
                            if (navigation_.RemoveWaypoint(universe_, snapshot_.systemId, i,
                                                           NavigationMass(), snapshot_.time, why))
                                MapRouteChanged(active);
                            else
                                mapNotice_ = "No route: " + why;
                        }
                    });
        }
        L.Divider();
        L.Scroll(Box().Grow().Id("routeStops").Gap(t.metrics.gap),
                 [&]
                 {
                     for (size_t i = 0; i < path.size(); ++i)
                     {
                         const std::string mark = i < at    ? "passed  "
                                                  : i == at ? "here  "
                                                            : std::to_string(i - at) + ".  ";
                         L.Text(mark + MapSystemName(path[i]), TextStyle::Body()
                                                                   .Tint(i == at  ? t.colors.accent
                                                                         : i < at ? t.colors.dim
                                                                                  : t.colors.text)
                                                                   .Wrap());
                     }
                 });
    }
    else if (mapTab_ == 1)
    {
        L.Text("Favourites", TextStyle::Title());
        const std::vector<std::string> favourites = navigation_.Favorites();
        L.Scroll(Box().Grow().Id("favourites").Gap(t.metrics.rowGap),
                 [&]
                 {
                     if (favourites.empty())
                         L.Text("Right-click a star and add it to your favourites.",
                                TextStyle::Body().Wrap());
                     for (size_t i = 0; i < favourites.size(); ++i)
                     {
                         const std::string& id = favourites[i];
                         L.Row(Box()
                                   .GrowX()
                                   .Gap(t.metrics.rowGap)
                                   .Align(Ui::Align::Start, Ui::Align::Center),
                               [&]
                               {
                                   L.Column(
                                       Box().GrowX(), [&]
                                       { L.Text(MapSystemName(id), TextStyle::Body().Wrap()); });
                                   if (L.Button("showFav" + std::to_string(i), "Show"))
                                       FocusMapSystem(id);
                                   if (L.Button("plotFav" + std::to_string(i), "Plot", false,
                                                id != snapshot_.systemId))
                                       PlotMapRoute(id);
                               });
                     }
                 });
    }
    else
    {
        L.Text("LEGEND", TextStyle::Label());
        L.Text("Filled: charted. A ring alone: uncharted -- a gate leads there, and nothing "
               "more is known.",
               note);
        L.Text("Charted: the public charts, and what you and your corporation have found. "
               "Your finds are nobody else's.",
               note);
        L.Text("The ring's colour is who holds the system; a triangle over it, that they "
               "would shoot you.",
               note);
        L.Text("A second, outer ring: your corporation's front is open there -- another "
               "side holds it, and you or nobody hold a system next door, so a claim may be "
               "laid or a structure besieged. Behind the line nobody may, until a border falls. "
               "Guns are never bound by it; a wormhole you found opens no front.",
               note);
        L.Text("sec, pir, econ: security, pirates, prosperity.", note);
        L.Text("A dashed line: a wormhole your scans found. Only you see it, and it closes.", note);
        L.Text("Your route: bright ahead, dim behind. A ring round the destination, a number "
               "over each waypoint, a diamond on a favourite.",
               note);
        L.Divider();
        // Galactic news (system captures, reconquests) from the server's galaxy snapshot,
        // newest first, wrapped and scrolled.
        const std::vector<std::string>& news = galaxyState_.events;
        L.Text("GALACTIC NEWS", TextStyle::Label());
        L.Scroll(Box().Grow().Id("news").Gap(t.metrics.gap),
                 [&]
                 {
                     if (news.empty())
                         L.Text("Nothing has changed hands yet.",
                                TextStyle::Small().Tint(t.colors.dim).Wrap());
                     for (size_t i = news.size(); i-- > 0;)
                         L.Text(news[i], TextStyle::Small().Wrap());
                 });
    }
}

// Full-screen galaxy star map (EVE-style): dimmed background, systems as nodes by mapPos,
// gate links as lines, the current system highlighted. The map itself is a picture drawn by
// hand; around it, the heading, the legend and the news are laid out (#297), and the
// picture takes whatever room they leave.
void Game::DrawGalaxyMap()
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    // Opaque, as the sensor screen is (#366): the shade alone let a bright ship sprite show
    // through the middle of the map, a second picture of a different place.
    Color ground = t.colors.shade;
    ground.a = 255;
    DrawRectangle(0, 0, screenWidth_, screenHeight_, ground);

    Ui::Layout& L = mapLayout_;
    L.Begin(desk_.SurfaceFrame(
        WIN_MAP, { MenuBarW(), 0.0f, screenWidth_ - MenuBarW(), (float)screenHeight_ }));
    L.Column(
        Box().Grow().Pad(t.metrics.padding * 2.0f).Gap(t.metrics.gap),
        [&]
        {
            L.Row(Box().GrowX().Gap(t.metrics.padding).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("GALAXY MAP", TextStyle::Heading().Tint(t.colors.accent));
                      L.Text("right-click a star  ·  drag to pan  ·  wheel to zoom  ·  [G] / "
                             "[Esc] close",
                             TextStyle::Small().Tint(t.colors.dim));
                  });
            L.Row(Box().Grow().Gap(t.metrics.padding * 2.0f),
                  [&]
                  {
                      L.Row(Box().Id("graph").Grow(), [] {});
                      L.Column(Box()
                                   .Width(Size::Fixed(std::max(280.0f, t.metrics.sidePanel)))
                                   .Height(Size::Grow())
                                   .Pad(t.metrics.padding)
                                   .Gap(t.metrics.gap)
                                   .Fill(t.colors.panel)
                                   .Border(t.colors.border, t.metrics.border)
                                   .ThemeCorners(),
                               [&] { DrawMapSidebar(L); });
                  });
        });
    L.End();
    // The picture's room; the furniture around it is drawn over the shade.
    const Rectangle area = L.BoxOf("graph");
    L.Draw();

    // The picture's own text, at the theme's sizes and the player's scale like every window
    // (#366): a system's name a step above body text, what is known about it at the label
    // size, the naming hint at body size. Hand-placed pixel sizes stayed at 16 and 12 on a
    // screen where everything else had grown, and read as specks. Every offset around a
    // node is in the same units, so the lines under a name move apart as the text grows.
    const float k = Ui::Scale();
    auto        u = [k](float units) { return units * k; };
    const int   namePx = (int)Ui::Px(t.fontSize.body + 3.0f);
    const int   notePx = (int)Ui::Px(t.fontSize.label + 1.0f);
    const int   hintPx = (int)Ui::Px(t.fontSize.body + 1.0f);
    const float line = (float)notePx + u(3.0f);  // one line of notes under the name

    const std::vector<WorldLoader::SystemInfo>& systems = universe_.systems;
    if (systems.empty() || area.width <= 0.0f || area.height <= 0.0f)
    {
        const char* none = "No galaxy data";
        Ui::Text(none, (int)(area.x + (area.width - Ui::TextWidth(none, namePx)) * 0.5f),
                 (int)(area.y + area.height * 0.5f), namePx, t.colors.dim);
        return;
    }
    Vector2 nameAt{ -1.0f, -1.0f };  // where the name field goes, while it is being typed in

    // Extents in mapPos.
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& s : systems)
    {
        minX = fminf(minX, s.mapPos.x);
        minY = fminf(minY, s.mapPos.y);
        maxX = fmaxf(maxX, s.mapPos.x);
        maxY = fmaxf(maxY, s.mapPos.y);
    }
    float spanX = fmaxf(1.0f, maxX - minX), spanY = fmaxf(1.0f, maxY - minY);
    float pad = u(80.0f);
    float baseScale = fminf((area.width - 2 * pad) / spanX, (area.height - 2 * pad) / spanY);

    // On first display: where you are, close enough to read your own civilisation and the
    // frontier beside it (#342 S2). The whole galaxy -- several centres thousands of map
    // units apart -- is a wheel away.
    constexpr float NEAR_SPAN = 1800.0f, MAX_ZOOM = 40.0f;
    if (!galaxyInit_)
    {
        galaxyCenter_ = { (minX + maxX) / 2.0f, (minY + maxY) / 2.0f };
        galaxyZoom_ = 1.0f;
        for (const auto& s : systems)
            if (s.id == snapshot_.systemId)
            {
                galaxyCenter_ = s.mapPos;
                galaxyZoom_ = Clamp(fmaxf(spanX, spanY) / NEAR_SPAN, 1.0f, MAX_ZOOM);
                // Better: everything within two jumps fills the picture, with room beside the
                // outermost nodes for their names (#366). A fixed span left the neighbourhood
                // a knot in the middle of an empty screen.
                std::vector<std::string> near = { s.id };
                for (int hop = 0; hop < 2; hop++)
                {
                    const size_t known = near.size();
                    for (const auto& l : universe_.links)
                        for (size_t i = 0; i < known; i++)
                        {
                            if (l.a == near[i] &&
                                std::find(near.begin(), near.end(), l.b) == near.end())
                                near.push_back(l.b);
                            else if (l.b == near[i] &&
                                     std::find(near.begin(), near.end(), l.a) == near.end())
                                near.push_back(l.a);
                        }
                }
                float reachX = 0.0f, reachY = 0.0f;
                for (const auto& o : systems)
                    if (std::find(near.begin(), near.end(), o.id) != near.end())
                    {
                        reachX = fmaxf(reachX, fabsf(o.mapPos.x - s.mapPos.x));
                        reachY = fmaxf(reachY, fabsf(o.mapPos.y - s.mapPos.y));
                    }
                const float roomX = area.width * 0.5f - u(150.0f);
                const float roomY = area.height * 0.5f - u(40.0f);
                if (reachX + reachY > 0.0f && roomX > 0.0f && roomY > 0.0f && baseScale > 0.0f)
                {
                    const float fit = fminf(reachX > 0.0f ? roomX / reachX : 1e9f,
                                            reachY > 0.0f ? roomY / reachY : 1e9f);
                    galaxyZoom_ = Clamp(fit / baseScale, 1.0f, MAX_ZOOM);
                }
            }
        galaxyInit_ = true;
    }

    // Input: wheel zoom and drag-to-pan — like the radar. Drawn by the desk, which says
    // whether the map owns the mouse (#297).
    Vector2 m = GetMousePosition();
    bool    over = Ui::MouseOver(area);
    if (over)
    {
        float wheel = Ui::MouseWheel();
        if (wheel != 0.0f)
            galaxyZoom_ = Clamp(galaxyZoom_ * (1.0f + wheel * 0.12f), 0.3f, MAX_ZOOM);
    }

    float scale = baseScale * galaxyZoom_;

    if (over && Ui::MousePressed(MOUSE_BUTTON_LEFT))
    {
        galaxyDragging_ = true;
        galaxyDragLast_ = m;
    }
    if (galaxyDragging_)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            galaxyCenter_.x -= (m.x - galaxyDragLast_.x) / scale;
            galaxyCenter_.y -= (m.y - galaxyDragLast_.y) / scale;
            galaxyDragLast_ = m;
        }
        else
        {
            galaxyDragging_ = false;
        }
    }

    Vector2 c{ area.x + area.width / 2.0f, area.y + area.height / 2.0f };
    auto    toScreen = [&](Vector2 mp) -> Vector2
    { return { c.x + (mp.x - galaxyCenter_.x) * scale, c.y + (mp.y - galaxyCenter_.y) * scale }; };
    auto posById = [&](const std::string& id, Vector2& out) -> bool
    {
        for (const auto& s : systems)
            if (s.id == id)
            {
                out = toScreen(s.mapPos);
                return true;
            }
        return false;
    };

    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);

    // Gate links.
    for (const auto& l : universe_.links)
    {
        Vector2 a, b;
        if (!posById(l.a, a) || !posById(l.b, b))
            continue;
        if (l.kind != "hole")
        {
            DrawLineEx(a, b, 1.5f, Fade(Ui::PANEL_BORDER, 0.9f));
            continue;
        }
        // A roaming hole this pilot found (#342 S9): dashed, in the hole's own colour, and
        // only theirs -- nobody else's map has it.
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = sqrtf(dx * dx + dy * dy);
        const int   dashes = std::max(1, (int)(len / u(10.0f)));
        for (int k = 0; k < dashes; k += 2)
        {
            const float t0 = (float)k / dashes, t1 = (float)(k + 1) / dashes;
            DrawLineEx({ a.x + dx * t0, a.y + dy * t0 }, { a.x + dx * t1, a.y + dy * t1 }, 2.0f,
                       Color{ 120, 220, 190, 230 });
        }
    }

    // The itinerary (#432): bright ahead, dim where the ship has been, an arrow on each hop
    // to say which way it is flown.
    const std::vector<std::string>& route = navigation_.Path();
    for (size_t i = 1; i < route.size(); ++i)
    {
        Vector2 a, b;
        if (!posById(route[i - 1], a) || !posById(route[i], b))
            continue;
        const Color col =
            i <= navigation_.Reached() ? Fade(t.colors.accent, 0.3f) : t.colors.accent;
        const Vector2 dir = Vector2Normalize(Vector2Subtract(b, a));
        const Vector2 tip = Vector2Lerp(a, b, 0.6f);
        const Vector2 tail = Vector2Subtract(tip, Vector2Scale(dir, u(8.0f)));
        const Vector2 wing{ -dir.y * u(4.0f), dir.x * u(4.0f) };
        DrawLineEx(a, b, u(2.5f), col);
        DrawLineEx(tip, Vector2Add(tail, wing), u(2.0f), col);
        DrawLineEx(tip, Vector2Subtract(tail, wing), u(2.0f), col);
    }

    // A right click on a star opens its menu (#432): the nearest within reach of the cursor.
    std::string clickedSystem;
    if (over && Ui::MousePressed(MOUSE_BUTTON_RIGHT))
    {
        float nearest = u(14.0f);
        for (const auto& s : systems)
        {
            const float d = Vector2Distance(m, toScreen(s.mapPos));
            if (d <= nearest)
            {
                nearest = d;
                clickedSystem = s.id;
            }
        }
    }

    // Which system the player is in, according to the server. The client has no
    // simulation of its own to ask (#3).
    std::string activeSys = snapshot_.systemId;

    // How close each system's nearest neighbour is on screen. With a region beyond the
    // wormhole (#140) the map holds twenty systems, and three lines of text under each one
    // is a smear: a crowded system shows its details only while it is the one you are in
    // or the one under the cursor, and the wheel brings the rest back.
    auto nearestOnScreen = [&](const WorldLoader::SystemInfo& s)
    {
        float best = 1e9f;
        for (const auto& o : systems)
            if (&o != &s)
            {
                const float dx = (o.mapPos.x - s.mapPos.x) * scale;
                const float dy = (o.mapPos.y - s.mapPos.y) * scale;
                best = fminf(best, sqrtf(dx * dx + dy * dy));
            }
        return best;
    };

    // Labels are placed, not just drawn (#259, #313): the system you are in and the one under
    // the cursor claim their text first, and any other label that would land on a claimed
    // one gives up its details, then its name. Spacing alone did not keep a neighbour's name
    // off the five lines the current system says about itself.
    std::vector<const WorldLoader::SystemInfo*> order;
    for (const auto& s : systems)
        order.push_back(&s);
    auto priority = [&](const WorldLoader::SystemInfo* s)
    {
        if (s->id == activeSys)
            return 0;
        return CheckCollisionPointCircle(m, toScreen(s->mapPos), u(12.0f)) ? 1 : 2;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](const WorldLoader::SystemInfo* a, const WorldLoader::SystemInfo* b)
                     { return priority(a) < priority(b); });
    std::vector<Rectangle> claimed;
    auto                   unclaimed = [&](Rectangle r)
    {
        for (const Rectangle& c : claimed)
            if (CheckCollisionRecs(r, c))
                return false;
        return true;
    };
    // Every node first: a label never runs across another system's disc (#366). A label
    // starts clear of its own node, so this costs it nothing.
    for (const auto& s : systems)
    {
        const Vector2 q = toScreen(s.mapPos);
        const float   r = u(11.0f);
        claimed.push_back({ q.x - r, q.y - r, 2.0f * r, 2.0f * r });
    }

    // System nodes.
    for (const WorldLoader::SystemInfo* sp : order)
    {
        const WorldLoader::SystemInfo& s = *sp;
        Vector2                        p = toScreen(s.mapPos);
        bool                           cur = (s.id == activeSys);
        const bool                     hovered = CheckCollisionPointCircle(m, p, u(12.0f));
        const float                    room = nearestOnScreen(s);
        bool                           detail = cur || hovered || room >= u(110.0f);
        bool                           named = cur || hovered || room >= u(45.0f);
        // The itinerary's marks (#432): a ring round the destination, a waypoint's number, and
        // a diamond for a favourite.
        if (s.id == navigation_.Destination())
            DrawCircleLines((int)p.x, (int)p.y, u(19.0f), t.colors.accent);
        for (size_t w = 0; w < navigation_.Waypoints().size(); ++w)
            if (navigation_.Waypoints()[w] == s.id)
            {
                DrawCircleLines((int)p.x, (int)p.y, u(19.0f), Fade(t.colors.accent, 0.7f));
                Ui::Text(std::to_string(w + 1).c_str(), (int)(p.x - u(4.0f)),
                         (int)(p.y - u(19.0f) - (float)notePx), notePx, t.colors.accent);
            }
        if (navigation_.Favorite(s.id))
            Ui::DrawMark({ p.x - u(12.0f), p.y - u(12.0f) }, u(3.5f), Ui::Mark::Diamond,
                         t.colors.accent);
        const float gapX = u(14.0f);  // from the node to its labels
        float       lx = p.x + gapX;  // where its labels start
        // The name is centred on the node; line `i` of what is known sits under it.
        const float nameY = p.y - (float)namePx * 0.5f;
        auto        lineY = [&](int i)
        { return (int)(p.y + (float)namePx * 0.5f + u(2.0f) + (float)(i - 1) * line); };
        {
            // What its labels would cover: the name line, and the lines under it.
            float nameW = (float)Ui::TextWidth(s.name.c_str(), namePx);
            if (!s.designation.empty())
                nameW += u(8.0f) + (float)Ui::TextWidth(s.designation.c_str(), notePx);
            const float     detailW = fmaxf(nameW, u(s.charted ? 190.0f : 80.0f));
            const int       lines = !s.charted ? 1
                                    : cur      ? (CanNameHere() ? 5 : 4)
                                               : (Obs::ChartsOf(s).empty() ? 2 : 3);
            const float     bottom = (float)lineY(lines) + line - nameY;
            const Rectangle nameBox{ p.x + gapX, nameY, nameW, (float)namePx + u(2.0f) };
            const Rectangle leftBox{ p.x - gapX - nameW, nameY, nameW, (float)namePx + u(2.0f) };
            const Rectangle detailBox{ p.x + gapX, nameY, detailW, bottom };
            // In order: everything to the right, the name to the right, the name to the left
            // of the node. A label that fits nowhere is left to the cursor.
            if (!cur && !hovered)
            {
                if (detail && !unclaimed(detailBox))
                    detail = false;
                if (named && !detail && !unclaimed(nameBox))
                {
                    if (unclaimed(leftBox))
                        lx = leftBox.x;
                    else
                        named = false;
                }
            }
            if (detail)
                claimed.push_back(detailBox);
            else if (named)
                claimed.push_back(lx < p.x ? leftBox : nameBox);
        }
        // Uncharted (#144): a gate from somewhere known leads there, and that is all anyone
        // knows -- an empty ring and a designation, no numbers.
        if (!s.charted)
        {
            DrawCircleLines((int)p.x, (int)p.y, u(7.0f), Ui::TEXT_DIM);
            if (named)
            {
                Ui::Text(s.name.c_str(), (int)lx, (int)nameY, namePx, Ui::TEXT_DIM);
                if (detail)
                    Ui::Text("uncharted", (int)lx, lineY(1), notePx, Ui::TEXT_DIM);
            }
            continue;
        }
        DrawCircleV(p, u(cur ? 10.0f : 7.0f), cur ? Ui::ACCENT : Ui::TEXT_DIM);
        if (cur)
            DrawCircleLines((int)p.x, (int)p.y, u(16.0f), Fade(Ui::ACCENT, 0.6f));
        if (named)
        {
            Ui::Text(s.name.c_str(), (int)lx, (int)nameY, namePx, cur ? Ui::ACCENT : Ui::TEXT);
            // A named system keeps its designation beside the name (#145), on its baseline.
            if (!s.designation.empty())
                Ui::Text(s.designation.c_str(),
                         (int)(lx + u(8.0f)) + Ui::TextWidth(s.name.c_str(), namePx),
                         (int)(nameY + (float)(namePx - notePx) * 0.75f), notePx, Ui::TEXT_DIM);
        }

        // Live summary: security/pirates/economy/controller, from the server's galaxy
        // snapshot (galaxyState_).
        bool                           haveStats = false;
        float                          security = 0.0f, prosperity = 0.0f;
        int                            pirates = 0;
        FactionId                      controller = Factions::Unaffiliated();
        const Proto::GalaxySystemStat* held = nullptr;  // a corporation's (#431 slice 5)
        bool                           front = false;   // your corporation's front (#431 D3)
        for (const Proto::GalaxySystemStat& g : galaxyState_.systems)
            if (g.id == s.id)
            {
                haveStats = true;
                security = g.security;
                pirates = g.pirates;
                prosperity = g.prosperity;
                controller = g.controller;
                held = g.holder.empty() ? nullptr : &g;
                front = g.front;
                break;
            }
        const float ring = u(cur ? 13.0f : 10.0f);
        // A system held by a faction that would shoot this pilot says so by shape as well as
        // by the ring's colour (#362): the hostile triangle over the ring, as on the radar.
        const bool hostileHeld = haveStats && HostileToPlayerFaction(controller);
        if (hostileHeld)
            Ui::DrawMark({ p.x, p.y - ring - u(6.0f) }, u(4.5f), Ui::Mark::Triangle,
                         t.standing.hostile);
        // Where your corporation's front is open (#431 D3) -- another side holds it, and you
        // hold, or nobody holds, a system next door -- a second ring outside the holder's: a
        // claim may be laid or a structure besieged there. Behind the line, none.
        if (front)
            DrawCircleLines((int)p.x, (int)p.y, ring + u(4.0f), Fade(t.colors.warn, 0.8f));
        if (haveStats && !detail)
            // Crowded: the controller only as the ring's colour.
            DrawCircleLines((int)p.x, (int)p.y, ring, Fade(FactionColor(controller), 0.7f));
        else if (haveStats)
        {
            Color secCol = security >= 0.7f   ? Color{ 120, 210, 130, 255 }
                           : security >= 0.4f ? GOLD
                                              : Color{ 230, 120, 60, 255 };
            Ui::Text(
                TextFormat("sec %.1f  pir %d  econ %.0f%%", security, pirates, prosperity * 100.0f),
                (int)lx, lineY(1), notePx, secCol);
            // Territory controller (L3) — in the faction's color; or the corporation that holds
            // it, with the tax its markets take (#431 slice 5).
            const std::string holder =
                held != nullptr ? TextFormat("held by %s [%s]  tax %.1f%%",
                                             held->holderName.c_str(), held->holderTicker.c_str(),
                                             held->FeeOr((float)Goods::Rules().fee) * 100.0f)
                                : FactionName(controller);
            if (held != nullptr)
                Ui::Text(holder.c_str(), (int)lx, lineY(2), notePx,
                         held->holder == corpView_.id ? t.standing.friendly : Ui::TEXT);
            else
                Ui::Text(holder.c_str(), (int)lx, lineY(2), notePx, FactionColor(controller));
            if (front)
                Ui::Text("  front open", (int)lx + Ui::TextWidth(holder.c_str(), notePx), lineY(2),
                         notePx, t.colors.warn);
            // The node ring is tinted with the controller's color.
            DrawCircleLines((int)p.x, (int)p.y, ring, Fade(FactionColor(controller), 0.7f));
        }
        if (cur)
            Ui::Text("you are here", (int)lx, lineY(3), notePx, Ui::TEXT_DIM);
        // ...and whose charts it is on, when they are not the public ones (#431).
        const char* whose = s.knownBy == "own"    ? "  -- your charts only"
                            : s.knownBy == "corp" ? "  -- your corporation's charts"
                                                  : "";
        // Whose charts made it public, and who got here first: "charted by Syndicate".
        const std::string charts = detail ? Obs::ChartsOf(s) : std::string();
        if (!charts.empty())
            Ui::Text(TextFormat("%s%s", charts.c_str(), whose), (int)lx, lineY(cur ? 4 : 3), notePx,
                     Ui::TEXT_DIM);
        if (cur && CanNameHere())
        {
            if (desk_.KeyboardFocus().Holds(WIN_MAP, "name"))
                nameAt = { lx, (float)lineY(4) + line * 0.5f };  // drawn after the clip
            else
                Ui::Text("[N] name this system", (int)lx, lineY(5), hintPx, Ui::ACCENT);
        }
    }

    EndScissorMode();

    if (!clickedSystem.empty())
        OpenMapMenu(clickedSystem, m);

    // The name field (#145): a Ui::Layout text field with the keyboard routed to it by the
    // desk (#297). Enter sends the name on the next numbered input; Esc, or a click
    // elsewhere, puts it away.
    if (nameAt.x >= 0.0f)
    {
        const Rectangle field{ nameAt.x, nameAt.y, Ui::Px(260.0f),
                               Ui::Px(t.metrics.buttonHeight + t.fontSize.small * 1.6f +
                                      t.metrics.rowGap) };
        Ui::Layout&     N = mapNameLayout_;
        N.Begin(desk_.SurfaceFrame(WIN_MAP, field));
        N.Column(Ui::Box().Grow().Gap(t.metrics.rowGap),
                 [&]
                 {
                     Ui::TextFieldOptions o;
                     o.maxLength = 24;
                     o.placeholder = "a name for this system";
                     const Ui::EditResult r = N.TextField("name", nameBuf_, o);
                     // The server checks it and says why not in the journal, which flashes
                     // here; a name it accepts comes back to everyone in the galaxy index. It
                     // rides on the next numbered input like any one-shot intent: a command of
                     // its own would be one more tick of movement the server steps and this
                     // client never predicted.
                     if (r.submitted && !nameBuf_.empty())
                         cmd_.nameSystem = nameBuf_;
                     N.Text("[Enter] name it   [Esc] cancel", Ui::TextStyle::Small());
                 });
        N.End();
        N.Draw();
    }
}
