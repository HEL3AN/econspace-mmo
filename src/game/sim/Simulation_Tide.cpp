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
// A mouth the tide opens is unstable (#342 S4): only small ships pass, and a day after it
// opened it collapses unless somebody has finished an Anchor at it, which makes it permanent
// gates named after whoever built it. A mouth that collapses leaves its region drifting: in
// the log, with everything built in it, cut off -- and the next mouth from the same parent
// leads there again. Everything here is decided by the clock and the log, in the coarse
// pass, whether anybody is there to see it or not.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/SaveFile.h"
#include "sim/WorldRules.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/ShipDesign.h"
#include "core/WorldLoader.h"
#include "entities/JumpGate.h"
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
constexpr double   HOUR = 3600.0;
// What keys where a faction's Anchor goes up at a mouth, besides who, where and which.
constexpr uint64_t ANCHOR_KEY = 0xA7C40u;
// What keys whether a slot that opens takes an archived region back, and which.
constexpr uint64_t RETURN_KEY = 0x7E7D27u;
// An Anchor's spot is tried this many times before a faction gives the mouth up.
constexpr int ANCHOR_TRIES = 24;

std::string Hours(double seconds)
{
    const long long m = (long long)std::lround(std::max(0.0, seconds) / 60.0);
    return std::to_string(m / 60) + " h " + (m % 60 < 10 ? "0" : "") + std::to_string(m % 60) +
           " min";
}

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
    return Gen::ReadSealedRegion(text, seed, out);
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
    const nlohmann::json doc = { { "schema", Gen::SEAL_SCHEMA },
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
    // A region drifting back from the archive (S4) is sealed as it comes back: what it is
    // now is what it will be read as.
    region = returning_ != nullptr ? *returning_ : Gen::GenerateRegion(params);
    // The admin's systems put into it (S7) are part of what it is made as.
    if (returning_ == nullptr)
        ApplySystemPacks(region);
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
    o.mouthAt = time_;
    o.lastVisit = time_;  // nobody has been in it for no time yet
    openings_.push_back(o);
}

std::string Simulation::EntryOf(int slot)
{
    return Gen::SystemId(slot, 1, 1);
}

const Simulation::RegionOpening* Simulation::OpeningOf(int slot) const
{
    for (auto it = openings_.rbegin(); it != openings_.rend(); ++it)
        if (it->slot == slot)
            return it->state == "archived" || it->state == "returned" ? nullptr : &*it;
    return nullptr;
}

Simulation::RegionOpening* Simulation::LiveOpening(int slot)
{
    return const_cast<RegionOpening*>(OpeningOf(slot));
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
            // S4: a world from before it has none, and every mouth in it stays.
            o.mouthAt = oj.value("mouthAt", o.openedAt);
            o.closesAt = oj.value("closesAt", 0.0);
            o.warned = oj.value("warned", false);
            o.anchoredBy = oj.value("anchoredBy", std::string());
            o.anchoredAt = oj.value("anchoredAt", 0.0);
            o.lastVisit = oj.value("lastVisit", o.openedAt);
            o.fading = oj.value("fading", false);
            o.archive = oj.value("archive", -1);
            h.log.push_back(o);
        }
    ReadPacks(j, h.packs);
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
        oj["mouthAt"] = o.mouthAt;
        oj["lastVisit"] = o.lastVisit;
        if (o.closesAt > 0.0)
            oj["closesAt"] = o.closesAt;
        if (o.warned)
            oj["warned"] = true;
        if (!o.anchoredBy.empty())
        {
            oj["anchoredBy"] = o.anchoredBy;
            oj["anchoredAt"] = o.anchoredAt;
        }
        if (o.fading)
            oj["fading"] = true;
        if (o.archive >= 0)
            oj["archive"] = o.archive;
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
    SaveArchive(j);  // the regions that drifted away (S4)
    SaveHoles(j);    // the roaming holes the epochs decided (S9)
    SavePacks(j);    // where the admin's packs went (S7)
}

