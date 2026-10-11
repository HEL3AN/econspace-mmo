// How the world grows (#342 S3): the opening log, sealing, and the tide that opens the next
// region.
//
// A world is its seed plus the log of the regions that opened, in order. Each region is
// sealed the moment it opens -- written to its own file as the generator made it -- and read
// from there from then on, so a later build's rules change only what opens later. Every
// region on the edge gathers tide in the coarse pass, from time, from the players in it, from
// how much of it has been explored and from the factions surveying it; when it crosses the
// threshold, a wormhole opens from its deepest system into the next slot of the skeleton.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/SaveFile.h"
#include "sim/WorldRules.h"

#include "core/WorldLoader.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "gen/Rng.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
// The seed's keys for the tide: how strained a region's slot is, and how far into its first
// tide a region begins.
constexpr uint64_t STRAIN_KEY = 0x71DE5u;
constexpr uint64_t START_KEY = 0x71DE6u;
constexpr int      SEAL_SCHEMA = 1;
constexpr double   HOUR = 3600.0;

std::string Percent(double part, double whole)
{
    return std::to_string((int)std::lround(whole > 0.0 ? 100.0 * part / whole : 0.0)) + "%";
}
}  // namespace

// --- The log and the seal ------------------------------------------------------------------

std::string Simulation::SealedPath(const std::string& dir, int slot)
{
    std::string d = dir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\')
        d += '/';
    return d + std::to_string(slot) + ".json";
}

bool Simulation::ReadSealedText(const std::string& text, uint64_t seed, Gen::Region& out)
{
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    // Another world's region -- a world.json moved aside and its regions left behind -- is
    // not this one's, whatever its slot.
    if (j.is_discarded() || !j.is_object() || j.value("schema", 0) != SEAL_SCHEMA ||
        !j.contains("seed") || !j["seed"].is_number_unsigned() ||
        j["seed"].get<uint64_t>() != seed || !j.contains("region"))
        return false;
    return Gen::RegionFromJson(j["region"], out);
}

bool Simulation::ReadSealed(const std::string& path, uint64_t seed, Gen::Region& out)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ReadSealedText(text, seed, out);
}

std::string Simulation::SealedText(int slot) const
{
    if (sealDir_.empty() || openSlots_.count(slot) == 0)
        return std::string();
    std::ifstream in(SealedPath(sealDir_, slot));
    return in.is_open()
               ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())
               : std::string();
}

void Simulation::Seal(const Gen::Region& region)
{
    // Nowhere to keep it -- a test, a benchmark, the batch mode -- and nothing to read it
    // back: a world without a file is remade from its seed anyway.
    if (sealDir_.empty())
        return;
    const nlohmann::json doc = { { "schema", SEAL_SCHEMA },
                                 { "seed", regionSeed_ },
                                 { "generator", Gen::GENERATOR_VERSION },
                                 { "region", Gen::RegionToJson(region) } };
    std::error_code      ec;
    std::filesystem::create_directories(sealDir_, ec);
    if (!Save::WriteFileAtomic(SealedPath(sealDir_, region.slot), doc.dump(1) + "\n", 0))
        TraceLog(LOG_WARNING, "World: could not seal region %d into %s", region.slot,
                 SealedPath(sealDir_, region.slot).c_str());
}

Gen::Region Simulation::MakeSealed(int slot, const Gen::RegionParams& params)
{
    Gen::Region region;
    const auto  kept = sealedIn_.find(slot);
    if (kept != sealedIn_.end() && ReadSealedText(kept->second, regionSeed_, region))
    {
        // As it was made, by whatever rules made it then: never written again.
        sealedIn_.erase(kept);
        return region;
    }
    region = Gen::GenerateRegion(params);
    Seal(region);
    return region;
}

void Simulation::LogOpening(int slot, const std::string& mouth, const std::string& by,
                            const std::string& why)
{
    RegionOpening o;
    o.slot = slot;
    o.openedAt = time_;
    o.parent = mouth.empty() ? -1 : Gen::SlotOf(mouth);
    o.mouth = mouth;
    o.by = by;
    o.why = why;
    openings_.push_back(o);
}

