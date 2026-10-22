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
#include "sim/FactionMind.h"
#include "sim/WorldRules.h"
#include "gen/Names.h"
#include "gen/Region.h"

#include "entities/Station.h"
#include "entities/Structure.h"

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

bool Simulation::CanChart(const std::string& systemId, FactionId faction) const
{
    const auto it = systems_.find(systemId);
    if (it == systems_.end() || PublicByPlace(systemId) || !it->second.agg.chartedBy.empty())
        return false;
    // A region's natives have always known their home; their charts are not the centres'.
    const int slot = Gen::SlotOf(systemId);
    return natives_.count(slot) == 0 || NativesOf(slot) != faction;
}

void Simulation::MarkCharted(SystemAggregate& a, FactionId faction)
{
    a.chartedBy = Factions::Id(faction);
    a.publicChart = true;
    chartsChanged_ = true;  // world state, written at once
    PublicChartsChanged();
}

bool Simulation::ChartBy(const std::string& systemId, FactionId faction)
{
    if (!CanChart(systemId, faction))
        return false;
    SystemState&     st = systems_[systemId];
    SystemAggregate& a = st.agg;
    // Nobody from outside before it (K4): no pilot found it, and no faction charted it, or
    // CanChart would have refused.
    const bool first = a.discoverer.empty();
    MarkCharted(a, faction);
    if (!first)
        return true;  // a pilot was here first: theirs to name, and their find was the news

    const std::string designation = SystemName(systemId);
    const std::string native = NativeNameOf(systemId);
    std::string       name = native;
    if (name.empty() && a.givenName.empty())
        name = Gen::ChartedName(faction, regionSeed_, systemId, TakenNames());
    if (!name.empty() && a.givenName.empty())
        CallSystem(st, name);
    const std::string who = FactionName(faction);
    Announce("contact", faction, systemId,
             !native.empty()
                 ? who + " made first contact in " + designation + ": its people call it " + native
             : !name.empty() ? who + " charted " + designation + " and named it " + name
                             : who + " charted " + designation);
    return true;
}

std::string Simulation::NativeNameOf(const std::string& systemId) const
{
    const auto it = natives_.find(Gen::SlotOf(systemId));
    if (it == natives_.end())
        return std::string();
    const nlohmann::json n = nlohmann::json::parse(it->second, nullptr, false);
    if (!n.is_object() || !n.contains("names") || !n["names"].is_object())
        return std::string();
    const nlohmann::json& names = n["names"];
    const auto            name = names.find(systemId);
    return name != names.end() && name->is_string() ? name->get<std::string>() : std::string();
}

void Simulation::CallSystem(SystemState& st, const std::string& name)
{
    st.agg.givenName = name;
    chartsChanged_ = true;  // written at once, and everyone who knows it sees the name
    PublicChartsChanged();
    // Its planets are called after it (#259), as when a pilot names it.
    WorldLoader::NamePlanets(st.entities, name);
}

