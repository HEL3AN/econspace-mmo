// Construction (#39): laying down a site, finishing it, taking it down, and its end. One
// translation unit of Simulation (#17).
//
// Everything a player builds goes through AddStatic / RemoveStatic, so it reaches everyone
// in the system as a LayoutDelta and outlives a restart (#38) without anything here knowing
// how. What this file decides is only the rules: what it costs, where it may stand, how many
// one player may have, and when it is done.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "sim/Names.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/World.h"
#include "economy/Market.h"
#include "entities/Derelict.h"
#include "entities/Planet.h"
#include "entities/Ship.h"
#include "entities/Structure.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
float Distance(Vector2 a, Vector2 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::string Whole(float v)
{
    return std::to_string((long long)std::lround(v));
}

// What an upgrade is built onto, as a player reads it: "Outpost, Mining Outpost or Watch Post".
std::string OntoNames(const Blueprint& bp)
{
    std::vector<std::string> names;
    for (const std::string& id : bp.onto)
        if (const Blueprint* b = Blueprints::Find(id))
            if (b->byPlayers)  // what a player cannot have is no help to name
                names.push_back(b->name);
    if (names.empty())
        return "outpost";
    std::string out;
    for (size_t i = 0; i < names.size(); i++)
        out += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
    return out;
}

// An outpost of any kind: what a faction settles with, and the first link of the chain.
bool IsOutpostBlueprint(const Blueprint& bp)
{
    Outposts::Purpose p;
    return Outposts::PurposeOf(bp.archetype, p);
}

// An outpost, or anything grown from one or growing from one (#342 S6).
bool IsOutpostChain(const Structure& t)
{
    const Blueprint* bp = t.GetBlueprint();
    return Outposts::IsOutpost(t.GetBuilds()) ||
           (bp != nullptr && (!bp->onto.empty() || IsOutpostBlueprint(*bp)));
}
}  // namespace

const Structure* Simulation::UpgradeTarget(const SystemState& st, Vector2 at)
{
    const Structure* best = nullptr;
    float            nearest = 0.0f;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Structure)
            continue;
        const float d = Distance(at, e->GetPosition());
        if (d <= e->GetSize() && (best == nullptr || d < nearest))
        {
            best = static_cast<const Structure*>(e.get());
            nearest = d;
        }
    }
    return best;
}

int Simulation::StructuresOwnedBy(const std::string& account) const
{
    int n = 0;
    for (const auto& kv : systems_)
        for (const auto& e : kv.second.entities)
            if (e->GetKind() == EntityKind::Structure && e->GetOwner() == account)
                n++;
    return n;
}

std::string Simulation::PlacementProblem(const ClientSession& s, const Blueprint& bp, Vector2 at,
                                         bool fromHere) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.ship)
        return "there is no ship to build from";
    if (s.accountName.empty())
        return "only an account can own a structure";
    if (s.IsDocked())
        return "undock first: a site is laid down from space";
    if (s.ship->GetWarpPhase() != WarpPhase::None)
        return "not while warping";

    if (!bp.onto.empty())
    {
        // An upgrade (#342 S6, #433): built onto an outpost of the player's own, which becomes
        // its site. The place is the outpost's, so nothing about the spot is asked again, and
        // it adds no structure to the account's count.
        const Structure* onto = UpgradeTarget(*st, at);
        if (onto == nullptr)
            return "a " + bp.name + " is built onto your own " + OntoNames(bp) +
                   ", and there is none there";
        if (onto->GetOwner() != s.accountName)
            return onto->GetName() + " is not yours: a " + bp.name +
                   " is built only onto your own " + OntoNames(bp);
        const Blueprint* was = onto->GetBlueprint();
        if (was == nullptr || std::find(bp.onto.begin(), bp.onto.end(), was->id) == bp.onto.end())
            return "a " + bp.name + " is built onto " + OntoNames(bp) + ", and " + onto->GetName() +
                   " is not one";
        if (onto->IsBuilding())
            return onto->GetName() + " is still being built";
        const float edge = Distance(onto->GetPosition(), s.ship->GetPosition()) - onto->GetSize();
        if (fromHere && edge > bp.reach)
            return "too far from " + onto->GetName() + " (" + Whole(edge) + " of at most " +
                   Whole(bp.reach) + ")";
    }
    else
    {
        const float fromShip = Distance(at, s.ship->GetPosition());
        if (fromHere && fromShip > bp.reach)
            return "too far from the ship (" + Whole(fromShip) + " of at most " + Whole(bp.reach) +
                   ")";
        const std::string where = SpotProblem(*st, bp, at);
        if (!where.empty())
            return where;
        // One outpost to a system (#433), whoever's and whatever it has grown into: the
        // blueprint's cap counts only its own kind, and the chain would otherwise let a
        // second one go up beside the first the moment it became a market.
        if (IsOutpostBlueprint(bp))
            for (const auto& e : st->entities)
                if (e->GetKind() == EntityKind::Structure &&
                    IsOutpostChain(static_cast<const Structure&>(*e)))
                    return "this system has an outpost already: " + e->GetName();

        const int cap = Blueprints::PerAccount();
        if (cap > 0 && StructuresOwnedBy(s.accountName) >= cap)
            return "you already have " + std::to_string(cap) + " structures standing";
    }

    for (const auto& c : bp.cost)
        if (s.ship->GetCargoAmount(c.first) < c.second)
            return "the hold has " + std::to_string(s.ship->GetCargoAmount(c.first)) + " " +
                   ResourceName(c.first) + " of the " + std::to_string(c.second) + " it takes";
    return std::string();
}

