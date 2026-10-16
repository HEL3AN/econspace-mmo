// Roaming holes (#342 S9): EVE's wormholes, between regions already open.
//
// They are the third kind of wormhole the design names, beside the tide's mouths and the
// admin's links: shortcuts that come and go, and the way out for a pilot stranded in a region
// whose mouth collapsed. Each comes from an epoch: when one begins, every live region's key --
// the seed, the epoch, its slot -- says whether it gets a hole, from which of its systems to
// which system of another region, when in the epoch it opens, how long it lives, which ships
// it lets through and how much mass in all. That is decided once and kept: the systems open
// at that moment are not the seed's to say again.
//
// A hole is two gates of `gate.roaming`, one at each end, and both are hidden: a pilot sees
// them only once a scan has resolved one (#346, Simulation_Scan.cpp), and then the hole is a
// route of theirs -- on their map, and in the routes their orders plan. Nobody else's: NPCs,
// migrants and factions keep to the gates.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "core/Archetype.h"
#include "core/ShipDesign.h"
#include "entities/JumpGate.h"
#include "entities/Ship.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "gen/Rng.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace
{
// What keys a region's hole of an epoch, beside the epoch and the slot.
constexpr uint64_t    HOLE_KEY = 0x401E5u;
constexpr double      HOUR = 3600.0;
constexpr const char* HOLE_ARCHETYPE = "gate.roaming";

std::string HoleKey(const std::string& id)
{
    return "hole:" + id;
}
}  // namespace

const Simulation::RoamingHole* Simulation::HoleById(const std::string& id) const
{
    for (const RoamingHole& h : holes_)
        if (h.id == id)
            return &h;
    return nullptr;
}

Simulation::RoamingHole* Simulation::MutableHole(const std::string& id)
{
    return const_cast<RoamingHole*>(HoleById(id));
}

const Simulation::RoamingHole* Simulation::HoleOfGate(int gateId) const
{
    if (gateId == 0)
        return nullptr;
    for (const RoamingHole& h : holes_)
        if (h.open && (h.gateA == gateId || h.gateB == gateId))
            return &h;
    return nullptr;
}

double Simulation::ShipMass(const ClientSession& s) const
{
    const Ships::Design* d = designs_.Find(s.DesignOf(s.currentShip));
    Ships::Stats         stats;
    std::string          why;
    if (d == nullptr || !Ships::Derive(Archetypes::ShipCatalogue(), *d, stats, why))
        return 0.0;
    return stats.mass;
}

