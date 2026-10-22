// The world in the editor (#342 S8): which world regions are opened from -- the one a new
// server makes of a seed, or a saved one read from its seals -- the galaxy map of its
// skeleton, and packs: begun from nothing, exported from a region, edited on the index map.
// One translation unit of Editor (#17).
//
// The F5 levels are galaxy -> region -> system. The galaxy shows every slot the seed lays out,
// open or not, because a pin may wait on one that is not (an admin preparing a surprise for
// when the tide gets there) and a pack is woven into one; a click opens any slot's region on
// the region map, which opens any of its systems for editing, saved as a pin.

#include "Editor.h"

#include "core/Faction.h"
#include "gen/Pack.h"
#include "gen/Region.h"
#include "ui/Button.h"
#include "ui/UiTheme.h"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using nlohmann::json;

namespace
{
const float kPanelW = 300.0f;

// Instruments, as on the region map: what a slot is to the world, never a faction's paint.
const Color kWormhole = { 206, 116, 236, 255 };
const Color kCentre = { 255, 214, 120, 255 };
const Color kDrifted = { 120, 120, 140, 255 };
const Color kPack = { 120, 214, 140, 255 };  // the editor's "added": an admin's, not the seed's
const Color kPin = { 236, 180, 84, 255 };    // PIN_CHANGED

// The slot a pin is for, as Gen::ApplyPins reads it: its own, else its system's, else 0.
int PinSlot(const json& pin)
{
    if (pin.contains("slot") && pin["slot"].is_number_integer())
        return pin["slot"].get<int>();
    const int named =
        pin.contains("system") && pin["system"].is_string() ? Gen::SlotOf(pin["system"]) : -1;
    return named >= 0 ? named : 0;
}

std::string OwnerName(const std::string& id)
{
    FactionId f;
    return Factions::Find(id, f) ? Factions::Def(f).name : id;
}
}  // namespace

// ---- The world regions come from -------------------------------------------------------

Gen::WorldParams Editor::WorldParamsFor(uint64_t seed) const
{
    Gen::WorldParams p;
    p.seed = seed;
    p.centres = Saved(seed) ? world_.centres : Gen::DEFAULT_CENTRES;
    p.cap = Saved(seed) ? std::max(Gen::DEFAULT_REGION_CAP, world_.cap) : Gen::DEFAULT_REGION_CAP;
    p.owners = Factions::MajorIds();
    p.pirates = Factions::Id(Factions::Pirates());
    p.unaffiliated = Factions::Id(Factions::Unaffiliated());
    return p;
}

const std::vector<Gen::Region>& Editor::OpenRegions(uint64_t seed) const
{
    if (openSeed_ == seed && openSaved_ == Saved(seed) && !openRegions_.empty())
        return openRegions_;
    openSeed_ = seed;
    openSaved_ = Saved(seed);
    openRegions_.clear();
    const Gen::GalaxySkeleton g = Gen::GenerateGalaxySkeleton(WorldParamsFor(seed));
    if (!Saved(seed))
        openRegions_ = Gen::GenerateWorld(g);
    else
        for (const WorldSource::Entry& e : world_.log)
        {
            // As it was made, whatever rules made it: the seal, never the generator.
            std::ifstream     in(world_.sealDir + "/" + std::to_string(e.slot) + ".json");
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            Gen::Region       r;
            if (!in.is_open() || !Gen::ReadSealedRegion(text, seed, r))
            {
                TraceLog(LOG_WARNING, "World: region %d is not sealed in %s -- shown as made now",
                         e.slot, world_.sealDir.c_str());
                Gen::Region made;
                if (e.slot < (int)g.slots.size())
                {
                    Gen::RegionParams prm = Gen::SlotParams(g, e.slot);
                    if (prm.kind == Gen::SlotKind::Frontier)
                        prm.homeId = "home";
                    made = Gen::GenerateRegion(prm);
                }
                r = made;
            }
            openRegions_.push_back(std::move(r));
        }
    // Natives and an admin's factions fly their own flags (#342 S5, S7): the registry must
    // know them before a system of theirs is built.
    for (const Gen::Region& r : openRegions_)
    {
        Gen::RegisterNatives(r.natives);
        Gen::RegisterPackFactions(r.factions, r.relations);
    }
    return openRegions_;
}