std::string Simulation::SpotProblem(const SystemState& st, const Blueprint& bp, Vector2 at) const
{
    if (Distance(at, Vector2{ 0.0f, 0.0f }) > World::SYSTEM_RADIUS)
        return "outside the system";

    const Archetype* a = Archetypes::Find(bp.archetype);
    const float      own = a != nullptr ? a->defaultSize : 0.0f;
    for (const auto& e : st.entities)
    {
        const float d = Distance(at, e->GetPosition());
        switch (e->GetKind())
        {
            // Not inside a body, nor where one will be: a planet comes round its orbit and a
            // satellite round its planet, and a structure standing in the path would be
            // swept through on the next pass (#210). A rule, not a check of where it is now.
            case EntityKind::Star:
                if (d < e->GetSize() + bp.bodyClearance)
                    return "too close to the star";
                break;
            case EntityKind::Planet:
            {
                const float path = static_cast<const Planet&>(*e).GetOrbitRadius();
                const float fromCentre = Distance(at, Vector2{ 0.0f, 0.0f });
                if (std::fabs(fromCentre - path) < e->GetSize() + bp.bodyClearance)
                    return "in the path of " + e->GetName();
                break;
            }
            // Room around what others use: a site on a dock or across a gate is in the way
            // of everyone who flies there (#41 names it griefing; this is its first rule).
            case EntityKind::Station:
            case EntityKind::Gate:
            case EntityKind::Derelict:
            case EntityKind::Structure:
                if (!e->GetOrbit() && d < e->GetSize() + own + bp.clearance)
                    return "too close to " + e->GetName() + " (keep " + Whole(bp.clearance) +
                           " clear)";
                break;
            // A belt or a cloud is a place, and marking one is what a buoy is for.
            case EntityKind::Field:
            case EntityKind::Nebula:
            case EntityKind::Npc:
            case EntityKind::PlayerShip:
            case EntityKind::Unknown: break;
        }

        // A satellite's path: the ring its planet's path is widened into by its own orbit.
        if (const std::optional<Orbit>& o = e->GetOrbit())
        {
            float planetPath = 0.0f;
            int   index = 0;
            for (const auto& p : st.entities)
                if (p->GetKind() == EntityKind::Planet && index++ == o->planet)
                    planetPath = static_cast<const Planet&>(*p).GetOrbitRadius();
            const float fromCentre = Distance(at, Vector2{ 0.0f, 0.0f });
            if (std::fabs(fromCentre - planetPath) <
                o->radius + e->GetSize() + own + std::max(bp.clearance, bp.bodyClearance))
                return "in the path of " + e->GetName();
        }
    }

    // An Anchor stands at an unstable mouth, and one is all a mouth needs (#342 S4).
    if (bp.mouth > 0.0f)
    {
        const Entity* gate = nullptr;
        if (MouthNear(st, at, bp.mouth, &gate) == nullptr)
            return "only at an unstable wormhole mouth (within " + Whole(bp.mouth) + " of it)";
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Structure &&
                static_cast<const Structure&>(*e).GetBuilds() == bp.archetype &&
                Distance(e->GetPosition(), gate->GetPosition()) - gate->GetSize() <= bp.mouth)
                return "this mouth has an Anchor already";
    }

    if (bp.perSystem > 0)
    {
        int here = 0;
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Structure &&
                static_cast<const Structure&>(*e).GetBuilds() == bp.archetype)
                here++;
        if (here >= bp.perSystem)
            return "this system already has " + std::to_string(here) + " of them";
    }
    return std::string();
}