void Simulation::ScheduleHoles(int epoch)
{
    if (epoch <= holeEpoch_ || skeleton_.slots.empty())
        return;
    holeEpoch_ = epoch;
    const WorldRules::Holes& r = WorldRules::HoleRules();
    const double             length = r.epochHours * HOUR;

    // Every region in the galaxy now, and those a hole may lead to: not one cut off, or a
    // hole would be a way from one stranded region into another.
    std::vector<int> live, reachable;
    for (int slot : openSlots_)
    {
        const RegionOpening* o = OpeningOf(slot);
        if (o != nullptr && (o->state == "archived" || o->state == "returned"))
            continue;
        live.push_back(slot);
        if (o == nullptr || o->state != "drifted")
            reachable.push_back(slot);
    }
    auto systemsOf = [this](int slot)
    {
        std::vector<std::string> ids = RegionSystems(slot);
        std::sort(ids.begin(), ids.end());  // the index's order is the order regions opened in
        return ids;
    };
    // Where an end goes: at the edge of its system, facing the other end on the map, clear of
    // the planets, the gates and the other holes' ends there.
    auto place = [this](uint64_t key, const std::string& here, const std::string& there)
    {
        nlohmann::json doc = nlohmann::json::object();
        const auto     d = regionDocs_.find(here);
        if (d != regionDocs_.end())
            doc = nlohmann::json::parse(d->second, nullptr, false);
        if (!doc.is_object())
            doc = nlohmann::json::object();
        if (!doc.contains("gates") || !doc["gates"].is_array())
            doc["gates"] = nlohmann::json::array();
        const auto mouths = wormholes_.find(here);
        if (mouths != wormholes_.end())
            for (const std::string& g : mouths->second)
                doc["gates"].push_back(nlohmann::json::parse(g, nullptr, false));
        for (const RoamingHole& h : holes_)
            for (const auto& [sys, at] : { std::make_pair(h.a, h.atA), std::make_pair(h.b, h.atB) })
                if (sys == here)
                    doc["gates"].push_back({ { "pos", { at.x, at.y } }, { "size", 700.0 } });
        const WorldLoader::SystemInfo* a = SystemInfoById(here);
        const WorldLoader::SystemInfo* b = SystemInfoById(there);
        const double dx = a != nullptr && b != nullptr ? (double)(b->mapPos.x - a->mapPos.x) : 1.0;
        const double dy = a != nullptr && b != nullptr ? (double)(b->mapPos.y - a->mapPos.y) : 0.0;
        const nlohmann::json pos = Gen::EdgeGatePos(key, &doc, dx, dy);
        return Vector2{ pos[0].get<float>(), pos[1].get<float>() };
    };

    for (int slot : live)
    {
        const RegionOpening* o = OpeningOf(slot);
        const bool           cutOff = o != nullptr && o->state == "drifted";
        Gen::Rng             rng(Gen::Key(regionSeed_, HOLE_KEY, (uint64_t)epoch, (uint64_t)slot));
        const double         chance = cutOff ? r.drifted : AtCap() ? r.atCap : r.perRegion;
        if (rng.Unit() >= chance)
            continue;
        const std::vector<std::string> here = systemsOf(slot);
        std::vector<int>               others;
        for (int t : reachable)
            if (t != slot)
                others.push_back(t);
        if (here.empty() || others.empty())
            continue;
        RoamingHole h;
        h.id = "h" + std::to_string(epoch) + "-" + std::to_string(slot);
        h.epoch = epoch;
        h.slot = slot;
        h.a = here[(size_t)rng.Range(0, (int)here.size() - 1)];
        const std::vector<std::string> there =
            systemsOf(others[(size_t)rng.Range(0, (int)others.size() - 1)]);
        if (there.empty())
            continue;
        h.b = there[(size_t)rng.Range(0, (int)there.size() - 1)];
        h.opensAt = (double)epoch * length + rng.Unit() * length;
        h.closesAt = h.opensAt + rng.Between(r.minHours, r.maxHours) * HOUR;
        const double kind = rng.Unit();
        h.maxMass = kind < r.smallWeight                    ? r.smallMass
                    : kind < r.smallWeight + r.mediumWeight ? r.mediumMass
                                                            : r.largeMass;
        h.budget = h.maxMass * rng.Between(r.budgetMin, r.budgetMax);
        const uint64_t at = rng.Next();
        h.atA = place(Gen::Key(at, 1u), h.a, h.b);
        h.atB = place(Gen::Key(at, 2u), h.b, h.a);
        if (h.closesAt > time_)  // decided late -- after a long stop -- it may be over already
            holes_.push_back(std::move(h));
    }
}