bool Editor::WorldRegionAt(uint64_t seed, int slot, Gen::Region& out,
                           std::map<std::string, std::string>* names, std::string* homeName,
                           Vector2* homeMap) const
{
    const Gen::GalaxySkeleton       g = Gen::GenerateGalaxySkeleton(WorldParamsFor(seed));
    const std::vector<Gen::Region>& world = OpenRegions(seed);
    if (world.empty())
        return false;
    if (names != nullptr)
        for (const Gen::Region& r : world)
            for (const json& s : r.systems)
                (*names)[s.value("id", std::string())] = s.value("name", std::string());
    if (slot < 0)
        slot = world.front().slot;
    if (slot >= (int)g.slots.size())
        return false;
    const Gen::Region* found = nullptr;
    for (const Gen::Region& r : world)
        if (r.slot == slot)
            found = &r;
    if (found != nullptr)
        out = *found;
    else
    {
        // A slot the world has not opened: its contents are its own whatever it opens from
        // (#342), but where its mouth is and which way it lies are not -- so it is opened as
        // the tide most likely would, from its lowest-numbered open neighbour.
        Gen::RegionParams  prm = Gen::SlotParams(g, slot);
        const Gen::Region* from = nullptr;
        for (int n : g.slots[(size_t)slot].neighbours)
            for (const Gen::Region& r : world)
                if (from == nullptr && r.slot == n)
                    from = &r;
        json homeDoc;
        if (prm.kind == Gen::SlotKind::Frontier && from != nullptr)
        {
            double tx, ty;
            Gen::SlotMapPoint(seed, slot, tx, ty);
            const std::string homeId = Gen::MouthSystem(*from, tx, ty);
            const auto        doc = from->documents.find(homeId);
            if (doc != from->documents.end())
                homeDoc = doc->second;
            Gen::HomeFrom(prm, g, *from, homeId, homeDoc);
        }
        else if (prm.kind == Gen::SlotKind::Frontier)
            prm.homeId = "home";
        out = Gen::GenerateRegion(prm);
        Gen::RegisterNatives(out.natives);
    }
    if (homeName != nullptr)
        *homeName = out.homeId;
    for (const Gen::Region& r : world)
        for (const json& s : r.systems)
            if (!out.homeId.empty() && s.value("id", std::string()) == out.homeId)
            {
                if (homeName != nullptr)
                    *homeName = s.value("name", out.homeId);
                if (homeMap != nullptr)
                    *homeMap = { s["map"][0].get<float>(), s["map"][1].get<float>() };
            }
    return true;
}

bool Editor::WorldRegion(uint64_t seed, std::string& systemId, Gen::Region& out,
                         std::map<std::string, std::string>* names, std::string* homeName,
                         Vector2* homeMap) const
{
    const int slot = systemId.empty() ? -1 : Gen::SlotOf(systemId);
    if (!systemId.empty() && slot < 0)
        return false;
    if (!WorldRegionAt(seed, slot, out, names, homeName, homeMap))
        return false;
    // An empty id is the region's heart -- the first centre's capital -- or its way in.
    if (systemId.empty())
        systemId = !out.heartId.empty() ? out.heartId : out.entryId;
    return out.documents.count(systemId) != 0;
}

int Editor::NextOpenSlot(uint64_t seed, int slot) const
{
    const std::vector<Gen::Region>& world = OpenRegions(seed);
    for (size_t i = 0; i < world.size(); i++)
        if (world[i].slot == slot)
            return world[(i + 1) % world.size()].slot;
    return world.empty() ? -1 : world.front().slot;
}