int Simulation::Deploy(ClientSession& s, const std::string& blueprint, Vector2 at,
                       const std::string& name)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot build: " + why);
        return 0;
    };
    const Blueprint* bp = Blueprints::Find(blueprint);
    if (bp == nullptr)
        return refuse("there is no blueprint '" + blueprint + "'");
    if (!bp->byPlayers)
        return refuse("only a faction builds " + bp->name + "s");
    std::string       why;
    const std::string called = name.empty() ? bp->name : name;
    if (!name.empty() && !Names::ValidSystemName(name, why))
        return refuse(why);
    why = PlacementProblem(s, *bp, at);
    if (!why.empty())
        return refuse(why);
    if (!bp->onto.empty())
        return Upgrade(s, *bp, at, name);

    // Committed in full when the site goes down. Dismantling it gives back what the rules
    // say (Blueprints::Refund); having it shot down gives back nothing but a wreck.
    for (const auto& c : bp->cost)
        s.ship->RemoveCargo(c.first, c.second);

    auto site = std::make_unique<Structure>(at, 0.0f, called, bp->archetype);
    site->SetBlueprint(bp->id);
    site->StartBuilding(time_, time_ + bp->buildSeconds);
    if (bp->lifetime > 0.0f)
        site->SetExpiresAt(time_ + bp->buildSeconds + bp->lifetime);
    const int id = AddStatic(s.systemId, std::move(site), s.accountName);
    if (id == 0)
        return refuse("the system would not take it");
    s.RecordEvent(Ev::Kind::Notice,
                  "Building " + called + ": ready in " + Whole(bp->buildSeconds) + " s");
    return id;
}

int Simulation::Upgrade(ClientSession& s, const Blueprint& bp, Vector2 at, const std::string& name)
{
    SystemState* st = SystemById(s.systemId);
    Structure*   t = st != nullptr ? const_cast<Structure*>(UpgradeTarget(*st, at)) : nullptr;
    if (t == nullptr)
        return 0;
    for (const auto& c : bp.cost)
        s.ship->RemoveCargo(c.first, c.second);
    // As a faction's (StartUpgrade): the outpost is the site of what it grows into, as far along
    // as a new site and as hurt as it was, by the share of its hull. It keeps its owner, and
    // its name unless the player gives it another.
    const float       hurt = t->MaxHull(time_) > 0.0f ? t->GetDamage() / t->MaxHull(time_) : 0.0f;
    const std::string was = t->GetName();
    t->Retask(bp.archetype, bp.id, name.empty() ? was : name);
    t->StartBuilding(time_, time_ + bp.buildSeconds);
    t->SetDamage(hurt * t->MaxHull(time_));
    MarkStaticChanged(*st, t->GetId());
    st->profile = ProfileOf(*st);
    ScheduleStructure(st->id, *t);
    s.RecordEvent(Ev::Kind::Notice, "Building " + was + " into a " + bp.name + ": ready in " +
                                        Whole(bp.buildSeconds) + " s");
    return t->GetId();
}