std::string Simulation::OpenHoleNow(const std::string& a, const std::string& b, double maxMass,
                                    double hours, double budget)
{
    if (a == b || !HasSystem(a) || !HasSystem(b))
        return std::string();
    RoamingHole h;
    int         n = 0;
    while (HoleById("admin-" + std::to_string(n)) != nullptr)
        n++;
    h.id = "admin-" + std::to_string(n);
    h.epoch = holeEpoch_;
    h.slot = Gen::SlotOf(a);
    h.a = a;
    h.b = b;
    // Halfway between the middle and the edge, on the side facing the other end: clear of the
    // gates, which are at the edge, and of where ships arrive.
    auto halfway = [this](const std::string& here, const std::string& there)
    {
        const WorldLoader::SystemInfo* p = SystemInfoById(here);
        const WorldLoader::SystemInfo* q = SystemInfoById(there);
        float       dx = p != nullptr && q != nullptr ? q->mapPos.x - p->mapPos.x : 1.0f;
        float       dy = p != nullptr && q != nullptr ? q->mapPos.y - p->mapPos.y : 0.0f;
        const float l = std::sqrt(dx * dx + dy * dy);
        dx = l > 0.0f ? dx / l : 1.0f;
        dy = l > 0.0f ? dy / l : 0.0f;
        return Vector2{ dx * 400000.0f, dy * 400000.0f };
    };
    h.atA = halfway(a, b);
    h.atB = halfway(b, a);
    h.opensAt = time_;
    h.closesAt = time_ + hours * HOUR;
    h.maxMass = maxMass;
    h.budget = budget;
    holes_.push_back(h);
    OpenHole(holes_.back());
    return h.id;
}

std::string Simulation::HoleName(const RoamingHole& h) const
{
    const WorldRules::Holes& r = WorldRules::HoleRules();
    std::string              name = h.maxMass <= r.smallMass    ? "Wormhole (small ships"
                                    : h.maxMass <= r.mediumMass ? "Wormhole (medium ships"
                                                                : "Wormhole (any ship";
    const bool               ending = h.closesAt - time_ < r.endingHours * HOUR;
    const bool               low = h.budget > 0.0 && (h.budget - h.spent) / h.budget < r.lowMass;
    if (!ending && !low)
        return name + ", fresh)";
    return name + (ending ? ", ending" : "") + (low ? ", mass low" : "") + ")";
}

void Simulation::BuildHoleGates(RoamingHole& h)
{
    const Archetype* look = Archetypes::Find(HOLE_ARCHETYPE);
    const float      size = look != nullptr ? look->defaultSize : 700.0f;
    h.dressed = HoleName(h);
    for (int end = 0; end < 2; end++)
    {
        const std::string& here = end == 0 ? h.a : h.b;
        const std::string& there = end == 0 ? h.b : h.a;
        int&               gate = end == 0 ? h.gateA : h.gateB;
        SystemState*       st = SystemById(here);
        if (st == nullptr || st->entities.empty())
            continue;  // not built yet: the next pass builds it, once it is
        bool standing = false;
        for (const auto& e : st->entities)
            standing = standing || (gate != 0 && e->GetId() == gate);
        if (standing)
            continue;
        auto g = std::make_unique<JumpGate>(end == 0 ? h.atA : h.atB, size, h.dressed, there);
        g->SetArchetype(HOLE_ARCHETYPE);
        g->SetClosesAt(h.closesAt);  // every client counts it down from the layout
        gate = NextAgentId();
        g->SetId(gate);
        st->entities.push_back(std::move(g));
        st->pendingAdded.insert(gate);
        st->layoutRev++;
    }
}

void Simulation::OpenHole(RoamingHole& h)
{
    h.open = true;
    BuildHoleGates(h);
}

void Simulation::CloseHole(RoamingHole& h, const std::string& why)
{
    for (const auto& [here, gate] : { std::make_pair(h.a, h.gateA), std::make_pair(h.b, h.gateB) })
    {
        SystemState* st = SystemById(here);
        if (st == nullptr || gate == 0)
            continue;
        // A gate is never RemoveStatic's (#38), but it is a change to the layer, and a delta
        // says so before any snapshot shows it gone -- to those who never saw it, a removal
        // of something they do not have, which is nothing.
        const auto it = std::find_if(st->entities.begin(), st->entities.end(),
                                     [gate = gate](const std::unique_ptr<Entity>& e)
                                     { return e->GetId() == gate; });
        if (it == st->entities.end())
            continue;
        st->entities.erase(it);
        st->pendingAdded.erase(gate);
        st->pendingChanged.erase(gate);
        st->pendingRemoved.insert(gate);
        st->layoutRev++;
    }
    const std::string key = HoleKey(h.id);
    for (auto& kv : sessions_)
    {
        ClientSession& s = kv.second;
        s.scanProgress.erase(key);
        if (s.scanned.erase(key) == 0)
            continue;
        s.routesChanged = true;
        if (s.systemId == h.a || s.systemId == h.b)
            s.RecordEvent(Ev::Kind::Notice, "The wormhole to " +
                                                SystemName(s.systemId == h.a ? h.b : h.a) +
                                                " collapsed: " + why);
    }
    foundSignatures_.erase(key);
    h.open = false;
    h.gateA = h.gateB = 0;
    h.closesAt = std::min(h.closesAt, time_);
}

