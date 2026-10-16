// Admin packs (#342 S7): hand-made regions and systems woven into a new world, kept as they
// were woven, and named when the files they came from change. The format and the weaving
// rules are gen/Pack.h's; this is where the galaxy takes them in.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"

#include "gen/Pack.h"
#include "gen/Region.h"
#include "raylib.h"
#include <nlohmann/json.hpp>

#include <cstdlib>

void Simulation::SetPacks(std::vector<std::shared_ptr<const Gen::Pack>> packs,
                          std::vector<Gen::Woven>                       woven)
{
    packs_ = std::move(packs);
    woven_ = std::move(woven);
}

const Gen::Pack* Simulation::PackById(const std::string& id) const
{
    for (const auto& p : packs_)
        if (p->id == id)
            return p.get();
    return nullptr;
}

namespace
{
// An open region with its documents, as a pack's admin links are woven against: what is in
// each system -- with the mouths beside it -- so a new gate stays clear of what is there.
Gen::Region WithDocuments(int slot, const WorldLoader::Universe& universe,
                          const std::map<std::string, std::string>&              docs,
                          const std::map<std::string, std::vector<std::string>>& mouths)
{
    Gen::Region r;
    r.slot = slot;
    for (const auto& info : universe.systems)
    {
        const auto doc = docs.find(info.id);
        if (Gen::SlotOf(info.id) != slot || doc == docs.end())
            continue;
        r.systems.push_back({ { "id", info.id },
                              { "name", info.name },
                              { "map", nlohmann::json::array({ info.mapPos.x, info.mapPos.y }) } });
        nlohmann::json d = nlohmann::json::parse(doc->second, nullptr, false);
        if (!d.is_object())
            d = nlohmann::json::object();
        const auto m = mouths.find(info.id);
        if (m != mouths.end())
            for (const std::string& gate : m->second)
                d["gates"].push_back(nlohmann::json::parse(gate, nullptr, false));
        r.documents[info.id] = std::move(d);
    }
    return r;
}
}  // namespace

void Simulation::WeaveRegionPacks()
{
    for (const auto& p : packs_)
    {
        if (p->kind != "region")
            continue;
        const size_t before = packProblems_.size();
        // Each into a free slot beside what is open by now -- a pack woven earlier included,
        // in id order, so the same packs make the same world.
        const int slot = Gen::PlaceRegionPack(*p, skeleton_, openSlots_, {}, packProblems_);
        if (slot < 0)
            continue;
        std::vector<Gen::Region> beside;
        for (int n : skeleton_.slots[(size_t)slot].neighbours)
            if (openSlots_.count(n) != 0)
                beside.push_back(WithDocuments(n, universe_, regionDocs_, wormholes_));
        Gen::Region r = Gen::WeaveRegionPack(*p, skeleton_, slot, beside, packProblems_);
        if (packProblems_.size() != before)
            continue;
        Seal(r);
        AddRegion(r);
        LogOpening(slot, std::string(), "pack", p->id);
        woven_.push_back({ p->id, p->kind, p->hash, slot, std::string() });
    }
}

bool Simulation::OpenPackRegion(int slot, const std::string& id)
{
    Gen::Region r;
    const auto  kept = sealedIn_.find(slot);
    if (kept != sealedIn_.end() && ReadSealedText(kept->second, regionSeed_, r) && r.pack == id)
        sealedIn_.erase(kept);  // as it was woven, whatever the pack says now
    else
    {
        // Not sealed (a world kept without its regions, a test): woven again, which gives the
        // same region as long as the pack and what is beside it are as they were.
        const Gen::Pack* p = PackById(id);
        if (p == nullptr)
        {
            TraceLog(LOG_WARNING,
                     "World: region %d is pack '%s', which is neither sealed nor in data/packs/ "
                     "-- not opened",
                     slot, id.c_str());
            return false;
        }
        std::vector<Gen::Region> beside;
        if (slot >= 0 && slot < (int)skeleton_.slots.size())
            for (int n : skeleton_.slots[(size_t)slot].neighbours)
                if (openSlots_.count(n) != 0)
                    beside.push_back(WithDocuments(n, universe_, regionDocs_, wormholes_));
        std::vector<std::string> problems;
        r = Gen::WeaveRegionPack(*p, skeleton_, slot, beside, problems);
        for (const std::string& why : problems)
            TraceLog(LOG_WARNING, "World: %s", why.c_str());
        if (!problems.empty())
            return false;
        Seal(r);
    }
    AddRegion(r);
    LogOpening(slot, std::string(), "pack", id);
    return true;
}