bool Editor::OpenWorldFile(const std::string& pathIn)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string     path = pathIn;
    if (fs::is_directory(path, ec))
        path = (fs::path(path) / "world.json").generic_string();
    std::ifstream in(path);
    const json    j = in.is_open() ? json::parse(in, nullptr, false) : json();
    if (!j.is_object() || !j.contains("seed") || !j["seed"].is_number_unsigned())
    {
        TraceLog(LOG_WARNING, "World: %s is not a world (no seed in it)", path.c_str());
        return false;
    }
    WorldSource w;
    w.path = path;
    w.seed = j["seed"].get<uint64_t>();
    w.sealDir = (fs::path(path).parent_path() / "regions").generic_string();
    if (fs::path(path).parent_path().empty())
        w.sealDir = "regions";
    if (j.contains("centres") && j["centres"].is_number_integer() && j["centres"].get<int>() >= 1)
        w.centres = std::min(j["centres"].get<int>(), Gen::MAX_CENTRES);
    if (j.contains("growth") && j["growth"].is_object() && j["growth"].value("maxRegions", 0) > 0)
        w.cap = j["growth"]["maxRegions"].get<int>();
    if (j.contains("openings") && j["openings"].is_array())
        for (const json& o : j["openings"])
        {
            if (!o.is_object() || !o.contains("slot") || !o["slot"].is_number_integer())
                continue;
            WorldSource::Entry e;
            e.slot = o["slot"].get<int>();
            e.parent = o.value("parent", -1);
            e.state = o.value("state", std::string("open"));
            e.by = o.value("by", std::string("world"));
            e.why = o.value("why", std::string());
            // Gone from the galaxy into the archive (S4), or that entry's earlier self.
            if (e.state == "archived" || e.state == "returned")
                continue;
            w.log.push_back(e);
        }
    if (j.contains("galaxy") && j["galaxy"].is_object())
        for (auto g = j["galaxy"].begin(); g != j["galaxy"].end(); ++g)
            if (g.value().is_object())
            {
                if (!g.value().value("name", std::string()).empty())
                    w.names[g.key()] = g.value()["name"].get<std::string>();
                if (!g.value().value("discoverer", std::string()).empty())
                    w.found[g.key()] = g.value()["discoverer"].get<std::string>();
            }
    if (j.contains("packs") && j["packs"].is_array())
        for (const json& p : j["packs"])
            if (p.is_object() && p.value("kind", std::string()) == "region")
                w.packs[p.value("slot", -1)] = p.value("id", std::string());
    if (w.log.empty())
    {
        // Before S3 a world kept no log; its regions are a new world's of that seed.
        TraceLog(LOG_WARNING, "World: %s has no opening log -- shown as a new world of seed %llu",
                 path.c_str(), (unsigned long long)w.seed);
        w.path.clear();
    }
    world_ = w;
    openSeed_ = 0;  // what was cached is another world's
    TraceLog(LOG_INFO, "World: %s, seed %llu, %d centre(s), %d region(s) open, sealed in %s",
             path.c_str(), (unsigned long long)w.seed, w.centres, (int)w.log.size(),
             w.sealDir.c_str());
    return true;
}

// ---- The galaxy map ---------------------------------------------------------------------

void Editor::OpenWorldMap(uint64_t seed)
{
    if (mode_ != Mode::World)
        worldMap_.back = mode_;
    BuildWorldMap(seed > 0 ? seed : 1);
    mode_ = Mode::World;
    activeField_.clear();
    openDropdown_.clear();
    placeArchetype_.clear();
}

