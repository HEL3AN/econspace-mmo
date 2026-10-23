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
#include "sim/Names.h"
#include "sim/WorldRules.h"
#include "gen/Names.h"
#include "gen/Region.h"

#include "core/Archetype.h"
#include "entities/Station.h"
#include "entities/Structure.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
    // ...and what natives call their systems, before anyone has met them: a pilot who named
    // a system so would find the same name on another the day contact is made.
    for (const auto& kv : natives_)
    {
        const nlohmann::json n = nlohmann::json::parse(kv.second, nullptr, false);
        if (n.is_object() && n.contains("names") && n["names"].is_object())
            for (const auto& name : n["names"])
                if (name.is_string())
                    taken.insert(Gen::FoldName(name.get<std::string>()));
    }
    return taken;
}

std::string Simulation::NameProblem(const std::string& name) const
{
    std::string why;
    if (!Names::AllowedSystemName(name, blockedNames_, why))
        return why;
    if (TakenNames().count(Gen::FoldName(name)) != 0)
        return "there is already a system called " + name;
    return std::string();
}

std::string Simulation::SystemNamed(const std::string& which) const
{
    if (which.empty())
        return std::string();
    for (const WorldLoader::SystemInfo& info : universe_.systems)
        if (info.id == which)
            return info.id;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
    {
        const auto st = systems_.find(info.id);
        if (Names::SameName(info.id, which) || Names::SameName(info.name, which) ||
            (st != systems_.end() && Names::SameName(st->second.agg.givenName, which)))
            return info.id;
    }
    return std::string();
}

