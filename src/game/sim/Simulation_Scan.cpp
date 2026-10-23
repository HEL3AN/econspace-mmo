// Signatures and the ship's scanner (#346).
//
// Exploration used to be only visiting. Now some things are hidden: anything whose archetype
// has a `signature` -- a roaming hole's two ends (#342 S9), a pirate clan's hidden base -- is
// left out of a pilot's layout, deltas and so of their overview, sensor and map, until a scan
// of theirs resolves it. A pass of the ship's scanner takes WorldRules::Scan's seconds over
// the whole system, and adds the signature's strength times the scanner's power to what the
// pilot has of it; at 1 it is found, and from then on it is theirs to see, across a restart.
// The first pilot ever to find one is news.
//
// The server decides what a pilot may see: the client is never sent what it has not found,
// so a modified client has nothing to show.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "core/Archetype.h"
#include "entities/JumpGate.h"
#include "entities/Ship.h"
#include "entities/Structure.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace
{
// What a hidden object is, as its archetype names it: what a site will become.
std::string KindName(const Entity& e)
{
    const Archetype* a = e.GetArchetype();
    std::string      name = a != nullptr ? a->name : e.GetName();
    if (e.GetKind() == EntityKind::Structure)
        if (const Archetype* b = Archetypes::Find(static_cast<const Structure&>(e).GetBuilds()))
            name = b->name;
    return name;
}
}  // namespace

float Simulation::SignatureOf(const Entity& e)
{
    const Archetype* a = e.GetArchetype();
    if (a != nullptr && a->Has(Component::Signature))
        return a->signatureStrength;
    // A site is as hidden as what it will be: a hidden base going up is not a beacon.
    if (e.GetKind() == EntityKind::Structure)
    {
        const Archetype* b = Archetypes::Find(static_cast<const Structure&>(e).GetBuilds());
        if (b != nullptr && b->Has(Component::Signature))
            return b->signatureStrength;
    }
    return 0.0f;
}

std::string Simulation::SignatureKey(const SystemState& st, const Entity& e) const
{
    if (SignatureOf(e) <= 0.0f)
        return std::string();
    if (e.GetKind() == EntityKind::Gate)
        if (const RoamingHole* h = HoleOfGate(e.GetId()))
            return "hole:" + h->id;
    const auto key = st.keys.find(e.GetId());
    // Every structure has a key (#38); one that does not is found for this run only.
    return st.id + "/" + (key != st.keys.end() ? key->second : "#" + std::to_string(e.GetId()));
}

std::string Simulation::SignatureCode(const std::string& key)
{
    // FNV-1a of the key: the same code for everyone, and on every run.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char code[8];
    std::snprintf(code, sizeof(code), "%c%c%c-%03d", 'A' + (int)(h % 26), 'A' + (int)(h / 26 % 26),
                  'A' + (int)(h / 676 % 26), (int)(h / 17576 % 1000));
    return code;
}

namespace
{
// A pilot's own, or their corporation's (#468): nothing to scan for.
bool Ours(const Corps::Registry& corps, const std::string& owner, const std::string& account)
{
    std::string corp;
    return !owner.empty() &&
           (owner == account || (Corps::CorpOwner(owner, corp) && corps.SameCorp(owner, account)));
}
}  // namespace

bool Simulation::HiddenFrom(const ClientSession& s, const SystemState& st, const Entity& e) const
{
    if (SignatureOf(e) <= 0.0f)
        return false;
    if (Ours(corps_, e.GetOwner(), s.accountName))
        return false;  // nobody hides their own
    return s.known.count(SignatureKey(st, e)) == 0;
}

double Simulation::ScanPower(const ClientSession& s) const
{
    const double probes = s.ship ? (double)s.ship->GetStats().scan : 0.0;
    return WorldRules::ScanRules().power * (1.0 + std::max(0.0, probes));
}

bool Simulation::StartScan(ClientSession& s)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot scan: " + why);
        return false;
    };
    if (!s.ship)
        return false;
    if (s.IsDocked())
        return refuse("the station's hull blinds the scanner; undock first");
    if (s.ship->IsWarping())
        return refuse("not while warping");
    if (s.scanUntil > 0.0)
        return refuse("a pass is under way");
    const double seconds = WorldRules::ScanRules().seconds;
    s.scanUntil = time_ + seconds;
    s.scanSystem = s.systemId;
    char line[96];
    std::snprintf(line, sizeof(line), "Scanning %s (%.0f s)", SystemName(s.systemId).c_str(),
                  seconds);
    s.RecordEvent(Ev::Kind::Notice, line);
    return true;
}

void Simulation::CreditFind(ClientSession& s, const std::string& key, const std::string& what,
                            const std::string& systemId)
{
    if (!foundSignatures_.insert(key).second)
        return;
    const std::string who = s.ship ? s.ship->GetPilotName() : s.accountName;
    Announce("scan", std::nullopt, systemId,
             (who.empty() ? std::string("A pilot") : who) + " found " + what + " in " +
                 SystemName(systemId));
}