bool Simulation::ReadWorldHeader(const std::string& path, WorldHeader& out)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("seed") ||
        !j["seed"].is_number_unsigned())
        return false;
    WorldHeader h;
    h.seed = j["seed"].get<uint64_t>();
    auto integer = [&j](const char* key, int fallback)
    { return j.contains(key) && j[key].is_number_integer() ? j[key].get<int>() : fallback; };
    h.generator = integer("generator", 0);
    h.skeleton = integer("skeleton", 0);
    h.centres = integer("centres", -1);
    if (j.contains("growth") && j["growth"].is_object())
    {
        const nlohmann::json& g = j["growth"];
        h.maxRegions = std::max(0, g.value("maxRegions", 0));
        h.maxSystems = std::max(0, g.value("maxSystems", 0));
    }
    if (j.contains("openings") && j["openings"].is_array())
        for (const nlohmann::json& oj : j["openings"])
        {
            if (!oj.is_object() || !oj.contains("slot") || !oj["slot"].is_number_integer())
                continue;
            RegionOpening o;
            o.slot = oj["slot"].get<int>();
            o.openedAt = oj.value("openedAt", 0.0);
            o.parent = oj.value("parent", -1);
            o.mouth = oj.value("mouth", std::string());
            o.state = oj.value("state", std::string("open"));
            o.by = oj.value("by", std::string("world"));
            o.why = oj.value("why", std::string());
            h.log.push_back(o);
        }
    out = std::move(h);
    return true;
}

void Simulation::SaveGrowth(nlohmann::json& j) const
{
    using nlohmann::json;
    json log = json::array();
    for (const RegionOpening& o : openings_)
    {
        json oj = { { "slot", o.slot },   { "openedAt", o.openedAt }, { "parent", o.parent },
                    { "mouth", o.mouth }, { "state", o.state },       { "by", o.by } };
        if (!o.why.empty())
            oj["why"] = o.why;
        log.push_back(std::move(oj));
    }
    j["openings"] = std::move(log);
    json tide = json::object();
    for (const auto& kv : tides_)
        tide[std::to_string(kv.first)] = { { "tide", kv.second.tide },
                                           { "time", kv.second.fromTime },
                                           { "players", kv.second.fromPlayers },
                                           { "explore", kv.second.fromExplore },
                                           { "surveys", kv.second.fromSurveys },
                                           { "pending", kv.second.surveys },
                                           { "lastMouthAt", kv.second.lastMouthAt } };
    j["growth"] = { { "paused", growthPaused_ },   { "maxRegions", maxRegions_ },
                    { "maxSystems", maxSystems_ }, { "surplus", surplusTide_ },
                    { "tide", std::move(tide) },   { "explored", explored_ },
                    { "surveyed", surveyed_ } };
}

void Simulation::LoadGrowth(const nlohmann::json& j)
{
    // A world from before S3 has none: its tide starts now, as a new world's does.
    if (!j.contains("growth") || !j["growth"].is_object())
        return;
    const nlohmann::json& g = j["growth"];
    growthPaused_ = g.value("paused", false);
    surplusTide_ = std::max(0.0, g.value("surplus", 0.0));
    const nlohmann::json& tide = g.value("tide", nlohmann::json::object());
    for (auto it = tide.begin(); tide.is_object() && it != tide.end(); ++it)
    {
        const int slot = std::atoi(it.key().c_str());
        if (openSlots_.count(slot) == 0 || !it.value().is_object())
            continue;
        RegionTide t;
        t.tide = std::max(0.0, it.value().value("tide", 0.0));
        t.fromTime = it.value().value("time", 0.0);
        t.fromPlayers = it.value().value("players", 0.0);
        t.fromExplore = it.value().value("explore", 0.0);
        t.fromSurveys = it.value().value("surveys", 0.0);
        t.surveys = std::max(0, it.value().value("pending", 0));
        t.lastMouthAt = it.value().value("lastMouthAt", -1.0e18);
        tides_[slot] = t;
    }
    for (const char* key : { "explored", "surveyed" })
    {
        const nlohmann::json&  ids = g.value(key, nlohmann::json::array());
        std::set<std::string>& into = std::string(key) == "explored" ? explored_ : surveyed_;
        for (const nlohmann::json& id : ids)
            if (id.is_string() && HasSystem(id.get<std::string>()))
                into.insert(id.get<std::string>());
    }
}

// --- The tide ------------------------------------------------------------------------------