bool Simulation::RenameSystem(const std::string& which, const std::string& name, std::string& said)
{
    const std::string id = SystemNamed(which);
    auto              it = systems_.find(id);
    if (id.empty() || it == systems_.end())
    {
        said = "no system is called " + which;
        return false;
    }
    if (PublicByPlace(id))
    {
        said = SystemName(id) + " is in known space: its name is the world's (a pin, in the "
                                "editor), not a pilot's";
        return false;
    }
    SystemState&      st = it->second;
    SystemAggregate&  a = st.agg;
    const std::string before = SystemName(id);
    std::string       designation = id;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
        if (info.id == id)
            designation = info.name;
    // What it is called with no name given: its people's, once anyone has met them (K4).
    const bool        met = !a.discoverer.empty() || !a.chartedBy.empty();
    const std::string native = met ? NativeNameOf(id) : std::string();

    std::string now;
    if (name == "-")
    {
        if (a.givenName.empty() || a.givenName == native)
        {
            said = before + " has no name a pilot or a faction gave it";
            return false;
        }
        now = native;
    }
    else
    {
        // Only its case changing is its own name, not another system's.
        const bool  recased = Names::SameName(a.givenName, name) && a.givenName != name;
        std::string why;
        if (recased ? !Names::AllowedSystemName(name, blockedNames_, why)
                    : !(why = NameProblem(name)).empty())
        {
            said = why;
            return false;
        }
        now = name;
    }

    if (now.empty())
    {
        st.agg.givenName.clear();
        chartsChanged_ = true;
        PublicChartsChanged();
        WorldLoader::NamePlanets(st.entities, designation);
    }
    else
        CallSystem(st, now);
    const std::string after = now.empty() ? designation : now;
    Announce("renamed", std::nullopt, id,
             "The server's admin renamed " + before + " " + after +
                 (designation != before && designation != after ? " (" + designation + ")" : ""));
    // Its discoverer is told, whoever named it: the find is theirs, and with the name cleared
    // so is the naming again.
    if (!a.discoverer.empty())
        TellAccount(a.discoverer, Ev::Kind::Notice,
                    "The server's admin renamed " + before + ", which you found, " + after +
                        (now.empty() && native.empty() ? ": you may name it again" : ""));
    said = before + " is now " + after;
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
            news = news || !kept.Observed() || kept.outdated || !kept.SameAs(now);
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
    // Docked at a faction's station: what it would pay for each of their charts (slice 16).
    FactionId buyer;
    if (ChartBuyer(s, buyer))
    {
        out.chartBuyer = true;
        out.chartFaction = buyer;
        out.chartTreasury = std::floor(minds_[buyer].treasury);
        for (const ChartOffer& o : ChartOffers(s, buyer))
            for (Proto::GalaxySystemStat& g : out.systems)
                if (g.id == o.system)
                {
                    g.chartPrice = o.price;
                    g.chartWhat = o.What();
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

// --- Charts sold to factions (#346, #431 K5 and slice 16) -------------------------------------
//
// A faction pays for what changes its picture. What a pilot sells is written into the
// faction's intel -- the same record a survey of its own writes -- so what it already knows is
// worth nothing and cannot be sold to it again, an update pays only for what is new, and the
// faction decides on what it bought (where to survey, settle or besiege) like on anything else
// it saw. Another faction's picture is its own: it may still pay.

std::string Simulation::ChartOffer::What() const
{
    std::string text;
    auto        add = [&](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
    if (news)
        add("new system");
    if (changed > 0)
        add(changed == 1 ? std::string("1 change") : std::to_string(changed) + " changes");
    if (finds > 0)
        add(finds == 1 ? std::string("1 hidden find") : std::to_string(finds) + " hidden finds");
    return text.empty() ? std::string("nothing it lacks") : text;
}

bool Simulation::ChartBuyer(const ClientSession& s, FactionId& f) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.IsDocked() || s.accountName.empty())
        return false;
    for (const auto& e : st->entities)
        if (e->GetId() == s.dockedStationId)
        {
            if (e->GetKind() != EntityKind::Station)
                return false;  // a structure's dock is a player's, not a faction's desk
            f = static_cast<const Station&>(*e).GetFaction();
            return f != Factions::Unaffiliated();
        }
    return false;
}

Simulation::ChartReport Simulation::ReportOf(const ClientSession& s,
                                             const std::string&   systemId) const
{
    ChartReport        r;
    const SystemState* st = SystemById(systemId);
    if (st == nullptr)
        return r;
    // Only a system they went to themselves: a key of their own charts, which nothing but their
    // own arrival writes -- not their corporation's charts, nor the public ones.
    r.charted = s.known.count(ChartKey(systemId)) != 0;
    if (!r.charted)
        return r;
    const auto seen = s.sightings.find(systemId);
    const bool looked = seen != s.sightings.end() && seen->second.Observed();
    // What they saw, unless it is from before the region drifted (#431 M3): its geography
    // holds, and what stood there is for looking at again.
    if (looked && !seen->second.outdated)
    {
        r.observed = true;
        r.at = seen->second.at;
        for (const WorldLoader::SeenObject& o : seen->second.seen)
        {
            if (o.kind == "station")
            {
                r.stations++;
                continue;
            }
            // A hidden base they found is a find, below, not one more structure: a faction
            // that never scanned counts none.
            const Archetype* a = Archetypes::Find(o.kind);
            if (a == nullptr || !a->Has(Component::Signature))
                r.structures++;
            FactionId by;
            if (!o.building && Outposts::IsOutpost(o.kind) && Outposts::FactionOf(o.owner, by))
                r.outposts.push_back(by);
        }
        r.structures += seen->second.more;
    }
    // Their finds that still stand there: what their scans resolved, not their own.
    for (const auto& e : st->entities)
    {
        if (SignatureOf(*e) <= 0.0f || (!e->GetOwner().empty() && e->GetOwner() == s.accountName))
            continue;
        const std::string key = SignatureKey(*st, *e);
        if (s.known.count(key) != 0)
            r.finds.push_back(key);
    }
    return r;
}

Simulation::ChartOffer Simulation::ChartOfferFor(const ClientSession& s, FactionId f,
                                                 const std::string& systemId) const
{
    ChartOffer o;
    o.system = systemId;
    const SystemState* st = SystemById(systemId);
    const ChartReport  r = ReportOf(s, systemId);
    if (st == nullptr || !r.charted)
    {
        o.why = "it is not on your own charts";
        return o;
    }
    o.seenAt = r.at;
    // Where it holds or has ships, a faction sees for itself (as its saved intel has it).
    if ((st->agg.claimed && st->agg.controller == f) || st->agg.presence[f] >= 1.0f)
    {
        o.why = FactionName(f) + " sees " + SystemName(systemId) + " for itself";
        return o;
    }
    const WorldRules::Charts& rules = WorldRules::ChartRules();
    const FactionMind&        mind = minds_[f];
    const auto                k = mind.intel.find(systemId);
    const Intel*              known = k == mind.intel.end() ? nullptr : &k->second;
    double                    value = 0.0;
    // That it is there and how to get there: news to a faction that never saw it, unless
    // everybody's charts have it by its place (a centre).
    if (known == nullptr)
    {
        o.news = true;
        if (!PublicByPlace(systemId))
            value += rules.system;
    }
    // What stood there, if they looked after it did: whatever differs from what it believes,
    // and the older the look the less it is worth.
    if (r.observed && (known == nullptr || r.at > known->seenAt))
    {
        const int stations = std::abs(r.stations - (known != nullptr ? known->stations : 0));
        const int structures = std::abs(r.structures - (known != nullptr ? known->structures : 0));
        o.changed = stations + structures;
        const double age = std::max(0.0, time_ - r.at) / 3600.0;
        value += (stations * rules.station + structures * rules.structure) *
                 std::pow(0.5, age / rules.halfLifeHours);
    }
    // What their scans found there that it was never told of.
    for (const std::string& key : r.finds)
        if (mind.finds.count(key) == 0)
        {
            o.finds++;
            value += rules.find;
        }
    // Rare is worth more: every other faction with a picture of it makes it less so.
    int others = 0;
    for (FactionId g : Factions::All())
        if (g != f && minds_[g].intel.count(systemId) != 0)
            others++;
    value /= 1.0 + rules.rarity * others;
    o.price = (int)std::min(1.0e9, std::floor(value));
    if (o.price <= 0)
    {
        o.price = 0;
        o.why = FactionName(f) + " knows all you have of " + SystemName(systemId);
    }
    return o;
}

std::vector<Simulation::ChartOffer> Simulation::ChartOffers(const ClientSession& s,
                                                            FactionId            f) const
{
    std::set<std::string> systems;
    for (const std::string& key : s.known)
    {
        const std::string id = ChartedSystemOf(key);
        if (!id.empty())
            systems.insert(id);
    }
    std::vector<ChartOffer> out;
    for (const std::string& id : systems)
    {
        ChartOffer o = ChartOfferFor(s, f, id);
        if (o.price > 0)
            out.push_back(std::move(o));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ChartOffer& a, const ChartOffer& b) { return a.price > b.price; });
    return out;
}

bool Simulation::SellCharts(ClientSession& s, const std::string& which)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "No sale: " + why);
        return false;
    };
    FactionId f;
    if (!ChartBuyer(s, f))
        return refuse("dock at a faction's station to sell it your charts");
    std::vector<ChartOffer> offers;
    if (which == "*")
    {
        offers = ChartOffers(s, f);
        if (offers.empty())
            return refuse(FactionName(f) + " knows everything you have charted");
    }
    else
    {
        const std::string id = SystemNamed(which);
        if (id.empty())
            return refuse("no system is called " + which);
        ChartOffer o = ChartOfferFor(s, f, id);
        if (o.price <= 0)
            return refuse(o.why);
        offers.push_back(std::move(o));
    }

    FactionMind& mind = minds_[f];
    int          sold = 0;
    double       paid = 0.0;
    std::string  last, shortOf;
    for (const ChartOffer& o : offers)
    {
        if ((double)o.price > mind.treasury)
        {
            shortOf = SystemName(o.system);
            continue;  // a cheaper one may still be bought
        }
        const SystemState& st = *SystemById(o.system);
        const ChartReport  r = ReportOf(s, o.system);
        const bool         had = mind.intel.count(o.system) != 0;
        Intel&             i = mind.intel[o.system];
        if (!had)
        {
            // Geography, known for good once known (K6): what the pilot's charts show.
            i.belts = st.profile.belts;
            i.planets = st.profile.planets;
            i.gates = st.profile.gates;
        }
        // Who holds it and how safe it is travel live on everybody's charts.
        i.traffic = st.agg.prosperity;
        i.security = st.agg.security;
        i.controller = st.agg.controller;
        i.claimed = st.agg.claimed;
        if (r.observed && (!had || r.at > i.seenAt))
        {
            i.seenAt = r.at;
            i.stations = r.stations;
            i.structures = r.structures;
            i.outposts = r.outposts;
            i.defenders = st.profile.defenders;  // a station's, which does not move
        }
        for (const std::string& key : r.finds)
            mind.finds.insert(key);
        mind.treasury -= o.price;
        s.account.AddMoney(o.price);
        paid += o.price;
        sold++;
        last = SystemName(o.system) + " (" + o.What() + ")";
        // It charts what it bought (K3: a find is the pilot's until a faction reaches it or
        // they sell the charts).
        ChartBy(o.system, f);
    }
    if (sold == 0)
        return refuse(FactionName(f) + "'s treasury holds " +
                      std::to_string((long long)std::floor(mind.treasury)) +
                      " cr, not enough for the charts of " + shortOf);
    // Its old word of hidden things that are gone: a wormhole that closed is no find to keep.
    for (auto it = mind.finds.begin(); it != mind.finds.end();)
    {
        const std::string hole = it->rfind("hole:", 0) == 0 ? it->substr(5) : std::string();
        it = !hole.empty() && HoleById(hole) == nullptr ? mind.finds.erase(it) : std::next(it);
    }
    s.RecordEvent(
        Ev::Kind::Notice,
        "Sold " +
            (sold == 1 ? "the charts of " + last
                       : "charts of " + std::to_string(sold) + " systems") +
            " to " + FactionName(f) + " for " + std::to_string((long long)paid) + " cr" +
            (shortOf.empty() ? std::string() : "; its treasury could not pay for " + shortOf));
    return true;
}