void Editor::BuildWorldMap(uint64_t seed)
{
    WorldMap& m = worldMap_;
    m.seed = seed;
    m.notice.clear();
    m.skeleton = Gen::GenerateGalaxySkeleton(WorldParamsFor(seed));
    m.slots.assign(m.skeleton.slots.size(), SlotView());

    const std::vector<Gen::Region>& world = OpenRegions(seed);
    for (const Gen::Region& r : world)
    {
        if (r.slot < 0 || r.slot >= (int)m.slots.size())
            continue;
        SlotView& v = m.slots[(size_t)r.slot];
        v.state = "open";
        v.systems = (int)r.systems.size();
        v.pack = r.pack;
        const Gen::Slot& s = m.skeleton.slots[(size_t)r.slot];
        if (!r.pack.empty() && r.heartId.empty() && r.homeId.empty())
            v.title = "pack " + r.pack;
        else if (s.kind == Gen::SlotKind::Centre)
            v.title = OwnerName(s.owner);
        else if (r.natives.is_object() && r.natives.contains("name"))
            v.title = r.natives.value("name", std::string());
    }
    // How each opened: the log's, or a new world's (every centre, a frontier off each).
    if (Saved(seed))
        for (const WorldSource::Entry& e : world_.log)
        {
            if (e.slot < 0 || e.slot >= (int)m.slots.size())
                continue;
            SlotView& v = m.slots[(size_t)e.slot];
            v.state = e.state;
            v.parent = e.parent;
            v.by = e.by;
            v.why = e.why;
        }
    else
        for (const Gen::Opening& o : Gen::InitialOpenings(m.skeleton))
            if (o.slot >= 0 && o.slot < (int)m.slots.size())
            {
                m.slots[(size_t)o.slot].parent = o.from;
                m.slots[(size_t)o.slot].by = "world";
            }

    // The pins, by the slot each is for: one on a slot nobody has opened waits for it.
    const json pins = ReadPins(dataDir_ + "pins.json");
    if (pins.is_object() && pins.contains("pins") && pins["pins"].is_array())
        for (const json& pin : pins["pins"])
        {
            if (!pin.is_object())
                continue;
            const int slot = PinSlot(pin);
            if (slot < 0 || slot >= (int)m.slots.size())
                continue;
            if (!pin.contains("seed"))
                m.slots[(size_t)slot].everySeed++;
            else if (pin["seed"].is_number_unsigned() && pin["seed"].get<uint64_t>() == seed)
                m.slots[(size_t)slot].pins++;
        }

    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    for (size_t i = 0; i < m.skeleton.slots.size(); i++)
    {
        const float x = (float)m.skeleton.slots[i].mapX, y = (float)m.skeleton.slots[i].mapY;
        x1 = i == 0 ? x : fminf(x1, x);
        y1 = i == 0 ? y : fminf(y1, y);
        x2 = i == 0 ? x : fmaxf(x2, x);
        y2 = i == 0 ? y : fmaxf(y2, y);
    }
    m.bounds = { x1, y1, fmaxf(1.0f, x2 - x1), fmaxf(1.0f, y2 - y1) };
    TraceLog(LOG_INFO, "Galaxy map: seed %llu, %d slots, %d open%s", (unsigned long long)seed,
             (int)m.slots.size(), (int)world.size(), Saved(seed) ? " (a saved world)" : "");
}

Vector2 Editor::WorldToScreen(double x, double y) const
{
    // One scale for both axes: the lattice is a lattice, and stretching it would make
    // neighbours look further apart one way than the other.
    const Rectangle r = RegionMapRect();
    const Rectangle b = worldMap_.bounds;
    const float     pad = 40.0f;
    const float     s = fminf((r.width - 2.0f * pad) / b.width, (r.height - 2.0f * pad) / b.height);
    const float     cx = r.x + r.width / 2.0f, cy = r.y + r.height / 2.0f;
    return { cx + ((float)x - (b.x + b.width / 2.0f)) * s,
             cy + ((float)y - (b.y + b.height / 2.0f)) * s };
}

int Editor::WorldHit(Vector2 p) const
{
    if (!CheckCollisionPointRec(p, RegionMapRect()))
        return -1;
    int   best = -1;
    float bestD = 14.0f * 14.0f;
    for (const Gen::Slot& s : worldMap_.skeleton.slots)
    {
        const float d = Vector2DistanceSqr(p, WorldToScreen(s.mapX, s.mapY));
        if (d < bestD)
        {
            bestD = d;
            best = s.index;
        }
    }
    return best;
}

void Editor::LevelUp()
{
    // Up a level: a system to its region, a region to the galaxy, the galaxy back down.
    if (mode_ == Mode::World)
    {
        mode_ = worldMap_.back == Mode::World ? Mode::System : worldMap_.back;
        return;
    }
    if (mode_ == Mode::Region)
    {
        OpenWorldMap(regionMap_.seed);
        return;
    }
    if ((mode_ == Mode::System && generated_.open) || mode_ == Mode::Survey)
    {
        ToggleRegionMap();
        return;
    }
    OpenWorldMap(Saved(world_.seed) ? world_.seed : (regionMap_.seed > 0 ? regionMap_.seed : 1));
}