void Simulation::LoadGrowth(const nlohmann::json& j)
{
    LoadArchive(j);
    LoadHoles(j);
    if (woven_.empty())  // a world opened without its header's packs (a test) keeps them
        ReadPacks(j, woven_);
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
    // Cut off, a region has no tide of its own: nothing passes through it to strain it (S4).
    const RegionOpening* o = OpeningOf(slot);
    return openSlots_.count(slot) != 0 && (o == nullptr || o->state != "drifted") &&
           MouthTargetFrom(slot) >= 0;
}

int Simulation::MouthTargetFrom(int fromSlot) const
{
    // The region that drifted from here is where the next mouth leads (S4), until somebody
    // anchors it; the lowest of them if more than one did.
    int drifted = -1;
    for (const RegionOpening& o : openings_)
        if (o.parent == fromSlot && o.state == "drifted" && OpeningOf(o.slot) == &o &&
            (drifted < 0 || o.slot < drifted))
            drifted = o.slot;
    return drifted >= 0 ? drifted : NextSlotFrom(fromSlot);
}

int Simulation::MouthsAllowed() const
{
    return 1 + (int)sessions_.size() / std::max(1, WorldRules::TideRules().playersPerMouth);
}

int Simulation::YoungMouths() const
{
    // Unstable, or -- where mouths are permanent at once -- under a mouth's life old.
    const double life = WorldRules::TideRules().mouthHours * HOUR;
    int          n = 0;
    for (const RegionOpening& o : openings_)
        if (o.by != "world" && o.by != "pack" &&
            (o.state == "unstable" || (o.state == "open" && time_ - o.mouthAt < life)))
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
        // ...and a region somebody comes to is not dead (S4).
        if (RegionOpening* o = LiveOpening(slot))
        {
            o->lastVisit = time_;
            if (o->fading)
            {
                o->fading = false;
                Record("drift", std::nullopt, sv.second.systemId,
                       "The region beyond " + SystemName(o->mouth) +
                           " holds: a pilot came, and it no longer drifts away");
            }
        }
    }
    // Mouths failing and collapsing, every pass: a collapse is on the clock, not the period.
    StepMouths();
    // ...and roaming holes opening and closing (S9), on the clock too.
    StepHoles();
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
    // At the cap, the regions nobody comes to drift away (S4) -- before the tide, so a slot
    // one frees can be opened in the same period.
    StepDrift();

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
        // Finding a drifted region again opens no slot, so the cap does not hold it back (S4).
        const RegionOpening* target = OpeningOf(MouthTargetFrom(slot));
        const bool           again = target != nullptr && target->state == "drifted";
        if (!again && AtCap())
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
    const int slot = MouthTargetFrom(fromSlot);
    if (slot < 0 || dataDir_.empty())
        return -1;
    const RegionOpening* drifted = OpeningOf(slot);
    if (drifted != nullptr && drifted->state == "drifted")
    {
        // The next mouth from the parent finds the region that drifted from it (S4).
        if (ReopenRegion(slot, by, why) < 0)
            return -1;
    }
    else
    {
        if (AtCap())
            return -1;
        // A slot that opens may take back a region that drifted away at the cap instead of a
        // fresh one (S4): drawn from where and how many openings in, never from when.
        bool back = false;
        if (!archives_.empty())
        {
            Gen::Rng rng(
                Gen::Key(regionSeed_, RETURN_KEY, (uint64_t)slot, (uint64_t)openings_.size()));
            if (rng.Unit() < WorldRules::ArchiveRules().returnChance)
            {
                auto pick = archives_.begin();
                std::advance(pick, rng.Range(0, (int)archives_.size() - 1));
                back = BringBack(pick->first, slot, fromSlot, by, why) >= 0;
            }
        }
        if (!back)
        {
            if (!OpenSlotFrom(slot, fromSlot, dataDir_))
                return -1;
            RegionOpening& o = openings_.back();
            o.by = by;
            o.why = why;
            // Unstable until anchored, unless mouths are permanent at once (S4).
            if (WorldRules::MouthRules().anchors)
            {
                o.state = "unstable";
                o.closesAt = time_ + WorldRules::TideRules().mouthHours * HOUR;
            }
            MaterializeOpened(slot);
            const std::string mouth = o.mouth;
            Announce("tide", std::nullopt, mouth,
                     "A wormhole tore open in " + SystemName(mouth) +
                         ": beyond it lies space nobody has charted" + NativesLine(slot) +
                         (o.state == "unstable"
                              ? ". Unanchored, it collapses in " + Hours(o.closesAt - time_)
                              : std::string()));
        }
        openedNow_.push_back(slot);
        PublicChartsChanged();
    }
    // Word of it reaches the factions nearby, and the expeditions begin (S6).
    SendWordOfMouth(slot);
    RegionTide& t = tides_[fromSlot];
    t = RegionTide{};
    t.lastMouthAt = time_;
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
            // A region back from the archive (S4): the names players gave it, and what had
            // become of what stood and lay in it, replayed as a save's changes are.
            const auto back = returnState_.find(info.id);
            if (back != returnState_.end())
            {
                const nlohmann::json r = nlohmann::json::parse(back->second);
                st.agg.givenName = r.value("name", std::string());
                st.agg.discoverer = r.value("discoverer", std::string());
                // Whose charts it was on (#431): a faction's stay public. A pilot's own
                // charts of it are by its old id, which is slice 15's to translate.
                FactionId by;
                if (Factions::Find(r.value("chartedBy", std::string()), by))
                    MarkCharted(st.agg, by);  // contact was made long ago
                const nlohmann::json& c = r.value("changes", nlohmann::json());
                if (c.is_object())
                {
                    for (const nlohmann::json& k : c.value("removed", nlohmann::json::array()))
                        if (k.is_string())
                            st.removedKeys.insert(k.get<std::string>());
                    st.nextAddedKey = std::max(1, c.value("nextAdded", 1));
                    st.restore = c.dump();
                }
            }
            fresh.push_back(&info);
        }
    returnState_.clear();
    if (!materialized_)
        return;  // MaterializeAllSystems will build them with everything else
    for (const WorldLoader::SystemInfo* info : fresh)
        MaterializeSystem(*info, dataDir_ + "systems/");

    // The wormhole in the parent's system, for whoever is standing there: a gate is the
    // route graph's, so it never goes through AddStatic -- but it is a change to the layer,
    // and a delta says so before any snapshot shows it (#38).
    PriceMarkets();  // the new systems' markets, and every route the mouth shortened
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
        DressCrossing(static_cast<JumpGate&>(*e), o);  // unstable, until anchored (S4)
        const int id = NextAgentId();
        e->SetId(id);
        home->entities.push_back(std::move(e));
        home->pendingAdded.insert(id);
        home->layoutRev++;
    }
    home->profile = ProfileOf(*home);
}