void Simulation::StepHoles()
{
    if (skeleton_.slots.empty())
        return;  // a world of files, not of regions: nothing for a hole to join
    const WorldRules::Holes& r = WorldRules::HoleRules();
    const int                current = (int)std::floor(time_ / (r.epochHours * HOUR));
    // The epochs whose holes may still be open, after a stop: each decided once.
    const int back = (int)std::ceil(r.maxHours / r.epochHours);
    for (int e = std::max(holeEpoch_ + 1, current - back); e <= current; e++)
        ScheduleHoles(e);

    for (RoamingHole& h : holes_)
    {
        if (!h.open && time_ >= h.opensAt && time_ < h.closesAt)
            OpenHole(h);
        if (!h.open)
            continue;
        if (time_ >= h.closesAt)
        {
            CloseHole(h, "its time ran out");
            continue;
        }
        if (!HasSystem(h.a) || !HasSystem(h.b))
        {
            CloseHole(h, "the region beyond drifted away");
            continue;
        }
        BuildHoleGates(h);  // a system built since, or rebuilt
        const std::string name = HoleName(h);
        if (name == h.dressed)
            continue;
        h.dressed = name;
        for (const auto& [here, gate] :
             { std::make_pair(h.a, h.gateA), std::make_pair(h.b, h.gateB) })
            if (SystemState* st = SystemById(here))
                for (const auto& e : st->entities)
                    if (e->GetId() == gate)
                    {
                        static_cast<JumpGate&>(*e).SetName(name);
                        MarkStaticChanged(*st, gate);
                    }
    }
    holes_.erase(std::remove_if(holes_.begin(), holes_.end(), [this](const RoamingHole& h)
                                { return !h.open && h.closesAt <= time_; }),
                 holes_.end());
}

void Simulation::EnterThrough(ClientSession& s, int gateId, const std::string& dest)
{
    const RoamingHole* found = HoleOfGate(gateId);
    const std::string  from = s.systemId;
    ServerEnterSystem(s, dest, from);
    if (found == nullptr || !s.ship)
        return;
    RoamingHole& h = *MutableHole(found->id);
    // Out of the far end -- not beside a gate that happens to lead back as well.
    const int    far = dest == h.b ? h.gateB : h.gateA;
    SystemState* st = SystemById(dest);
    if (st != nullptr)
        for (const auto& e : st->entities)
            if (e->GetId() == far)
            {
                const Vector2 gp = e->GetPosition();
                const float   d = std::sqrt(gp.x * gp.x + gp.y * gp.y);
                const Vector2 dir =
                    d > 1.0f ? Vector2{ -gp.x / d, -gp.y / d } : Vector2{ 0.0f, 1.0f };
                const float off = e->GetSize() + ARRIVAL_CLEARANCE;
                s.ship->Teleport({ gp.x + dir.x * off, gp.y + dir.y * off });
                s.ship->SetHeading(std::atan2(dir.y, dir.x));
            }
    // Whoever went through knows the way back.
    if (s.scanned.insert(HoleKey(h.id)).second)
        s.routesChanged = true;
    h.spent += ShipMass(s);
    if (h.spent >= h.budget)
        CloseHole(h, "too much mass went through it");
}