std::set<std::string> Simulation::TakenNames() const
{
    std::set<std::string> taken;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
    {
        taken.insert(Gen::FoldName(info.name));
        taken.insert(Gen::FoldName(info.id));
    }
    for (const auto& kv : systems_)
        if (!kv.second.agg.givenName.empty())
            taken.insert(Gen::FoldName(kv.second.agg.givenName));
    return taken;
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

// The observed layer (#431 slice 14, M1): what stands in a system is known only as of the last
// time somebody whose eyes count for the pilot was there. A summary per system and per pilot
// (and per corporation) -- kind, name, owner -- never a copy of the system, and bounded.

WorldLoader::Sighting Simulation::SightingOf(const SystemState& st, const ClientSession* s) const
{
    WorldLoader::Sighting out;
    out.at = time_;
    for (const auto& e : st.entities)
    {
        WorldLoader::SeenObject o;
        if (e->GetKind() == EntityKind::Station)
        {
            const Station& station = static_cast<const Station&>(*e);
            o.kind = "station";
            o.owner = Outposts::OwnerOf(station.GetFaction());
        }
        else if (e->GetKind() == EntityKind::Structure)
        {
            const Structure& t = static_cast<const Structure&>(*e);
            o.kind = t.GetBuilds();
            o.owner = t.GetOwner();
            o.building = t.IsBuilding();
        }
        else
            continue;
        // A hidden base is the finder's alone (#346); a corporation's record shares none.
        if (s != nullptr ? HiddenFrom(*s, st, *e) : SignatureOf(*e) > 0.0f)
            continue;
        o.name = e->GetName();
        o.market = !o.building && e->Has(Component::Market);
        if (out.seen.size() < WorldLoader::Sighting::MAX_SEEN)
            out.seen.push_back(std::move(o));
        else
            out.more++;
    }
    return out;
}

WorldLoader::Sighting Simulation::LastSeen(const ClientSession& s,
                                           const std::string&   systemId) const
{
    WorldLoader::Sighting out;
    auto                  own = s.sightings.find(systemId);
    if (own != s.sightings.end())
        out = own->second;
    const Corps::Corp* corp = s.accountName.empty() ? nullptr : corps_.Of(s.accountName);
    if (corp != nullptr)
    {
        auto theirs = corp->sightings.find(systemId);
        if (theirs != corp->sightings.end() && theirs->second.at > out.at)
            out = theirs->second;
    }
    return out;
}

void Simulation::StepObservations()
{
    // Who has eyes where: every pilot in a system, and the owner of every structure standing
    // in one -- an account; a faction's is no pilot's -- and the corporations they are in.
    std::map<std::string, std::set<std::string>> accountsAt, corpsAt;
    for (const auto& kv : sessions_)
        if (!kv.second.accountName.empty())
            accountsAt[kv.second.systemId].insert(kv.second.accountName);
    for (const auto& kv : systems_)
        for (const auto& e : kv.second.entities)
        {
            FactionId f;
            if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty() &&
                !Outposts::FactionOf(e->GetOwner(), f))
                accountsAt[kv.first].insert(e->GetOwner());
        }
    for (const auto& [system, accounts] : accountsAt)
        for (const std::string& a : accounts)
            if (const Corps::Corp* c = corps_.Of(a))
                corpsAt[system].insert(c->id);

    // A corporation's record, kept for its members whether they are on or not.
    for (const auto& [system, corps] : corpsAt)
        if (const SystemState* st = SystemById(system))
        {
            const WorldLoader::Sighting now = SightingOf(*st, nullptr);
            for (const std::string& id : corps)
                corps_.Sight(id, system, now);
        }

    // Each pilot's own: wherever they, a corporation mate or a structure of theirs is.
    for (auto& kv : sessions_)
    {
        ClientSession&        s = kv.second;
        const Corps::Corp*    corp = s.accountName.empty() ? nullptr : corps_.Of(s.accountName);
        std::set<std::string> live;
        if (HasSystem(s.systemId))
            live.insert(s.systemId);
        for (const auto& [system, accounts] : accountsAt)
            if (!s.accountName.empty() && accounts.count(s.accountName) != 0)
                live.insert(system);
        if (corp != nullptr)
            for (const auto& [system, corps] : corpsAt)
                if (corps.count(corp->id) != 0)
                    live.insert(system);
        bool news = live != s.liveSystems;
        for (const std::string& system : live)
        {
            const SystemState* st = SystemById(system);
            if (st == nullptr)
                continue;
            const WorldLoader::Sighting now = SightingOf(*st, &s);
            WorldLoader::Sighting&      kept = s.sightings[system];
            news = news || !kept.Observed() || !kept.SameAs(now);
            kept = now;
        }
        if (news)
        {
            // Leaving is news too: the map turns from live to "as of" there.
            s.liveSystems = std::move(live);
            TouchKnown(s);
        }
    }
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
    // ...and where their corporation's front is open (#431 D3): held by another side, and
    // theirs to besiege or claim from next door.
    const Corps::Corp* corp = s.accountName.empty() ? nullptr : corps_.Of(s.accountName);
    for (const Proto::GalaxySystemStat& g : all.systems)
        if (Knows(s, g.id))
        {
            out.systems.push_back(g);
            const SystemState* st = corp != nullptr ? SystemById(g.id) : nullptr;
            if (st != nullptr)
            {
                const std::string holder = HolderOf(st->agg);
                out.systems.back().front =
                    !holder.empty() && holder != corp->id && FrontOpen(g.id, corp->id);
            }
        }
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