std::string Simulation::NativesLine(int slot) const
{
    const FactionId natives = NativesOf(slot);
    if (natives == Factions::Unaffiliated())
        return std::string();
    // Where they trade: their stations, as the region's documents have them.
    std::vector<std::string> where;
    for (const std::string& id : RegionSystems(slot))
    {
        const auto doc = regionDocs_.find(id);
        if (doc == regionDocs_.end())
            continue;
        const nlohmann::json sys = nlohmann::json::parse(doc->second, nullptr, false);
        if (!sys.is_object() || !sys.contains("stations") || !sys["stations"].is_array())
            continue;
        for (const nlohmann::json& st : sys["stations"])
            if (st.value("faction", std::string()) == Factions::Id(natives))
                where.push_back(st.value("name", std::string()) + " in " + SystemName(id));
    }
    if (where.empty())
        return std::string();
    std::string list;
    for (size_t i = 0; i < where.size(); i++)
        list += (i == 0 ? "" : (i + 1 == where.size() ? " and " : ", ")) + where[i];
    return " -- but not empty: the " + FactionName(natives) + " trade from " + list;
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
    const RegionOpening* target = OpeningOf(MouthTargetFrom(fromSlot));
    if (AtCap() && (target == nullptr || target->state != "drifted"))
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

// --- Mouths and Anchors (#342 S4) ----------------------------------------------------------

float Simulation::AnchorCost()
{
    const Blueprint* bp = Blueprints::Find(ANCHOR_BLUEPRINT);
    if (bp == nullptr || !bp->byFactions)
        return 0.0f;
    float cost = 0.0f;
    for (const auto& c : bp->cost)
        cost += (float)c.second;
    return cost;
}

bool Simulation::IsAnchor(const std::string& archetype)
{
    const Blueprint* bp = Blueprints::Find(ANCHOR_BLUEPRINT);
    return bp != nullptr && bp->archetype == archetype;
}

std::string Simulation::OwnerName(const std::string& owner) const
{
    FactionId f;
    return Outposts::FactionOf(owner, f) ? FactionName(f) : owner;
}

std::string Simulation::MouthProblem(const ClientSession& s, const Entity& g) const
{
    if (g.GetKind() != EntityKind::Gate || !static_cast<const JumpGate&>(g).IsUnstable() || !s.ship)
        return std::string();
    // What the ship weighs is its design's: the parts it is built from.
    const double mass = ShipMass(s);
    if (mass <= 0.0)
        return std::string();
    // A roaming hole has a class of its own (S9).
    if (const RoamingHole* h = HoleOfGate(g.GetId()))
        return mass <= h->maxMass ? std::string()
                                  : "the wormhole lets through only ships of up to " +
                                        std::to_string((int)h->maxMass) + " t, and yours is " +
                                        std::to_string((int)std::lround(mass)) + " t";
    const double most = WorldRules::MouthRules().maxMass;
    if (mass <= most)
        return std::string();
    return "the mouth is unstable: until it is anchored only ships of up to " +
           std::to_string((int)most) + " t pass, and yours is " +
           std::to_string((int)std::lround(mass)) + " t";
}

Simulation::RegionOpening* Simulation::CrossingOf(const std::string& a, const std::string& b) const
{
    for (const RegionOpening& o : openings_)
    {
        if (o.parent < 0 || o.state == "archived" || o.state == "returned")
            continue;
        const std::string entry = EntryOf(o.slot);
        if ((o.mouth == a && entry == b) || (o.mouth == b && entry == a))
            return const_cast<RegionOpening*>(&o);
    }
    return nullptr;
}

void Simulation::DressCrossing(JumpGate& g, const RegionOpening& o) const
{
    if (o.state == "unstable")
    {
        g.SetArchetype("gate.mouth");
        g.SetName("Unstable Mouth");
        g.SetClosesAt(o.closesAt);
    }
    else if (o.state == "anchored")
    {
        g.SetArchetype("gate.jump");
        g.SetName(o.anchoredBy + " Gate");  // named for whoever anchored it
        g.SetClosesAt(0.0);
    }
    else
    {
        g.SetArchetype("gate.jump");
        g.SetName("Wormhole");
        g.SetClosesAt(0.0);
    }
}

void Simulation::DressCrossings(SystemState& st)
{
    auto cut = [&](const std::unique_ptr<Entity>& e)
    {
        if (e->GetKind() != EntityKind::Gate)
            return false;
        JumpGate&            g = static_cast<JumpGate&>(*e);
        const RegionOpening* o = CrossingOf(st.id, g.GetDestination());
        if (o == nullptr || o->state == "open")
            return false;  // a gate as the data wrote it
        if (o->state == "drifted")
            return true;  // the mouth collapsed: there is nothing there now
        DressCrossing(g, *o);
        return false;
    };
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(), cut),
                      st.entities.end());
}

