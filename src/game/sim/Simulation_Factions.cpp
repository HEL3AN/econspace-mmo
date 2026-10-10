// Factions acting on the galaxy (#231).
//
// Every faction runs the same step; only its Temperament, read from data/factions.json,
// differs. A faction's strength in a system is its "presence" -- the ships it has
// committed there, of which the spawn director then makes real ones. Presence grows in the
// systems a faction holds; a holding with strength to spare may reach into ONE neighbouring
// system per period, the one where what the faction values most outweighs the risk by its
// own appetite for risk; and a system changes hands only when a hostile faction has held
// the upper hand there for a sustained time (#225). Nothing here depends on whether a
// player is present: the world goes on without them, slowly and for reasons.
#include "sim/Simulation.h"

#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/Station.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
// How many macro passes make one faction period: a minute at the 2 s pass.
constexpr int FACTION_PERIOD = 30;
// A holding keeps this share of its capacity at home; only what is above it can reach out.
constexpr float HOME_GUARD = 0.6f;
// The most one move takes out of a holding, as a share of the faction's capacity.
constexpr float MOVE_SHARE = 0.25f;
// Presence a faction has not kept up with fades by this share per period.
constexpr float FADE = 0.05f;
// Below this a presence is not worth calling one.
constexpr float SPARSE = 0.05f;

bool Hostile(FactionId a, FactionId b)
{
    const Stance s = Factions::Relation(a, b);
    return a != b && (s == Stance::War || s == Stance::Hostile);
}

// Lawless places breed their own trouble: in a system the pirates do not hold, their
// presence drifts towards this however nobody sends them, and no further.
float Unrest(const SystemAggregate& a)
{
    const float cap = Factions::TemperamentOf(FactionId::Pirates).capacity;
    return std::max(0.0f, 0.6f - a.security) * cap * 0.4f;
}
}  // namespace

void Simulation::SeedPresence(SystemAggregate& a)
{
    a.presence.fill(0.0f);
    // Half strength to begin with: a fresh world grows into its numbers rather than
    // starting at them, so nothing is decided in its first minutes (#143, #225). Nobody's
    // system has nobody's garrison.
    if (a.claimed)
        a.presence[(int)a.controller] = Factions::TemperamentOf(a.controller).capacity * 0.5f;
    if (a.controller != FactionId::Pirates)
        a.presence[(int)FactionId::Pirates] = Unrest(a);
}

// What a system offers, seen by anyone: how much passes through, how much ore there is,
// how much lies about to be salvaged, and who defends it.
Simulation::SystemOffer Simulation::OfferOf(const SystemState& st) const
{
    SystemOffer o;
    o.traffic = std::clamp(st.agg.prosperity, 0.0f, 1.0f);
    int belts = 0, wrecks = 0;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() == EntityKind::Field)
            belts++;
        else if (e->GetKind() == EntityKind::Derelict)
            wrecks++;
        else if (e->GetKind() == EntityKind::Station && e->Has(Component::Defensive))
            o.defenders.push_back(static_cast<const Station*>(e.get())->GetFaction());
    }
    o.ore = std::min(1.0f, belts / 3.0f);
    o.salvage = std::min(1.0f, wrecks / 4.0f);
    return o;
}