void Simulation::SetGrowthCap(int maxRegions, int maxSystems)
{
    if (maxRegions > 0)
        maxRegions_ = skeleton_.slots.empty() ? maxRegions
                                              : std::min(maxRegions, (int)skeleton_.slots.size());
    if (maxSystems > 0)
        maxSystems_ = maxSystems;
}

bool Simulation::AtCap() const
{
    return (maxRegions_ > 0 && (int)openSlots_.size() >= maxRegions_) ||
           (maxSystems_ > 0 && (int)universe_.systems.size() >= maxSystems_);
}

double Simulation::TideThreshold() const
{
    const double share = maxRegions_ > 0 ? (double)openSlots_.size() / maxRegions_ : 0.0;
    return WorldRules::TideRules().threshold * (1.0 + share);
}

int Simulation::NextSlotFrom(int fromSlot) const
{
    if (fromSlot < 0 || fromSlot >= (int)skeleton_.slots.size())
        return -1;
    for (int n : skeleton_.slots[(size_t)fromSlot].neighbours)
        if (openSlots_.count(n) == 0 && skeleton_.slots[(size_t)n].kind != Gen::SlotKind::Centre)
            return n;
    return -1;
}

bool Simulation::IsEdgeRegion(int slot) const
{
    return openSlots_.count(slot) != 0 && NextSlotFrom(slot) >= 0;
}

int Simulation::MouthsAllowed() const
{
    return 1 + (int)sessions_.size() / std::max(1, WorldRules::TideRules().playersPerMouth);
}

int Simulation::YoungMouths() const
{
    const double life = WorldRules::TideRules().mouthHours * HOUR;
    int          n = 0;
    for (const RegionOpening& o : openings_)
        if (o.by != "world" && time_ - o.openedAt < life)
            n++;
    return n;
}

double Simulation::StrainOf(int slot) const
{
    return 0.5 + Gen::Rng(Gen::Key(regionSeed_, (uint64_t)slot, STRAIN_KEY)).Unit();
}

std::vector<std::string> Simulation::RegionSystems(int slot) const
{
    std::vector<std::string> out;
    for (const auto& info : universe_.systems)
        if (Gen::SlotOf(info.id) == slot)
            out.push_back(info.id);
    return out;
}

double Simulation::ExploredShare(int slot) const
{
    const std::vector<std::string> ids = RegionSystems(slot);
    if (ids.empty())
        return 0.0;
    double seen = 0.0;
    for (const std::string& id : ids)
        seen += explored_.count(id)   ? 1.0
                : surveyed_.count(id) ? WorldRules::TideRules().surveyedWeight
                                      : 0.0;
    return std::min(1.0, seen / (double)ids.size());
}

void Simulation::NoteSurvey(const std::string& systemId)
{
    const int slot = Gen::SlotOf(systemId);
    if (slot < 0 || openSlots_.count(slot) == 0)
        return;
    surveyed_.insert(systemId);
    TideOf(slot).surveys++;
}

Simulation::RegionTide& Simulation::TideOf(int slot)
{
    auto it = tides_.find(slot);
    if (it == tides_.end())
    {
        // A region begins part way into its first tide, as the seed says for its slot.
        it = tides_.emplace(slot, RegionTide{}).first;
        it->second.tide =
            TideThreshold() * Gen::Rng(Gen::Key(regionSeed_, (uint64_t)slot, START_KEY)).Unit();
    }
    return it->second;
}