std::vector<WorldLoader::SystemLink> Simulation::HoleLinksOf(const ClientSession& s) const
{
    std::vector<WorldLoader::SystemLink> out;
    for (const RoamingHole& h : holes_)
        if (h.open && s.scanned.count(HoleKey(h.id)) != 0)
        {
            WorldLoader::SystemLink l{ h.a, h.b };
            l.kind = "hole";
            l.closesAt = h.closesAt;
            l.maxMass = (float)h.maxMass;
            out.push_back(std::move(l));
        }
    return out;
}

WorldLoader::Universe Simulation::KnownUniverseFor(const ClientSession& s) const
{
    WorldLoader::Universe u = KnownUniverse();
    for (const WorldLoader::SystemLink& l : HoleLinksOf(s))
    {
        // The far end on the map, as a gate's far side is: located and designated.
        for (const std::string& id : { l.a, l.b })
        {
            bool known = false;
            for (const WorldLoader::SystemInfo& si : u.systems)
                known = known || si.id == id;
            const WorldLoader::SystemInfo* info = SystemInfoById(id);
            if (known || info == nullptr)
                continue;
            WorldLoader::SystemInfo seen;
            seen.id = info->id;
            seen.name = info->name;
            seen.mapPos = info->mapPos;
            seen.security = 0.0f;
            seen.charted = false;
            u.systems.push_back(seen);
        }
        u.links.push_back(l);
    }
    return u;
}

void Simulation::SaveHoles(nlohmann::json& j) const
{
    using nlohmann::json;
    json list = json::array();
    for (const RoamingHole& h : holes_)
    {
        json hj = { { "id", h.id },
                    { "epoch", h.epoch },
                    { "slot", h.slot },
                    { "a", h.a },
                    { "b", h.b },
                    { "atA", { h.atA.x, h.atA.y } },
                    { "atB", { h.atB.x, h.atB.y } },
                    { "opensAt", h.opensAt },
                    { "closesAt", h.closesAt },
                    { "maxMass", h.maxMass },
                    { "budget", h.budget },
                    { "spent", h.spent } };
        list.push_back(std::move(hj));
    }
    // A field an older build ignores (#20): it decides the epoch again, and finds nothing.
    j["holes"] = { { "epoch", holeEpoch_ },
                   { "list", std::move(list) },
                   { "found", json(foundSignatures_) } };
}

void Simulation::LoadHoles(const nlohmann::json& j)
{
    holes_.clear();
    foundSignatures_.clear();
    if (!j.contains("holes") || !j["holes"].is_object())
        return;
    const nlohmann::json& hs = j["holes"];
    holeEpoch_ = hs.value("epoch", -1);
    auto point = [](const nlohmann::json& p)
    {
        return p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()
                   ? Vector2{ p[0].get<float>(), p[1].get<float>() }
                   : Vector2{ 0.0f, 0.0f };
    };
    for (const nlohmann::json& hj : hs.value("list", nlohmann::json::array()))
    {
        if (!hj.is_object())
            continue;
        RoamingHole h;
        h.id = hj.value("id", std::string());
        h.epoch = hj.value("epoch", 0);
        h.slot = hj.value("slot", 0);
        h.a = hj.value("a", std::string());
        h.b = hj.value("b", std::string());
        h.atA = point(hj.value("atA", nlohmann::json()));
        h.atB = point(hj.value("atB", nlohmann::json()));
        h.opensAt = hj.value("opensAt", 0.0);
        h.closesAt = hj.value("closesAt", 0.0);
        h.maxMass = hj.value("maxMass", 0.0);
        h.budget = hj.value("budget", 0.0);
        h.spent = hj.value("spent", 0.0);
        // A system gone since -- a region drifted into the archive -- takes its holes with it.
        if (h.id.empty() || !HasSystem(h.a) || !HasSystem(h.b) || h.closesAt <= time_)
            continue;
        h.open = time_ >= h.opensAt;  // its gates go up on the first pass
        holes_.push_back(std::move(h));
    }
    for (const nlohmann::json& k : hs.value("found", nlohmann::json::array()))
        if (k.is_string())
            foundSignatures_.insert(k.get<std::string>());
}