bool Simulation::CrossingGateAt(const std::string& systemId, const std::string& dest, Vector2& at,
                                float& size) const
{
    if (const SystemState* st = SystemById(systemId))
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Gate &&
                static_cast<const JumpGate&>(*e).GetDestination() == dest)
            {
                at = e->GetPosition();
                size = e->GetSize();
                return true;
            }
    // Not built, or cut: where the documents put it -- the mouth's beside home, the way back
    // in the region's own.
    auto from = [&](const nlohmann::json& g)
    {
        if (!g.is_object() || g.value("destination", std::string()) != dest || !g.contains("pos"))
            return false;
        at = { g["pos"][0].get<float>(), g["pos"][1].get<float>() };
        size = g.value("size", 900.0f);
        return true;
    };
    const auto mouths = wormholes_.find(systemId);
    if (mouths != wormholes_.end())
        for (const std::string& text : mouths->second)
            if (from(nlohmann::json::parse(text, nullptr, false)))
                return true;
    const auto doc = regionDocs_.find(systemId);
    if (doc == regionDocs_.end())
        return false;
    const nlohmann::json sys = nlohmann::json::parse(doc->second, nullptr, false);
    if (sys.is_object() && sys.contains("gates") && sys["gates"].is_array())
        for (const nlohmann::json& g : sys["gates"])
            if (from(g))
                return true;
    return false;
}