namespace
{
Structure* StructureIn(const SystemState& st, int id)
{
    for (const auto& e : st.entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<Structure*>(e.get());
    return nullptr;
}

std::string Amounts(const std::vector<std::pair<ResourceType, int>>& v)
{
    std::string out;
    for (const auto& c : v)
        out += (out.empty() ? "" : ", ") + std::to_string(c.second) + " " + ResourceName(c.first);
    return out.empty() ? std::string("nothing") : out;
}
}  // namespace

std::string Simulation::DismantleProblem(const ClientSession& s, int id) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.ship)
        return "there is no ship to do it from";
    const Structure* t = StructureIn(*st, id);
    if (t == nullptr)
        return "there is no structure #" + std::to_string(id) + " in this system";
    // Only its owner, and an account has to be one: a faction's outpost is not a player's to
    // take apart, and the world's own is nobody's.
    if (s.accountName.empty() || t->GetOwner() != s.accountName)
        return t->GetName() + " is not yours";
    if (s.IsDocked())
        return "undock first: a structure is taken apart from space";
    if (s.ship->GetWarpPhase() != WarpPhase::None)
        return "not while warping";
    const Blueprint* bp = t->GetBlueprint();
    if (bp == nullptr)
        return "nothing says how " + t->GetName() + " comes apart";
    const float d = Distance(t->GetPosition(), s.ship->GetPosition()) - t->GetSize();
    if (d > bp->reach)
        return "too far from " + t->GetName() + " (" + Whole(d) + " of at most " +
               Whole(bp->reach) + ")";

    // What comes back has to fit: a refusal can be acted on, material lost to a full hold
    // cannot.
    int units = 0;
    for (const auto& c : Blueprints::Refund(*bp, t->Progress(time_), t->HullFraction(time_)))
        units += c.second;
    const int room = s.ship->GetCargoCapacity() - s.ship->GetCargoUsed();
    if (units > room)
        return "the hold has room for " + std::to_string(room) + " of the " +
               std::to_string(units) + " it would return";
    return std::string();
}

bool Simulation::Dismantle(ClientSession& s, int id)
{
    const std::string why = DismantleProblem(s, id);
    if (!why.empty())
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot dismantle: " + why);
        return false;
    }
    const Structure* t = StructureIn(*SystemOf(s), id);
    const auto       back =
        Blueprints::Refund(*t->GetBlueprint(), t->Progress(time_), t->HullFraction(time_));
    const std::string name = t->GetName();
    if (!RemoveStatic(s.systemId, id))
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot dismantle: the system would not let it go");
        return false;
    }
    for (const auto& c : back)
        s.ship->AddCargo(c.first, c.second);
    s.RecordEvent(Ev::Kind::Notice, "Dismantled " + name + ": " + Amounts(back) + " to the hold");
    return true;
}

bool Simulation::LawOver(const SystemState& st, const Structure& t, const std::string& attacker,
                         FactionId& law) const
{
    if (!attacker.empty() && t.GetOwner() == attacker)
        return false;  // your own is yours to wreck
    FactionId owner;
    if (Outposts::FactionOf(t.GetOwner(), owner))
    {
        // A faction's own: an attack on it is an attack on the faction, wherever it stands.
        law = owner;
        return Factions::IsLawful(owner);
    }
    // A player's, or the world's: as safe as the system's law makes it.
    law = st.agg.controller;
    return st.agg.claimed && Factions::IsLawful(law);
}

bool Simulation::DamageStructure(SystemState& st, int id, float damage, ClientSession* by)
{
    return HitStructure(st, id, damage, by, Loss::Attack, Factions::Unaffiliated());
}