void Editor::HandleWorldInput()
{
    if (IsKeyPressed(KEY_ESCAPE))
    {
        LevelUp();
        return;
    }
    if (IsKeyPressed(KEY_PAGE_DOWN))
        OpenWorldMap(worldMap_.seed + 1);
    if (IsKeyPressed(KEY_PAGE_UP) && worldMap_.seed > 1)
        OpenWorldMap(worldMap_.seed - 1);
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;
    const int hit = WorldHit(GetMousePosition());
    if (hit < 0)
        return;
    if (generated_.open && dirty_ && !(IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))
    {
        worldMap_.notice = "unsaved edit to " + generated_.name +
                           ": Esc back and Ctrl+S, or Shift+click to discard it";
        return;
    }
    OpenRegionSlot(worldMap_.seed, hit);
}

void Editor::DrawWorldMap()
{
    const WorldMap& m = worldMap_;
    const Rectangle r = RegionMapRect();
    DrawRectangleRec(r, Fade(Ui::PANEL_BG, 0.5f));
    DrawRectangleLinesEx(r, 1.0f, Ui::PANEL_BORDER);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    auto at = [&](int slot)
    {
        const Gen::Slot& s = m.skeleton.slots[(size_t)slot];
        return WorldToScreen(s.mapX, s.mapY);
    };
    auto open = [&](int slot)
    { return slot >= 0 && slot < (int)m.slots.size() && !m.slots[(size_t)slot].state.empty(); };

    // Adjacency first, faint: who may one day join whom. A mouth over it, loud.
    for (const auto& l : m.skeleton.links)
        DrawLineEx(at(l.first), at(l.second), 1.0f,
                   Fade(Ui::PANEL_BORDER, open(l.first) && open(l.second) ? 0.9f : 0.45f));
    for (size_t i = 0; i < m.slots.size(); i++)
        if (open((int)i) && m.slots[i].parent >= 0 && open(m.slots[i].parent))
        {
            const bool drifted = m.slots[i].state == "drifted";
            DrawLineEx(at(m.slots[i].parent), at((int)i), drifted ? 1.0f : 2.5f,
                       drifted ? Fade(kDrifted, 0.8f) : kWormhole);
        }

    const int hover = WorldHit(GetMousePosition());
    for (size_t i = 0; i < m.slots.size(); i++)
    {
        const Gen::Slot& s = m.skeleton.slots[i];
        const SlotView&  v = m.slots[i];
        const Vector2    p = at((int)i);
        const bool       centre = s.kind == Gen::SlotKind::Centre;
        // Pinned: the amber ring of the region map, whether the slot is open or still waiting.
        if (v.pins > 0 || v.everySeed > 0)
            DrawRing(p, 11.0f, 14.0f, 0.0f, 360.0f, 32, v.pins > 0 ? kPin : Fade(kPin, 0.5f));
        if ((int)i == hover)
            DrawCircleLinesV(p, 17.0f, Ui::ACCENT);
        if (v.state.empty())
        {
            // A slot nobody has opened: hollow, and a centre's still a centre.
            DrawCircleLinesV(p, centre ? 7.0f : 4.0f, Fade(centre ? kCentre : Ui::TEXT_DIM, 0.7f));
        }
        else
        {
            Color c = Ui::TEXT;
            if (centre)
                c = kCentre;
            else if (!v.pack.empty())
                c = kPack;
            else if (v.state == "unstable")
                c = kWormhole;
            else if (v.state == "drifted")
                c = kDrifted;
            if (!v.pack.empty())
                DrawRectangleRec({ p.x - 6.0f, p.y - 6.0f, 12.0f, 12.0f }, c);
            else
                DrawCircleV(p, centre ? 9.0f : 7.0f, c);
            if (v.state == "anchored")
                DrawCircleLinesV(p, 10.0f, Ui::TEXT);
        }
        const bool  labelled = !v.state.empty() || v.pins > 0;
        const char* num = TextFormat("%d", s.index);
        Ui::Text(num, (int)p.x + 9, (int)p.y + 3, 10,
                 labelled ? Ui::TEXT : Fade(Ui::TEXT_DIM, 0.7f));
        // The name on the side away from the region it is joined to by a mouth -- its parent,
        // or for a centre the frontier it opened -- so the two names never run into each
        // other.
        if (!v.title.empty())
        {
            int partner = v.parent;
            for (size_t c = 0; partner < 0 && c < m.slots.size(); c++)
                if (m.slots[c].parent == (int)i && open((int)c))
                    partner = (int)c;
            const bool below = partner >= 0 && at(partner).y < p.y;
            const int  tw = Ui::TextWidth(v.title.c_str(), 12);
            Ui::Text(v.title.c_str(), (int)p.x - tw / 2, (int)(below ? p.y + 16.0f : p.y - 26.0f),
                     12, centre ? kCentre : Ui::TEXT);
        }
    }
    EndScissorMode();

    if (hover >= 0)
    {
        const Gen::Slot&                           s = m.skeleton.slots[(size_t)hover];
        const SlotView&                            v = m.slots[(size_t)hover];
        std::vector<std::pair<std::string, Color>> lines;
        lines.push_back(
            { TextFormat("slot %d   %s   ring %d   %d step%s from a centre", s.index,
                         Gen::SlotKindName(s.kind), s.ring, s.hops, s.hops == 1 ? "" : "s"),
              Ui::ACCENT });
        if (v.state.empty())
            lines.push_back({ "not open: the tide may reach it", Ui::TEXT_DIM });
        else
        {
            std::string how = v.state + ", opened by " + (v.by.empty() ? "the world" : v.by);
            if (v.parent >= 0)
                how += " from slot " + std::to_string(v.parent);
            lines.push_back({ how, Ui::TEXT });
            if (!v.why.empty())
                lines.push_back({ v.why, Ui::TEXT_DIM });
            lines.push_back({ TextFormat("%d systems%s%s", v.systems, v.title.empty() ? "" : "   ",
                                         v.title.c_str()),
                              Ui::TEXT });
        }
        if (s.kind == Gen::SlotKind::Centre)
            lines.push_back({ "a civilisation centre, " + OwnerName(s.owner) + "'s", kCentre });
        if (v.pins > 0)
            lines.push_back({ TextFormat("%d pin%s for seed %llu%s", v.pins, v.pins == 1 ? "" : "s",
                                         (unsigned long long)m.seed,
                                         v.state.empty() ? ", waiting for it to open" : ""),
                              kPin });
        if (v.everySeed > 0)
            lines.push_back(
                { TextFormat("%d pin%s for every seed", v.everySeed, v.everySeed == 1 ? "" : "s"),
                  kPin });
        lines.push_back({ "click: open the region", Ui::TEXT_DIM });
        int w = 0;
        for (const auto& l : lines)
            w = std::max(w, Ui::TextWidth(l.first.c_str(), 13));
        const Vector2 mp = GetMousePosition();
        Rectangle     box{ mp.x + 16.0f, mp.y + 12.0f, (float)w + 20.0f,
                           (float)lines.size() * 17.0f + 12.0f };
        if (box.x + box.width > r.x + r.width)
            box.x = mp.x - 16.0f - box.width;
        if (box.y + box.height > r.y + r.height)
            box.y = mp.y - 12.0f - box.height;
        DrawRectangleRec(box, Ui::PANEL_BG);
        DrawRectangleLinesEx(box, 1.0f, Ui::PANEL_BORDER);
        for (size_t i = 0; i < lines.size(); i++)
            Ui::Text(lines[i].first.c_str(), (int)box.x + 10, (int)(box.y + 7.0f + 17.0f * i), 13,
                     lines[i].second);
    }
    if (!m.notice.empty())
        Ui::Text(m.notice.c_str(), (int)r.x + 10, (int)(r.y + r.height - 22.0f), 13, kPin);
}