void Simulation::RedressMouth(const RegionOpening& o)
{
    const std::string entry = EntryOf(o.slot);
    for (const auto& [here, there] :
         { std::make_pair(o.mouth, entry), std::make_pair(entry, o.mouth) })
    {
        SystemState* st = SystemById(here);
        if (st == nullptr)
            continue;
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Gate &&
                static_cast<const JumpGate&>(*e).GetDestination() == there)
            {
                DressCrossing(static_cast<JumpGate&>(*e), o);
                MarkStaticChanged(*st, e->GetId());
            }
    }
}

void Simulation::CutMouth(const RegionOpening& o)
{
    const std::string entry = EntryOf(o.slot);
    auto&             links = universe_.links;
    links.erase(
        std::remove_if(
            links.begin(), links.end(), [&](const WorldLoader::SystemLink& l)
            { return (l.a == o.mouth && l.b == entry) || (l.a == entry && l.b == o.mouth); }),

        links.end());
    IndexLinks();
    // A gate is the route graph's, so it never goes through RemoveStatic -- but it is a change
    // to the layer, and a delta says so before any snapshot shows it gone (#38).
    for (const auto& [here, there] :
         { std::make_pair(o.mouth, entry), std::make_pair(entry, o.mouth) })
    {
        SystemState* st = SystemById(here);
        if (st == nullptr)
            continue;
        for (auto it = st->entities.begin(); it != st->entities.end();)
        {
            if ((*it)->GetKind() != EntityKind::Gate ||
                static_cast<const JumpGate&>(**it).GetDestination() != there)
            {
                ++it;
                continue;
            }
            const int id = (*it)->GetId();
            it = st->entities.erase(it);
            st->pendingAdded.erase(id);
            st->pendingChanged.erase(id);
            st->pendingRemoved.insert(id);
            st->layoutRev++;
        }
        st->profile = ProfileOf(*st);
    }
}

void Simulation::RestoreMouth(const RegionOpening& o)
{
    const std::string entry = EntryOf(o.slot);
    universe_.links.push_back({ o.mouth, entry });
    IndexLinks();
    // The two gates as the documents have them -- the mouth's beside home, the way back in the
    // region's own -- dressed for what the mouth is now.
    auto gateJson = [&](const std::string& here, const std::string& there)
    {
        const auto mouths = wormholes_.find(here);
        if (mouths != wormholes_.end())
            for (const std::string& text : mouths->second)
            {
                const nlohmann::json g = nlohmann::json::parse(text, nullptr, false);
                if (g.is_object() && g.value("destination", std::string()) == there)
                    return g;
            }
        const auto doc = regionDocs_.find(here);
        if (doc != regionDocs_.end())
        {
            const nlohmann::json sys = nlohmann::json::parse(doc->second, nullptr, false);
            if (sys.is_object() && sys.contains("gates") && sys["gates"].is_array())
                for (const nlohmann::json& g : sys["gates"])
                    if (g.value("destination", std::string()) == there)
                        return g;
        }
        return nlohmann::json();
    };
    for (const auto& [here, there] :
         { std::make_pair(o.mouth, entry), std::make_pair(entry, o.mouth) })
    {
        SystemState*         st = SystemById(here);
        const nlohmann::json g = gateJson(here, there);
        if (st == nullptr || st->entities.empty() || g.is_null())
            continue;  // not built yet: it is dressed when it is
        nlohmann::json doc = { { "gates", nlohmann::json::array({ g }) } };
        for (auto& e : WorldLoader::BuildSystem(doc))
        {
            if (e->GetKind() != EntityKind::Gate)
                continue;
            DressCrossing(static_cast<JumpGate&>(*e), o);
            const int id = NextAgentId();
            e->SetId(id);
            st->entities.push_back(std::move(e));
            st->pendingAdded.insert(id);
            st->layoutRev++;
        }
        st->profile = ProfileOf(*st);
    }
}