void Simulation::StepFactions()
{
    // Losses in battle are losses of presence: whatever the director spawned and somebody
    // destroyed was strength the faction had committed there.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        if (a.lostPirates > 0.0f)
            a.presence[(int)FactionId::Pirates] =
                std::max(0.0f, a.presence[(int)FactionId::Pirates] - a.lostPirates);
        if (a.lostPolice > 0.0f)
        {
            const FactionId law = a.policeFaction;
            a.presence[(int)law] = std::max(0.0f, a.presence[(int)law] - a.lostPolice);
        }
        a.lostPirates = a.lostPolice = 0.0f;
    }

    if (++factionPasses_ % FACTION_PERIOD != 0)
        return;

    // 1. Holdings recover towards capacity; everything else fades towards what the place
    //    breeds by itself, unless its faction keeps sending more.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        for (int f = 0; f < FACTION_COUNT; f++)
        {
            const Temperament& t = Factions::TemperamentOf((FactionId)f);
            float&             p = a.presence[f];
            if ((FactionId)f == a.controller && a.claimed)
                p += t.growth * (t.capacity - p);
            else
            {
                const float floor = (FactionId)f == FactionId::Pirates ? Unrest(a) : 0.0f;
                p = p > floor ? p - (p - floor) * FADE : p + (floor - p) * t.growth;
            }
            if (p < SPARSE)
                p = 0.0f;
        }
    }

    // 2. Each faction makes at most one move: its best target among the neighbours of what
    //    it holds, if the best is worth the risk at all.
    for (int fi = 0; fi < FACTION_COUNT; fi++)
    {
        const FactionId    f = (FactionId)fi;
        const Temperament& t = Factions::TemperamentOf(f);
        if (t.appetite <= 0.0f)
            continue;  // holds what it has and reaches for nothing

        float       bestScore = 0.0f;
        std::string from, to, why;
        for (auto& kv : systems_)
        {
            const SystemAggregate& home = kv.second.agg;
            if (home.controller != f)
                continue;
            const float surplus = home.presence[fi] - t.capacity * HOME_GUARD;
            if (surplus < 0.5f)
                continue;
            for (const std::string& nid : Neighbors(kv.first))
            {
                auto n = systems_.find(nid);
                if (n == systems_.end())
                    continue;
                const SystemAggregate& a = n->second.agg;
                // Only into a system it may take: nobody's, or an enemy's. Never a friend's
                // or a neutral power's -- that would be a war nobody declared.
                if ((a.claimed && a.controller == f) || (a.claimed && !Hostile(f, a.controller)))
                    continue;

                const SystemOffer o = OfferOf(n->second);
                const float       unclaimed = a.claimed ? 0.0f : 1.0f;
                const float value = t.traffic * o.traffic + t.ore * o.ore + t.salvage * o.salvage +
                                    t.unclaimed * unclaimed;
                float       hostile = 0.0f;
                for (int g = 0; g < FACTION_COUNT; g++)
                    if (Hostile(f, (FactionId)g) ||
                        ((FactionId)g == a.controller && a.controller != f))
                        hostile += a.presence[g];
                int guns = 0;
                for (FactionId d : o.defenders)
                    if (d != f)
                        guns++;
                // The lawless also fear the law itself; the law does not fear a quiet system.
                const float risk = hostile / t.capacity + 0.5f * guns +
                                   (Factions::IsLawful(f) ? 0.0f : a.security);
                const float score = value - risk / t.appetite;
                if (score > bestScore)
                {
                    bestScore = score;
                    from = kv.first;
                    to = nid;
                    why.clear();
                    auto add = [&](float w, float v, const char* word)
                    {
                        if (w * v >= 0.3f)
                            why += std::string(why.empty() ? "" : ", ") + word;
                    };
                    add(t.traffic, o.traffic, "traffic");
                    add(t.ore, o.ore, "ore");
                    add(t.salvage, o.salvage, "salvage");
                    add(t.unclaimed, unclaimed, "nobody holds it");
                    if (hostile < 1.0f && guns == 0)
                        why += std::string(why.empty() ? "" : ", ") + "nobody to stop them";
                }
            }
        }
        if (to.empty())
            continue;

        SystemAggregate& home = systems_[from].agg;
        SystemAggregate& dest = systems_[to].agg;
        const float      move =
            std::min(home.presence[fi] - t.capacity * HOME_GUARD, t.capacity * MOVE_SHARE);
        const bool first = dest.presence[fi] < 1.0f && dest.presence[fi] + move >= 1.0f;
        home.presence[fi] -= move;
        dest.presence[fi] += move;
        if (first)
            PushEvent(FactionName(f) + " move into " + SystemName(to) +
                      (why.empty() ? std::string() : ": " + why));
    }
}

// 3. Who holds a system: the controller, until a hostile faction has had the upper hand
//    there for CONTEST_PASSES in a row (#225).
void Simulation::StepControl(bool settling)
{
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        // Nobody's system goes to whoever has held it longest in strength; a held one only
        // to an enemy of its holder.
        const float held = a.claimed ? a.presence[(int)a.controller] : 0.0f;
        FactionId   challenger = a.controller;
        float       strongest = 0.0f;
        for (int g = 0; g < FACTION_COUNT; g++)
            if ((!a.claimed || Hostile((FactionId)g, a.controller)) &&
                ((FactionId)g != a.controller || !a.claimed) && a.presence[g] > strongest)
            {
                strongest = a.presence[g];
                challenger = (FactionId)g;
            }
        // And a faction must really have come: half its own capacity committed, which the
        // local trouble a lawless system breeds by itself (Unrest) never reaches. A gang is
        // not a faction moving in.
        const bool upper = (challenger != a.controller || !a.claimed) &&
                           strongest > held * 1.5f + 1.0f &&
                           strongest >= Factions::TemperamentOf(challenger).capacity * 0.5f;
        a.contested = upper ? a.contested + 1 : 0;
        if (a.contested < ContestPasses())
            continue;
        a.contested = 0;
        const FactionId was = a.controller;
        const bool      wasClaimed = a.claimed;
        a.controller = challenger;
        a.claimed = true;
        if (challenger == FactionId::Pirates)
            a.baseSecurity = std::min(a.baseSecurity, 0.15f);
        else if (was == FactionId::Pirates)
            a.baseSecurity = std::max(a.baseSecurity, 0.5f);
        if (!settling)
            PushEvent(!wasClaimed ? FactionName(challenger) + " claim " + SystemName(kv.first)
                      : challenger == FactionId::Pirates
                          ? "Pirates seized " + SystemName(kv.first)
                          : SystemName(kv.first) + " taken by " + FactionName(challenger));
    }
}