void Simulation::ApplySystemPacks(Gen::Region& region)
{
    for (Gen::Woven& w : woven_)
    {
        if (w.kind != "system" || w.slot != region.slot)
            continue;
        const Gen::Pack* p = PackById(w.id);
        if (p == nullptr)
        {
            TraceLog(LOG_WARNING,
                     "World: system pack '%s' is not in data/packs/ -- region %d "
                     "is made without it",
                     w.id.c_str(), region.slot);
            continue;
        }
        const std::string        target = Gen::SystemPackTarget(*p, region, regionSeed_, w.system);
        std::vector<std::string> problems;
        if (target.empty())
            problems.push_back(p->id + ": region " + std::to_string(region.slot) + " has " +
                               (w.system.empty() ? "no system it can take the place of"
                                                 : "no system " + w.system));
        else if (Gen::ApplySystemPack(*p, region, target, problems))
            w.system = target;
        for (const std::string& why : problems)
        {
            TraceLog(LOG_WARNING, "World: %s", why.c_str());
            packProblems_.push_back(why);
        }
    }
}

void Simulation::AddStarts(const Gen::Region& region)
{
    for (const std::string& id : region.starts)
    {
        if (IsStart(id))
            continue;
        StartPoint sp;
        sp.systemId = id;
        sp.systemName = id;
        for (const nlohmann::json& s : region.systems)
            if (s.value("id", std::string()) == id)
            {
                sp.systemName = s.value("name", id);
                sp.owner = s.value("owner", std::string());
            }
        const auto doc = region.documents.find(id);
        if (doc != region.documents.end())
            sp.station = Gen::DockingStation(doc->second);
        starts_.push_back(sp);
    }
}

std::vector<std::string> Simulation::PackChanges() const
{
    std::vector<std::string> out;
    for (const Gen::Woven& w : woven_)
    {
        const Gen::Pack* p = PackById(w.id);
        if (p == nullptr)
            out.push_back("pack '" + w.id +
                          "' is no longer in data/packs/ -- the world keeps it as it was woven");
        else if (p->hash != w.hash)
            out.push_back("pack '" + w.id + "' has changed since this world was made (" +
                          Gen::PackHashText(w.hash) + ", now " + Gen::PackHashText(p->hash) +
                          ") -- the world keeps it as it was woven; a pack is woven only into "
                          "a new world");
    }
    for (const auto& p : packs_)
    {
        bool woven = false;
        for (const Gen::Woven& w : woven_)
            woven = woven || w.id == p->id;
        if (!woven)
            out.push_back("pack '" + p->id +
                          "' was added after this world was made -- not woven; a pack is woven "
                          "only into a new world");
    }
    return out;
}

void Simulation::SavePacks(nlohmann::json& j) const
{
    if (woven_.empty())
        return;
    nlohmann::json list = nlohmann::json::array();
    for (const Gen::Woven& w : woven_)
    {
        nlohmann::json e = { { "id", w.id },
                             { "kind", w.kind },
                             { "hash", Gen::PackHashText(w.hash) },
                             { "slot", w.slot } };
        if (!w.system.empty())
            e["system"] = w.system;
        list.push_back(std::move(e));
    }
    j["packs"] = std::move(list);
}

void Simulation::ReadPacks(const nlohmann::json& j, std::vector<Gen::Woven>& out)
{
    out.clear();
    if (!j.contains("packs") || !j["packs"].is_array())
        return;
    for (const nlohmann::json& e : j["packs"])
    {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string())
            continue;
        Gen::Woven w;
        w.id = e["id"].get<std::string>();
        w.kind = e.value("kind", std::string("region"));
        w.hash = std::strtoull(e.value("hash", std::string("0")).c_str(), nullptr, 16);
        w.slot = e.value("slot", -1);
        w.system = e.value("system", std::string());
        out.push_back(std::move(w));
    }
}