void Simulation::StepTide(double passSeconds)
{
    if (skeleton_.slots.empty())
        return;  // a world of files, not of slots: nothing to grow into
    // Who is where, every pass: a pilot who crossed a region between two periods was there.
    for (const auto& sv : sessions_)
    {
        const int slot = Gen::SlotOf(sv.second.systemId);
        if (slot < 0)
            continue;
        playersSeen_[slot].insert(sv.first);
        explored_.insert(sv.second.systemId);
    }
    for (int slot : openSlots_)  // every edge region has one from the first pass
        if (IsEdgeRegion(slot))
            TideOf(slot);
    const WorldRules::Tide& r = WorldRules::TideRules();
    const int passes = std::max(1, (int)std::lround(r.period / std::max(0.001, passSeconds)));
    if (++tidePasses_ % passes != 0)
        return;
    std::map<int, std::set<int>> seen;
    seen.swap(playersSeen_);
    if (growthPaused_)
    {
        for (auto& kv : tides_)
            kv.second.surveys = 0;
        return;
    }

    // The edge, and how its time is shared out: the world grows at the pace of time however
    // long its edge is, and the seed staggers it so its regions do not all break at once.
    std::vector<int> edge;
    double           strain = 0.0;
    for (int slot : openSlots_)
        if (IsEdgeRegion(slot))
        {
            edge.push_back(slot);
            strain += StrainOf(slot);
        }
    double threshold = TideThreshold();
    for (int slot : edge)
    {
        RegionTide&  t = TideOf(slot);
        const double time = strain > 0.0 ? r.base * StrainOf(slot) / strain : 0.0;
        const auto   who = seen.find(slot);
        const int players = who == seen.end() ? 0 : std::min((int)who->second.size(), r.playerCap);
        const double share = ExploredShare(slot);
        const double explore =
            r.perExplore * r.exploreKnee * std::pow(share / r.exploreKnee, r.exploreCurve);
        const double surveys = r.perSurvey * t.surveys;
        t.tide += time + r.perPlayer * players + explore + surveys;
        t.fromTime += time;
        t.fromPlayers += r.perPlayer * players;
        t.fromExplore += explore;
        t.fromSurveys += surveys;
    }
    for (auto& kv : tides_)
        kv.second.surveys = 0;

    // Where it breaks through: the most strained first, then the lower slot.
    std::vector<int> ready;
    for (int slot : edge)
        if (tides_[slot].tide >= threshold)
            ready.push_back(slot);
    std::sort(ready.begin(), ready.end(),
              [this](int a, int b)
              {
                  const double ta = tides_.at(a).tide, tb = tides_.at(b).tide;
                  return ta != tb ? ta > tb : a < b;
              });
    for (int slot : ready)
    {
        RegionTide& t = tides_[slot];
        threshold = TideThreshold();
        if (t.tide < threshold)
            continue;
        if (AtCap())
        {
            // Nowhere to go: kept for the roaming holes (S9), and the region waits at the
            // threshold rather than gathering for ever.
            surplusTide_ += t.tide - threshold;
            t.tide = threshold;
            continue;
        }
        // Held back -- the world has its young mouths, or this region opened one lately: it
        // waits at the threshold, so a crowd held back for a day does not break through
        // everywhere at once when it may.
        if (YoungMouths() >= MouthsAllowed() || time_ - t.lastMouthAt < r.minGapHours * HOUR)
        {
            t.tide = threshold;
            continue;
        }
        const double      whole = t.fromTime + t.fromPlayers + t.fromExplore + t.fromSurveys;
        const std::string why = "time " + Percent(t.fromTime, whole) + ", players " +
                                Percent(t.fromPlayers, whole) + ", exploration " +
                                Percent(t.fromExplore, whole) + ", surveys " +
                                Percent(t.fromSurveys, whole);
        OpenNext(slot, "tide", why);
    }
}

// --- Opening -------------------------------------------------------------------------------

int Simulation::OpenNext(int fromSlot, const std::string& by, const std::string& why)
{
    const int slot = NextSlotFrom(fromSlot);
    if (slot < 0 || AtCap() || dataDir_.empty() || !OpenSlotFrom(slot, fromSlot, dataDir_))
        return -1;
    RegionOpening& o = openings_.back();
    o.by = by;
    o.why = why;
    const std::string mouth = o.mouth;
    MaterializeOpened(slot);
    RegionTide& t = tides_[fromSlot];
    t = RegionTide{};
    t.lastMouthAt = time_;
    Announce("tide", std::nullopt, mouth,
             "A wormhole tore open in " + SystemName(mouth) +
                 ": beyond it lies space nobody has charted");
    openedNow_.push_back(slot);
    return slot;
}