bool Simulation::HitStructure(SystemState& st, int id, float damage, ClientSession* by, Loss cause,
                              FactionId attacker)
{
    Structure* t = StructureIn(st, id);
    if (t == nullptr || damage <= 0.0f)
        return false;
    FactionId  law = Factions::Unaffiliated();
    const bool crime = by != nullptr && LawOver(st, *t, by->accountName, law);
    if (crime)
    {
        ChargeAttack(*by, law, false);
        CallPolice(st, *by, law);  // ...and the law comes, as it does for a ship
    }
    // An armed structure fires back at whoever shot it (StepStationDefence), for a while.
    if (by != nullptr && by->accountName != t->GetOwner())
        st.provokedBy[id][by->id] = time_ + PROVOKED_SECONDS;

    t->TakeDamage(damage);
    // Its owner hears of it, on or off (#431 C2): a player's, shot by somebody else or
    // besieged. Wear is not an attack, and a faction's outpost has its history instead.
    FactionId   ownerFaction;
    std::string attackedBy;
    if (cause != Loss::Decay && !t->GetOwner().empty() &&
        !Outposts::FactionOf(t->GetOwner(), ownerFaction) &&
        (by == nullptr || by->accountName != t->GetOwner()))
        attackedBy = by != nullptr ? (by->accountName.empty() ? "a pilot" : by->accountName)
                     : cause == Loss::Siege ? FactionName(attacker)
                                            : "unknown attackers";
    const float hull = t->HullFraction(time_);
    if (!attackedBy.empty())
        NoteAttack(t->GetOwner(), st.id + "/" + StaticKey(st.id, id),
                   t->GetName() + (t->IsBuilding() ? " (being built)" : ""), st.id, attackedBy,
                   hull, hull <= 0.0f);
    if (hull > 0.0f)
    {
        MarkStaticChanged(st, id);  // everyone sees the hull go down, and a save keeps it
        return false;
    }

    // Destroyed. Everything about it is read before it is gone.
    const std::string owner = t->GetOwner();
    const std::string name = t->GetName();
    const std::string systemId = st.id;
    const Vector2     at = t->GetPosition();
    const float       size = t->GetSize();
    const Blueprint*  bp = t->GetBlueprint();
    // An outpost being upgraded (#342 S6) is a site, but what is lost is the outpost it was.
    const bool                 upgrading = t->IsBuilding() && bp != nullptr && !bp->onto.empty();
    const bool                 wasSite = t->IsBuilding() && !upgrading;
    const std::optional<Orbit> orbit = t->GetOrbit();  // an orbital outpost's (#318)
    const std::string          builds = t->GetBuilds();
    const std::string          key = StaticKey(st.id, id);
    if (crime)
        ChargeAttack(*by, law, true);
    if (!RemoveStatic(systemId, id))
        return false;
    const bool anchor = IsAnchor(builds);

    // What is left: the wreck its blueprint names, holding the rules' share of what was
    // committed at the base price -- what the material was worth, not what one market
    // happens to pay today. Nobody's: whoever gets there first searches it.
    if (bp != nullptr && !bp->wreck.empty())
    {
        const Market prices;
        double       worth = 0.0;
        for (const auto& c : bp->cost)
            worth += c.second * prices.GetBasePrice(c.first);
        worth = std::floor(worth * Blueprints::Rules().wreckShare);
        if (worth > 0.0)
        {
            auto wreck = std::make_unique<Derelict>(at, size, "Wreck of " + name, worth);
            wreck->SetArchetype(bp->wreck);
            // ...for a while (#41): ruins nobody clears would pile up for ever. An instant on
            // the clock, so it goes on time in a system nobody is in.
            if (Blueprints::Rules().wreckSeconds > 0.0f)
                wreck->SetExpiresAt(time_ + Blueprints::Rules().wreckSeconds);
            // What was a satellite leaves its wreck on the same orbit: it is where the clock
            // puts it (#210), and a still one would sit in the band the satellites sweep.
            if (orbit)
                wreck->SetOrbit(*orbit);
            AddStatic(systemId, std::move(wreck), std::string());
        }
    }

    // Its owner was told by NoteAttack -- but not of their own shot.
    if (by != nullptr && !owner.empty() && by->accountName == owner)
        by->RecordEvent(Ev::Kind::Notice, (wasSite ? "The site of " : "") + name + " in " +
                                              SystemName(systemId) + " was destroyed");
    if (by != nullptr)
        by->RecordEvent(Ev::Kind::Notice, "Destroyed " + name);
    // An Anchor (#342 S4), anyone's: the mouth it held is unstable again.
    if (anchor)
    {
        AnchorLost(*SystemById(systemId), owner, name, key, at, wasSite, cause, attacker);
        return true;
    }
    FactionId f;
    if (!Outposts::FactionOf(owner, f))
        return true;
    // A faction's outpost (#295): the history says who lost what, and how. A site lost is a
    // settlement that will not happen; a finished one lost shakes the claim it anchored.
    const std::string where = SystemName(systemId);
    minds_[f].lostAt[systemId] = time_;  // remembered when it thinks of building here again
    if (wasSite)
    {
        Record("lost", f, systemId,
               FactionName(f) + " lost the outpost it was building in " + where);
        for (auto p = plans_.begin(); p != plans_.end(); ++p)
            if (p->second.kind == Plan::Kind::Settle && p->second.target == systemId &&
                p->second.site == key)
            {
                plans_.erase(p);  // said here; its due entry finds nothing
                break;
            }
        return true;
    }
    // An upgrade under way is over with it; nothing is left to grow.
    for (auto p = plans_.begin(); upgrading && p != plans_.end(); ++p)
        if (p->second.kind == Plan::Kind::Upgrade && p->second.target == systemId &&
            p->second.site == key)
        {
            plans_.erase(p);  // its due entry finds nothing
            break;
        }
    const std::string noun = upgrading ? std::string("outpost") : Outposts::Noun(builds);
    switch (cause)
    {
        case Loss::Siege:
            Announce("siege", attacker, systemId,
                     FactionName(attacker) + " destroyed the " + FactionName(f) + " " + noun +
                         " in " + where);
            break;
        case Loss::Decay:
            Announce("upkeep", f, systemId,
                     FactionName(f) + " lost its " + noun + " in " + where +
                         ": unsupplied, it fell into ruin");
            break;
        case Loss::Attack:
            Announce("lost", f, systemId, FactionName(f) + " lost its " + noun + " in " + where);
            break;
    }
    minds_[f].unsupplied.erase(systemId + "/" + key);
    ShakeClaim(*SystemById(systemId), f);
    return true;
}