std::string Simulation::AnchorAt(const RegionOpening& o, bool built) const
{
    const Blueprint* bp = Blueprints::Find(ANCHOR_BLUEPRINT);
    if (bp == nullptr || o.parent < 0)
        return std::string();
    const std::string entry = EntryOf(o.slot);
    for (const auto& [here, there] :
         { std::make_pair(o.mouth, entry), std::make_pair(entry, o.mouth) })
    {
        const SystemState* st = SystemById(here);
        Vector2            at{ 0.0f, 0.0f };
        float              size = 0.0f;
        if (st == nullptr || !CrossingGateAt(here, there, at, size))
            continue;
        for (const auto& e : st->entities)
        {
            if (e->GetKind() != EntityKind::Structure)
                continue;
            const Structure& t = static_cast<const Structure&>(*e);
            const float      d = std::hypot(t.GetPosition().x - at.x, t.GetPosition().y - at.y);
            if (IsAnchor(t.GetBuilds()) && (!built || !t.IsBuilding()) && d - size <= bp->mouth)
                return t.GetOwner();
        }
    }
    return std::string();
}

const Simulation::RegionOpening* Simulation::MouthNear(const SystemState& st, Vector2 at,
                                                       float within, const Entity** gate) const
{
    const RegionOpening* best = nullptr;
    float                nearest = within;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Gate || !static_cast<const JumpGate&>(*e).IsUnstable())
            continue;
        const float d =
            std::hypot(e->GetPosition().x - at.x, e->GetPosition().y - at.y) - e->GetSize();
        if (d > nearest)
            continue;
        const RegionOpening* o =
            CrossingOf(st.id, static_cast<const JumpGate&>(*e).GetDestination());

        if (o == nullptr || o->state != "unstable")
            continue;
        best = o;
        nearest = d;
        if (gate != nullptr)
            *gate = e.get();
    }
    return best;
}

void Simulation::StepMouths()
{
    const WorldRules::Mouths& m = WorldRules::MouthRules();
    // By slot, not by reference: a collapse changes the galaxy, not the log's length.
    for (size_t i = 0; i < openings_.size(); i++)
    {
        RegionOpening& o = openings_[i];
        if (o.state != "unstable" || o.closesAt <= 0.0)
            continue;
        if (!o.warned && time_ >= o.closesAt - m.warnHours * HOUR && time_ < o.closesAt)
        {
            o.warned = true;
            Announce("mouth", std::nullopt, o.mouth,
                     "The mouth in " + SystemName(o.mouth) +
                         " is failing: unless anchored it "
                         "collapses in " +
                         Hours(o.closesAt - time_));
            for (auto& kv : sessions_)
                if (Gen::SlotOf(kv.second.systemId) == o.slot || kv.second.systemId == o.mouth)
                    kv.second.RecordEvent(Ev::Kind::Notice, "The mouth to " + SystemName(o.mouth) +
                                                                " collapses in " +
                                                                Hours(o.closesAt - time_) +
                                                                "; beyond it you will be cut off");
        }
        if (time_ >= o.closesAt)
            CollapseMouth(o);
    }
}

void Simulation::CollapseMouth(RegionOpening& o)
{
    o.state = "drifted";
    o.closesAt = 0.0;
    o.warned = false;
    CutMouth(o);
    TopologyChanged();
    Announce("mouth", std::nullopt, o.mouth,
             "The mouth in " + SystemName(o.mouth) + " collapsed unanchored: the region beyond " +
                 "drifts, cut off, until the tide finds it again");
    for (auto& kv : sessions_)
        if (Gen::SlotOf(kv.second.systemId) == o.slot)
            kv.second.RecordEvent(Ev::Kind::Notice,
                                  "The mouth home has collapsed: this region drifts, cut off, "
                                  "until the tide finds it again");
}

