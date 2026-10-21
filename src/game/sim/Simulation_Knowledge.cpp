// Who knows which systems (#431 slice 1; documents/world_knowledge_and_territory.md, K1-K6).
//
// A pilot's map is the union of three things the game already kept: the public charts, which
// the world records per system (SystemAggregate::publicChart, `chartedBy`); what the pilot has
// found, in the account beside what their scans resolved (ClientSession::known, "system:<id>");
// and their corporation's charts, in the registry (Corps::Corp::charts, corps.json).
//
// Nothing here runs per tick. The host asks for a pilot's index only when the pilot's
// KnowledgeKey -- the public charts' revision, the corporations', what the pilot found and
// what their corporation charted -- differs from the one it last sent.

#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include <nlohmann/json.hpp>

bool Simulation::PublicByPlace(const std::string& systemId) const
{
    return regionDocs_.count(systemId) == 0 || InCentre(systemId);
}

bool Simulation::OnPublicCharts(const std::string& systemId) const
{
    auto it = systems_.find(systemId);
    return it != systems_.end() ? it->second.agg.publicChart : PublicByPlace(systemId);
}

std::string Simulation::ChartedSystemOf(const std::string& key)
{
    static const std::string prefix = "system:";
    return key.rfind(prefix, 0) == 0 ? key.substr(prefix.size()) : std::string();
}

bool Simulation::ChartBy(const std::string& systemId, FactionId faction)
{
    auto it = systems_.find(systemId);
    if (it == systems_.end() || !it->second.agg.chartedBy.empty())
        return false;
    SystemAggregate& a = it->second.agg;
    a.chartedBy = Factions::Id(faction);
    a.publicChart = true;
    chartsChanged_ = true;  // world state, written at once
    PublicChartsChanged();
    return true;
}

bool Simulation::Knows(const ClientSession& s, const std::string& systemId) const
{
    if (OnPublicCharts(systemId) || s.known.count(ChartKey(systemId)) != 0)
        return true;
    const Corps::Corp* corp = s.accountName.empty() ? nullptr : corps_.Of(s.accountName);
    return corp != nullptr && corp->charts.count(systemId) != 0;
}

bool Simulation::Learn(ClientSession& s, const std::string& systemId)
{
    if (!HasSystem(systemId) || OnPublicCharts(systemId) ||
        !s.known.insert(ChartKey(systemId)).second)
        return false;
    TouchKnown(s);
    corps_.AddCharts(s.accountName, { systemId });
    return true;
}

void Simulation::ShareCharts(const ClientSession& s)
{
    std::vector<std::string> systems;
    for (const std::string& key : s.known)
    {
        const std::string id = ChartedSystemOf(key);
        if (!id.empty())
            systems.push_back(id);
    }
    corps_.AddCharts(s.accountName, systems);
}

std::vector<long long> Simulation::KnowledgeKey(const ClientSession& s) const
{
    const Corps::Corp* corp = s.accountName.empty() ? nullptr : corps_.Of(s.accountName);
    return { publicRev_, (long long)corps_.Rev(), s.knownRev,
             corp != nullptr ? corp->chartsRev : 0 };
}

WorldLoader::Universe Simulation::KnownUniverseFor(const ClientSession& s) const
{
    WorldLoader::Universe u = BuildIndex(&s);
    // An unstable mouth is a limited crossing like a hole (#342 S4): it says when it closes
    // and what it lets by, so a route planned on the map is the one the server will fly
    // (#432). Anchoring it makes it a gate; the index is sent again (TakeTopologyChanged).
    // By opening rather than by link: a mouth is rare and the links are the whole galaxy.
    for (const RegionOpening& o : openings_)
    {
        if (o.parent < 0 || o.state != "unstable")
            continue;
        const std::string entry = EntryOf(o.slot);
        for (WorldLoader::SystemLink& link : u.links)
            if ((link.a == o.mouth && link.b == entry) || (link.a == entry && link.b == o.mouth))
            {
                link.kind = "mouth";
                link.closesAt = o.closesAt;
                link.maxMass = (float)WorldRules::MouthRules().maxMass;
            }
    }
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

Proto::GalaxyState Simulation::GalaxyStateFor(const ClientSession&      s,
                                              const Proto::GalaxyState& all) const
{
    Proto::GalaxyState out;
    out.events = all.events;
    for (const Proto::GalaxySystemStat& g : all.systems)
        if (Knows(s, g.id))
            out.systems.push_back(g);
    return out;
}

void Simulation::LoadKnown(ClientSession& s)
{
    // A pre-#431 world's finds of theirs, kept in the world until now.
    auto legacy = s.accountName.empty() ? legacyKnown_.end() : legacyKnown_.find(s.accountName);
    if (legacy != legacyKnown_.end())
    {
        for (const std::string& id : legacy->second)
            if (!OnPublicCharts(id))
                s.known.insert(ChartKey(id));
        legacyKnown_.erase(legacy);
        chartsChanged_ = true;  // the world's copy is gone: written at once
    }
    TouchKnown(s);
    // Wherever they are, they know (K6): a pilot left in a system an older world charted
    // for everybody keeps it on their map.
    Learn(s, s.systemId);
    // ...and what they found while in no corporation, or before #431, is their
    // corporation's too.
    ShareCharts(s);
}

void Simulation::SaveLegacyKnown(nlohmann::json& j) const
{
    if (!legacyKnown_.empty())
        j["knownPending"] = legacyKnown_;
}

void Simulation::LoadLegacyKnown(const nlohmann::json& j)
{
    const nlohmann::json pending = j.value("knownPending", nlohmann::json());
    if (!pending.is_object())
        return;
    for (auto it = pending.begin(); it != pending.end(); ++it)
        if (it.value().is_array())
            for (const nlohmann::json& id : it.value())
                if (id.is_string() && HasSystem(id.get<std::string>()))
                    legacyKnown_[it.key()].insert(id.get<std::string>());
}