std::string Simulation::Resolve(ClientSession& s, const SystemState& st, const Entity& e)
{
    const std::string key = SignatureKey(st, e);
    s.scanProgress.erase(key);
    s.known.insert(key);
    s.revealPending.push_back(e.GetId());
    std::string what, line;
    if (const RoamingHole* h = HoleOfGate(e.GetId()))
    {
        TouchHoles(s);
        const std::string to = static_cast<const JumpGate&>(e).GetDestination();
        what = "a wormhole to " + SystemName(to);
        line = SignatureCode(key) + ": " + what + " -- " + h->dressed;
    }
    else
    {
        const std::string name = KindName(e);
        what = "a " + name;
        line = SignatureCode(key) + ": " + name + " -- " + e.GetName();
    }
    CreditFind(s, key, what, st.id);
    return line;
}

void Simulation::RevealByFire(ClientSession& s, const SystemState& st, const Entity& e)
{
    // Shot at by something the pilot was never told exists (#415): the beam and the damage
    // name a place, and a client told of an effect before its cause breaks #38. So it is
    // found, by the one it fired on -- whoever else is in the system still has to scan.
    if (!HiddenFrom(s, st, e))
        return;
    s.RecordEvent(Ev::Kind::Scanned, Resolve(s, st, e) + ", revealed by its fire");
}

bool Simulation::BeamHiddenFrom(const ClientSession& s, const std::string& systemId,
                                const FireEvent& f) const
{
    if (f.shooterEntityId == 0 && f.targetEntityId == 0)
        return false;
    const SystemState* st = SystemById(systemId);
    if (st == nullptr)
        return false;
    for (const auto& e : st->entities)
        if ((e->GetId() == f.shooterEntityId || e->GetId() == f.targetEntityId) &&
            HiddenFrom(s, *st, *e))
            return true;
    return false;
}

void Simulation::StepPlayerScan(ClientSession& s)
{
    if (s.scanUntil <= 0.0)
        return;
    if (!s.ship || s.IsDocked() || s.systemId != s.scanSystem || s.ship->IsWarping())
    {
        s.scanUntil = 0.0;
        s.RecordEvent(Ev::Kind::Scanned, "Scan interrupted: the ship moved off the pass");
        return;
    }
    if (time_ < s.scanUntil)
        return;
    s.scanUntil = 0.0;
    SystemState* st = SystemOf(s);
    if (st == nullptr)
        return;
    const double power = ScanPower(s);
    int          resolved = 0, partial = 0;
    std::string  found;
    for (const auto& e : st->entities)
    {
        if (!HiddenFrom(s, *st, *e))
            continue;
        const std::string key = SignatureKey(*st, *e);
        float&            p = s.scanProgress[key];
        p = std::min(1.0f, p + (float)(SignatureOf(*e) * power));
        if (p < 0.999f)
        {
            partial++;
            continue;
        }
        found += "\n  " + Resolve(s, *st, *e);
        resolved++;
    }
    std::string line = "Scan of " + SystemName(st->id) + " complete: ";
    if (resolved == 0 && partial == 0)
        line += "no signatures";
    else
        line += std::to_string(resolved) + " resolved, " + std::to_string(partial) +
                " not yet -- scan again" + found;
    if (resolved > 0 && partial == 0)
        line = "Scan of " + SystemName(st->id) + " complete: " + std::to_string(resolved) +
               " resolved" + found;
    s.RecordEvent(Ev::Kind::Scanned, line);
}

Proto::ScanView Simulation::ScanViewOf(const ClientSession& s) const
{
    Proto::ScanView v;
    v.until = s.scanUntil;
    v.seconds = WorldRules::ScanRules().seconds;
    v.power = ScanPower(s);
    const SystemState* st = SystemById(s.systemId);
    if (st == nullptr)
        return v;
    for (const auto& e : st->entities)
    {
        if (SignatureOf(*e) <= 0.0f || Ours(corps_, e->GetOwner(), s.accountName))
            continue;
        const std::string    key = SignatureKey(*st, *e);
        Proto::SignatureView g;
        g.code = SignatureCode(key);
        if (s.known.count(key) != 0)
        {
            g.progress = 1.0f;
            g.id = e->GetId();
            if (e->GetKind() == EntityKind::Gate)
            {
                g.kind = "wormhole";
                g.dest = static_cast<const JumpGate&>(*e).GetDestination();
            }
            else
            {
                g.kind = KindName(*e);
                for (char& c : g.kind)
                    c = (char)std::tolower((unsigned char)c);
            }
        }
        else
        {
            // Nothing a pass has not touched: a signature is not shown before it is scanned.
            const auto p = s.scanProgress.find(key);
            if (p == s.scanProgress.end())
                continue;
            g.progress = p->second;
        }
        v.sigs.push_back(std::move(g));
    }
    std::sort(v.sigs.begin(), v.sigs.end(),
              [](const Proto::SignatureView& a, const Proto::SignatureView& b)
              { return a.code < b.code; });
    return v;
}