void Editor::DrawWorldPanel()
{
    const WorldMap& m = worldMap_;
    const Rectangle panel{ (float)screenWidth_ - kPanelW, 0.0f, kPanelW, (float)screenHeight_ };
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);
    const int x = (int)panel.x + 14;
    int       y = 14;

    Ui::Text("GALAXY", x, y, 14, Ui::ACCENT);
    Ui::Text(
        TextFormat("seed %llu   skeleton v%d", (unsigned long long)m.seed, Gen::SKELETON_VERSION),
        x + 70, y + 2, 11, Ui::TEXT_DIM);
    y += 24;
    int open = 0, systems = 0, waiting = 0, unstable = 0, drifted = 0, packs = 0;
    for (const SlotView& v : m.slots)
    {
        open += v.state.empty() ? 0 : 1;
        systems += v.systems;
        waiting += v.state.empty() && v.pins > 0 ? 1 : 0;
        unstable += v.state == "unstable" ? 1 : 0;
        drifted += v.state == "drifted" ? 1 : 0;
        packs += v.pack.empty() ? 0 : 1;
    }
    Ui::Text(TextFormat("%d slots   %d open   %d systems", (int)m.slots.size(), open, systems), x,
             y, 13, Ui::TEXT);
    y += 18;
    Ui::Text(TextFormat("%d centre(s)   %d pack region(s)", (int)m.skeleton.centres.size(), packs),
             x, y, 13, Ui::TEXT);
    y += 18;
    if (unstable + drifted > 0)
    {
        Ui::Text(TextFormat("%d unstable mouth(s)   %d drifted", unstable, drifted), x, y, 13,
                 kWormhole);
        y += 18;
    }
    if (waiting > 0)
    {
        Ui::Text(TextFormat("%d unopened slot(s) a pin waits on", waiting), x, y, 13, kPin);
        y += 18;
    }
    const std::string from =
        Saved(m.seed) ? "a saved world: " + world_.path : std::string("a new world of this seed");
    Ui::Text(from.c_str(), x, y, 11, Ui::TEXT_DIM);
    y += 15;
    if (!Saved(m.seed))
    {
        Ui::Text("(packs are woven only by a server)", x, y, 11, Ui::TEXT_DIM);
        y += 15;
    }
    y += 8;
    DrawLine(x, y, x + (int)kPanelW - 28, y, Ui::PANEL_BORDER);
    y += 12;

    Ui::Text("SLOTS", x, y, 11, Ui::TEXT_DIM);
    y += 18;
    struct Row
    {
        int         shape;  // 0 dot, 1 square, 2 hollow
        Color       c;
        const char* what;
    };
    const Row rows[] = { { 0, kCentre, "civilisation centre" },
                         { 0, Ui::TEXT, "open frontier" },
                         { 0, kWormhole, "unstable mouth: not anchored yet" },
                         { 0, kDrifted, "drifted: its mouth collapsed" },
                         { 1, kPack, "an admin pack's region" },
                         { 2, Ui::TEXT_DIM, "not open yet" } };
    for (const Row& row : rows)
    {
        const Vector2 p{ (float)x + 7.0f, (float)y + 6.0f };
        if (row.shape == 0)
            DrawCircleV(p, 6.0f, row.c);
        else if (row.shape == 1)
            DrawRectangleRec({ p.x - 6.0f, p.y - 6.0f, 12.0f, 12.0f }, row.c);
        else
            DrawCircleLinesV(p, 5.0f, row.c);
        Ui::Text(row.what, x + 20, y, 12, Ui::TEXT);
        y += 20;
    }
    DrawCircleV({ (float)x + 7.0f, (float)y + 6.0f }, 6.0f, Ui::TEXT);
    DrawCircleLinesV({ (float)x + 7.0f, (float)y + 6.0f }, 9.0f, Ui::TEXT);
    Ui::Text("anchored", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DrawRing({ (float)x + 7.0f, (float)y + 6.0f }, 5.0f, 7.5f, 0.0f, 360.0f, 24, kPin);
    Ui::Text("pinned (faint: a pin for every seed)", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DrawLineEx({ (float)x, (float)y + 6.0f }, { (float)x + 14.0f, (float)y + 6.0f }, 2.5f,
               kWormhole);
    Ui::Text("the mouth a region opened through", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DrawLineEx({ (float)x, (float)y + 6.0f }, { (float)x + 14.0f, (float)y + 6.0f }, 1.0f,
               Ui::PANEL_BORDER);
    Ui::Text("adjacent: may one day be joined", x + 20, y, 12, Ui::TEXT);

    const int ky = screenHeight_ - 74;
    DrawLine(x, ky - 6, x + (int)kPanelW - 28, ky - 6, Ui::PANEL_BORDER);
    Ui::Text("click  open the region of a slot", x, ky, 11, Ui::TEXT_DIM);
    Ui::Text("hover  what the world made of it", x, ky + 15, 11, Ui::TEXT_DIM);
    Ui::Text("PgDn / PgUp  next / previous seed", x, ky + 30, 11, Ui::TEXT_DIM);
    Ui::Text("Esc / F5  back", x, ky + 45, 11, Ui::TEXT_DIM);
}

// ---- Packs ------------------------------------------------------------------------------

void Editor::SetPacksDir(const std::string& dir)
{
    packsDir_ = dir;
    if (!packsDir_.empty() && packsDir_.back() != '/' && packsDir_.back() != '\\')
        packsDir_ += '/';
}

bool Editor::OpenPack(const std::string& id, bool create)
{
    if (!Gen::PackIdValid(id))
    {
        TraceLog(LOG_WARNING,
                 "Pack: \"%s\" is not a pack id -- lower-case letters, digits, '-' "
                 "and '_'",
                 id.c_str());
        return false;
    }
    const std::string dir = packsDir_ + id;
    if (create)
    {
        std::string error;
        if (!Gen::WritePack(dir, Gen::NewRegionPack(id, std::string()), false, error))
        {
            TraceLog(LOG_WARNING, "Pack: %s", error.c_str());
            return false;
        }
        TraceLog(LOG_INFO, "Pack: began %s", dir.c_str());
    }
    std::ifstream in(dir + "/pack.json");
    json          index = in.is_open() ? json::parse(in, nullptr, false) : json();
    if (!index.is_object())
    {
        TraceLog(LOG_WARNING, "Pack: %s/pack.json is missing or not valid JSON", dir.c_str());
        return false;
    }
    packId_ = id;
    indexPath_ = dir + "/pack.json";
    systemsDir_ = dir + "/";
    universeJson_ = index;
    universeDirty_ = false;
    currentSystem_ = 0;
    generated_.open = false;
    RefreshUniverseStruct();
    EnterGalaxyMode(true);
    // The map of a pack is in hundreds of units round its middle.
    camera_.zoom = 0.45f;
    CheckPack();
    return true;
}

bool Editor::ExportRegion(const std::string& idIn)
{
    const RegionMap& m = regionMap_;
    std::string      id = idIn.empty() ? "region-" + std::to_string(m.region.slot) : idIn;
    if (!Gen::PackIdValid(id))
    {
        Notice("Not exported: \"" + id + "\" is not a pack id");
        return false;
    }
    std::vector<std::string> notes;
    std::string              name;
    if (!m.region.heartId.empty())
        for (const json& s : m.region.systems)
            if (s.value("id", std::string()) == m.region.heartId)
                name = s.value("name", std::string());
    if (name.empty() && m.region.natives.is_object())
        name = m.region.natives.value("name", std::string());
    const Gen::PackFiles files = Gen::RegionAsPack(m.region, id, name, &notes);
    std::string          error;
    if (!Gen::WritePack(packsDir_ + id, files, false, error))
    {
        Notice("Not exported: " + error);
        regionMap_.notice = "not exported: " + error;
        return false;
    }
    for (const std::string& n : notes)
        TraceLog(LOG_INFO, "Pack %s: %s", id.c_str(), n.c_str());
    TraceLog(LOG_INFO, "Pack: region %d of seed %llu exported as %s%s (%d systems)", m.region.slot,
             (unsigned long long)m.seed, packsDir_.c_str(), id.c_str(),
             (int)m.region.systems.size());
    return OpenPack(id, false);
}

void Editor::CheckPack()
{
    if (packId_.empty())
        return;
    // Read as the server reads it -- the same check `econserver validate` makes of a pack,
    // short of weaving it into a trial world.
    Gen::Pack                p;
    std::vector<std::string> problems;
    Gen::LoadPack(packsDir_ + packId_, p, problems);
    for (const std::string& why : problems)
        TraceLog(LOG_WARNING, "Pack: %s", why.c_str());
    if (problems.empty())
        Notice("Pack " + packId_ + " reads as the server reads it: " +
               std::to_string(p.systems.size()) + " system(s), hash " + Gen::PackHashText(p.hash));
    else
        Notice(std::to_string(problems.size()) + " problem(s), the first: " + problems.front());
}