void Simulation::ScheduleStructure(const std::string& systemId, const Entity& e)
{
    if (e.GetKind() == EntityKind::Derelict)
    {
        const double end = static_cast<const Derelict&>(e).GetExpiresAt();
        if (end > 0.0)
            structureDue_.insert({ end, e.GetId(), systemId });
        return;
    }
    if (e.GetKind() != EntityKind::Structure)
        return;
    const Structure& t = static_cast<const Structure&>(e);
    if (t.IsBuilding())
        structureDue_.insert({ t.GetCompletesAt(), t.GetId(), systemId });
    if (t.GetExpiresAt() > 0.0)
        structureDue_.insert({ t.GetExpiresAt(), t.GetId(), systemId });
}

void Simulation::StepStructures()
{
    auto tell = [&](const std::string& owner, Ev::Kind kind, const std::string& text)
    {
        for (auto& kv : sessions_)
            if (kv.second.accountName == owner)
                kv.second.RecordEvent(kind, text);
    };

    // Only what is due (#295). The instants were fixed when the site went down; this only
    // notices them pass. A server that was down at the moment finishes it on its first tick
    // back, because everything overdue is due.
    while (!structureDue_.empty() && std::get<0>(*structureDue_.begin()) <= time_)
    {
        const int         id = std::get<1>(*structureDue_.begin());
        const std::string systemId = std::get<2>(*structureDue_.begin());
        structureDue_.erase(structureDue_.begin());

        SystemState* st = SystemById(systemId);
        if (st == nullptr)
            continue;
        Structure* t = nullptr;
        Derelict*  wreck = nullptr;
        for (const auto& e : st->entities)
            if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
                t = static_cast<Structure*>(e.get());
            else if (e->GetId() == id && e->GetKind() == EntityKind::Derelict)
                wreck = static_cast<Derelict*>(e.get());
        // A wreck's time is up (#41): it is gone, searched or not, whether anyone is here to
        // see it or not -- this queue runs in every system on the same clock.
        if (wreck != nullptr && wreck->GetExpiresAt() > 0.0 && time_ >= wreck->GetExpiresAt())
            RemoveStatic(systemId, id);
        if (t == nullptr)
            continue;  // taken away before its time came

        // What happens is read off the structure, not the entry: its time line is the truth,
        // and an entry is only a reminder to look.
        if (t->IsBuilding() && time_ >= t->GetCompletesAt())
        {
            t->Complete();
            MarkStaticChanged(*st, id);
            st->profile = ProfileOf(*st);  // a finished outpost is a claim (#295)
            tell(t->GetOwner(), Ev::Kind::Built,
                 t->GetName() + " is built in " + SystemName(systemId));
            // An outpost grown into a station is one from now on (#342 S6).
            const Archetype* became = Archetypes::Find(t->GetBuilds());
            if (became != nullptr && became->kind == EntityKind::Station)
            {
                FinishStation(*st, id);
                continue;
            }
            // An Anchor finished at an unstable mouth makes it permanent gates (#342 S4).
            const Blueprint* anchor = Blueprints::Find(ANCHOR_BLUEPRINT);
            if (anchor != nullptr && IsAnchor(t->GetBuilds()))
                if (const RegionOpening* o = MouthNear(*st, t->GetPosition(), anchor->mouth))
                    AnchorMouth(*LiveOpening(o->slot), t->GetOwner());
        }
        if (t->GetExpiresAt() > 0.0 && time_ >= t->GetExpiresAt())
        {
            const std::string owner = t->GetOwner();
            const std::string what = t->GetName();
            if (RemoveStatic(systemId, id))
                tell(owner, Ev::Kind::Notice,
                     what + " in " + SystemName(systemId) + " has run its course and is gone");
        }
    }
}