void Simulation::MaterializeOpened(int slot)
{
    std::vector<const WorldLoader::SystemInfo*> fresh;
    for (const auto& info : universe_.systems)
        if (Gen::SlotOf(info.id) == slot && systems_.count(info.id) == 0)
        {
            SystemState& st = systems_[info.id];
            st.id = info.id;
            SeedAggregate(st, info);
            fresh.push_back(&info);
        }
    if (!materialized_)
        return;  // MaterializeAllSystems will build them with everything else
    for (const WorldLoader::SystemInfo* info : fresh)
        MaterializeSystem(*info, dataDir_ + "systems/");

    // The wormhole in the parent's system, for whoever is standing there: a gate is the
    // route graph's, so it never goes through AddStatic -- but it is a change to the layer,
    // and a delta says so before any snapshot shows it (#38).
    const RegionOpening& o = openings_.back();
    SystemState*         home = SystemById(o.mouth);
    const auto           mouths = wormholes_.find(o.mouth);
    if (home == nullptr || home->entities.empty() || mouths == wormholes_.end() ||
        mouths->second.empty())
        return;
    nlohmann::json doc = nlohmann::json::object();
    doc["gates"] = nlohmann::json::array({ nlohmann::json::parse(mouths->second.back()) });
    for (auto& e : WorldLoader::BuildSystem(doc))
    {
        if (e->GetKind() != EntityKind::Gate)
            continue;
        const int id = NextAgentId();
        e->SetId(id);
        home->entities.push_back(std::move(e));
        home->pendingAdded.insert(id);
        home->layoutRev++;
    }
    home->profile = ProfileOf(*home);
}

std::vector<int> Simulation::TakeOpenedRegions()
{
    std::vector<int> out;
    out.swap(openedNow_);
    return out;
}

int Simulation::OpenMouthNow(int fromSlot, std::string& why)
{
    why.clear();
    if (skeleton_.slots.empty())
    {
        why = "this world has no skeleton to grow into";
        return -1;
    }
    if (fromSlot < 0)
    {
        // The edge region nearest to breaking through.
        double best = -1.0;
        for (int slot : openSlots_)
            if (IsEdgeRegion(slot))
            {
                const auto   t = tides_.find(slot);
                const double tide = t == tides_.end() ? 0.0 : t->second.tide;
                if (tide > best)
                {
                    best = tide;
                    fromSlot = slot;
                }
            }
        if (fromSlot < 0)
        {
            why = "no open region has a free slot beside it";
            return -1;
        }
    }
    if (openSlots_.count(fromSlot) == 0)
    {
        why = "region " + std::to_string(fromSlot) + " is not open";
        return -1;
    }
    if (AtCap())
    {
        why = "the world is at its cap (" + std::to_string(maxRegions_) + " regions, " +
              std::to_string(maxSystems_) + " systems)";
        return -1;
    }
    const int slot = OpenNext(fromSlot, "admin", std::string());
    if (slot < 0)
        why = "region " + std::to_string(fromSlot) + " has no free slot beside it";
    return slot;
}

bool Simulation::RegenerateRegion(int slot, std::string& why)
{
    why.clear();
    const RegionOpening* logged = nullptr;
    for (const RegionOpening& o : openings_)
        if (o.slot == slot)
            logged = &o;
    if (logged == nullptr)
    {
        why = "region " + std::to_string(slot) + " is not open";
        return false;
    }
    // A player's structure stands somewhere in the region as it is: remade, the place might
    // be inside a planet, or not be there at all.
    int built = 0;
    for (const std::string& id : RegionSystems(slot))
        if (const SystemState* st = SystemById(id))
            for (const auto& e : st->entities)
            {
                FactionId f;
                if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty() &&
                    !Outposts::FactionOf(e->GetOwner(), f))
                    built++;
            }
    if (built > 0)
    {
        why = "region " + std::to_string(slot) + " has " + std::to_string(built) +
              " player structure(s) in it";
        return false;
    }
    Gen::RegionParams params;
    nlohmann::json    homeDoc;
    if (!ParamsFor(slot, logged->mouth, dataDir_ + "systems/", params, homeDoc))
    {
        why = "its wormhole's system " + logged->mouth + " is not in this galaxy";
        return false;
    }
    const Gen::Region region = Gen::GenerateRegion(params);
    // Every region opened from this one has its wormhole in a system of it.
    for (const RegionOpening& o : openings_)
        if (o.parent == slot && region.documents.count(o.mouth) == 0)
        {
            why = "region " + std::to_string(o.slot) + " opened from " + o.mouth +
                  ", which the region remade by these rules does not have";
            return false;
        }
    Seal(region);
    return true;
}