int Simulation::ReopenRegion(int slot, const std::string& by, const std::string& why)
{
    RegionOpening* o = LiveOpening(slot);
    if (o == nullptr || o->state != "drifted")
        return -1;
    o->by = by;
    o->why = why;
    o->mouthAt = time_;
    o->warned = false;
    o->state = WorldRules::MouthRules().anchors ? "unstable" : "open";
    o->closesAt = o->state == "unstable" ? time_ + WorldRules::TideRules().mouthHours * HOUR : 0.0;
    RestoreMouth(*o);
    TopologyChanged();
    Announce("tide", std::nullopt, o->mouth,
             "The tide found the drifting region again: a mouth reopened in " +
                 SystemName(o->mouth) +
                 (o->state == "unstable"
                      ? ", and collapses in " + Hours(o->closesAt - time_) + " unless anchored"
                      : std::string()));
    // An Anchor that was finished while it was cut off holds it at once.
    const std::string anchorer = AnchorAt(*o);
    if (o->state == "unstable" && !anchorer.empty())
        AnchorMouth(*o, anchorer);
    return slot;
}

void Simulation::AnchorMouth(RegionOpening& o, const std::string& owner)
{
    if (o.state != "unstable")
        return;
    o.state = "anchored";
    o.anchoredBy = OwnerName(owner);
    o.anchoredAt = time_;
    o.closesAt = 0.0;
    o.warned = false;
    RedressMouth(o);
    topologyChanged_ = true;  // the index marks it unstable no longer (#432)
    FactionId                f;
    std::optional<FactionId> who;
    if (Outposts::FactionOf(owner, f))
        who = f;
    Announce("anchor", who, o.mouth,
             o.anchoredBy + " anchored the mouth in " + SystemName(o.mouth) +
                 ": the way beyond holds, through the " + o.anchoredBy + " Gate");
    if (who)
        ChartBy(o.mouth, *who);  // a faction's Anchor: it is there (#431 K2)
    for (auto& kv : sessions_)
        if (!owner.empty() && kv.second.accountName == owner)
            kv.second.RecordEvent(Ev::Kind::Notice, "Your Anchor holds the mouth in " +
                                                        SystemName(o.mouth) + ": the " +
                                                        o.anchoredBy + " Gate is permanent");
}

void Simulation::AnchorLost(SystemState& st, const std::string& owner, const std::string& name,
                            const std::string& key, Vector2 at, bool wasSite, Loss cause,
                            FactionId attacker)
{
    FactionId                f;
    std::optional<FactionId> whose;
    if (Outposts::FactionOf(owner, f))
    {
        whose = f;
        minds_[f].lostAt[st.id] = time_;
        for (auto p = plans_.begin(); p != plans_.end(); ++p)
            if (p->second.kind == Plan::Kind::Anchor && p->second.target == st.id &&
                p->second.site == key)
            {
                plans_.erase(p);  // said here; its due entry finds nothing
                break;
            }
    }
    const std::string where = SystemName(st.id);
    const std::string how =
        cause == Loss::Siege ? " to a " + FactionName(attacker) + " siege" : std::string();
    if (wasSite)
    {
        Record("anchor", whose, st.id,
               OwnerName(owner) + " lost the Anchor it was building in " + where + how);
        return;
    }
    // The mouth it held, if no other Anchor holds it as well, is unstable again: a day to
    // anchor it anew, or it collapses.
    const Blueprint* bp = Blueprints::Find(ANCHOR_BLUEPRINT);
    for (RegionOpening& o : openings_)
    {
        if (o.state != "anchored" || o.parent < 0)
            continue;
        const std::string entry = EntryOf(o.slot);
        const std::string there = o.mouth == st.id ? entry : entry == st.id ? o.mouth : "";
        Vector2           gate{ 0.0f, 0.0f };
        float             size = 0.0f;
        if (there.empty() || bp == nullptr || !CrossingGateAt(st.id, there, gate, size) ||
            std::hypot(gate.x - at.x, gate.y - at.y) - size > bp->mouth || !AnchorAt(o).empty())
            continue;
        o.state = "unstable";
        o.closesAt = time_ + WorldRules::TideRules().mouthHours * HOUR;
        o.mouthAt = time_;
        o.warned = false;
        RedressMouth(o);
        topologyChanged_ = true;  // the index marks it unstable again (#432)
        Announce("anchor", whose, st.id,
                 "The " + name + " in " + where + " fell" + how + ": the mouth is unstable " +
                     "again, and collapses in " + Hours(o.closesAt - time_) +
                     " unless anchored anew");
        return;
    }
    Announce("anchor", whose, st.id, "The " + name + " in " + where + " fell" + how);
}

// The convoy is at the mouth (S6): what it brought goes down beside the gate. Checked against
// what is there now, as a settlement is on arrival -- a mouth anchored or collapsed meanwhile,
// or another's Anchor going up at it, is no mouth to anchor, and what was paid goes back.
bool Simulation::LayAnchorSite(Plan& p)
{
    RegionOpening*    o = LiveOpening(p.slot);
    const Blueprint*  bp = Blueprints::Find(ANCHOR_BLUEPRINT);
    const FactionId   f = p.faction;
    const std::string where = SystemName(p.target);
    auto              back = [&](const std::string& why)
    {
        minds_[f].stock += AnchorCost();
        Record("anchor", f, p.target,
               FactionName(f) + "'s Anchor convoy turned back from " + where + ": " + why);
        return false;
    };
    if (o == nullptr || o->mouth != p.target || o->state != "unstable" || bp == nullptr)
        return back("the mouth is not there to anchor");
    if (!AnchorAt(*o, false).empty())
        return back("an Anchor stands there already");
    SystemState* st = SystemById(o->mouth);
    Vector2      gate{ 0.0f, 0.0f };
    float        size = 0.0f;
    if (st == nullptr || !CrossingGateAt(o->mouth, EntryOf(p.slot), gate, size))
        return back("the mouth is not there to anchor");
    // Beside the gate, clear of it, within the blueprint's reach of the mouth: drawn from who,
    // where and which mouth, never from when.
    const Archetype* arch = Archetypes::Find(bp->archetype);
    const float      own = arch != nullptr ? arch->defaultSize : 0.0f;
    const float      inner = size + own + bp->clearance + 200.0f;
    const float      outer = std::max(inner, size + bp->mouth * 0.8f);
    Gen::Rng         rng(
        Gen::Key(ANCHOR_KEY, (uint64_t)f.Index(), Intelligence::KeyOf(o->mouth), (uint64_t)p.slot));
    bool    found = false;
    Vector2 at{ 0.0f, 0.0f };
    for (int i = 0; i < ANCHOR_TRIES && !found; i++)
    {
        const double turn = rng.Unit() * 2.0 * PI;
        const float  r = (float)rng.Between((double)inner, (double)outer);
        at = { gate.x + r * (float)std::cos(turn), gate.y + r * (float)std::sin(turn) };
        found = SpotProblem(*st, *bp, at).empty();
    }
    if (!found)
        return back("no room beside the gate");
    auto site = std::make_unique<Structure>(at, 0.0f, FactionName(f) + " Anchor", bp->archetype);
    site->SetBlueprint(bp->id);
    site->StartBuilding(time_, time_ + bp->buildSeconds);
    const int id = AddStatic(o->mouth, std::move(site), Outposts::OwnerOf(f));
    if (id == 0)
        return back("no room beside the gate");
    p.site = StaticKey(o->mouth, id);
    p.dueAt = time_ + bp->buildSeconds;
    due_.insert({ p.dueAt, p.id });
    Announce("anchor", f, o->mouth,
             FactionName(f) + " began an Anchor at the mouth in " + SystemName(o->mouth) +
                 ": done in " + Hours(bp->buildSeconds) + ", if the mouth lasts");
    return true;
}

void Simulation::ResolveAnchor(const Plan& p)
{
    // It anchored the mouth the moment it was finished (StepStructures); this only notices a
    // site that is not there any more, or not finished yet after all.
    SystemState* st = SystemById(p.target);
    if (st == nullptr)
        return;
    const Structure* site = StructureByKey(*st, p.site);
    if (site == nullptr)
        return;  // lost: AnchorLost said so when it went
    if (site->IsBuilding())
    {
        Plan later = p;
        later.dueAt = std::max(site->GetCompletesAt(), time_);
        plans_[later.id] = later;
        due_.insert({ later.dueAt, later.id });
    }
}
