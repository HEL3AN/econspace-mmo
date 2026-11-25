// The world's own events that leave an ore field behind (#617, documents/mining_and_fields.md,
// slice 2).
//
// Time is cut into epochs (`events.epochHours`). When one begins, every live region's key --
// the seed, the epoch, its slot -- says whether something happens in it, of which kind, in which
// system, when in the epoch, how big and for how long. That is decided once and kept: a birth is
// written down (world.json `fields`) until the clock reaches it, and then the field is a static
// like any other, added through AddStatic with a delta, saved with the system's changes and
// ended by the schedule. Nothing here looks at who is where, so a cold system has its events
// the same as a hot one.
//
// What a region may spend is its ore bank: `eventShare` of what the region's stations and
// fleets use, a flow that fills a bank capped at `bankHours` of it. An Impact's rock is paid from
// the bank; a Collision moves ore from a belt, and makes none. Nothing comes from nothing.
//
// A region that opens (or a world that begins) is not a clean slate: its last `preRollEpochs`
// epochs are decided with the same keys, so there are fields at every stage of life.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/EventKinds.h"
#include "sim/WorldRules.h"

#include "core/Archetype.h"
#include "core/Orbits.h"
#include "entities/AsteroidField.h"
#include "entities/Planet.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "gen/Rng.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace
{
constexpr uint64_t EVENT_KEY = 0xE7E27u;
constexpr double   HOUR = 3600.0;
constexpr double   TURN = 6.283185307179586;
// The ores a ship is made of, in the order Gen::OreModel counts them.
constexpr const char* ORES[3] = { "Iron", "Ice", "Crystal" };
// The epochs before the world began, which the pre-roll decides, keep an unsigned key.
constexpr int64_t EPOCH_BASE = 1 << 20;

// One of `weights`' indices, by weight; -1 when they are all nothing.
int Pick(Gen::Rng& rng, const std::vector<double>& weights)
{
    double sum = 0.0;
    for (double w : weights)
        sum += std::max(0.0, w);
    if (sum <= 0.0)
        return -1;
    double at = rng.Unit() * sum;
    for (size_t i = 0; i < weights.size(); i++)
    {
        at -= std::max(0.0, weights[i]);
        if (at < 0.0)
            return (int)i;
    }
    return (int)weights.size() - 1;
}

std::string Fill(std::string line, const std::string& key, const std::string& value)
{
    const std::string token = "{" + key + "}";
    for (size_t at = line.find(token); at != std::string::npos; at = line.find(token))
        line.replace(at, token.size(), value);
    return line;
}
}  // namespace

bool Simulation::IsCentreSlot(int slot) const
{
    return std::find(skeleton_.centres.begin(), skeleton_.centres.end(), slot) !=
           skeleton_.centres.end();
}

// What the region's stations and fleets use, in units an hour, with the margin: the flow the
// belts were sized for and the bank accrues from.
std::array<double, 3> Simulation::RegionOreNeed(int slot) const
{
    std::array<double, 3> out = { 0.0, 0.0, 0.0 };
    if (fieldModelFrom_ != Archetypes::Generation())
    {
        fieldModel_ = std::make_shared<const Gen::OreModel>(OreModelFromData());
        fieldModelFrom_ = Archetypes::Generation();
    }
    if (!fieldModel_ || !fieldModel_->Active())
        return out;
    std::vector<nlohmann::json> docs;
    for (const std::string& id : RegionSystems(slot))
    {
        const auto d = regionDocs_.find(id);
        if (d == regionDocs_.end())
            continue;
        nlohmann::json doc = nlohmann::json::parse(d->second, nullptr, false);
        if (doc.is_object())
            docs.push_back(std::move(doc));
    }
    std::vector<const nlohmann::json*> ptrs;
    for (const nlohmann::json& d : docs)
        ptrs.push_back(&d);
    const std::array<long long, 3> need = Gen::OreNeed(ptrs, *fieldModel_);
    for (int k = 0; k < 3; k++)
        out[(size_t)k] =
            (double)need[(size_t)k] / 1000.0 * ((double)fieldModel_->marginPermille / 1000.0);
    return out;
}

// What the bank of region `slot` gains an hour, by ore: the share of the need events carry
// there, and in a frontier -- where the players are the demand -- a flow of its own.
std::array<double, 3> Simulation::FieldFlow(int slot) const
{
    const WorldRules::Events& r = WorldRules::EventRules();
    const bool                centre = IsCentreSlot(slot);
    std::array<double, 3>     need = RegionOreNeed(slot);
    std::array<double, 3>     flow = { 0.0, 0.0, 0.0 };
    for (int k = 0; k < 3; k++)
    {
        flow[(size_t)k] = (centre ? r.shareCentre : r.shareFrontier) * need[(size_t)k];
        if (!centre)
        {
            const auto f = r.frontierFlow.find(ORES[k]);
            if (f != r.frontierFlow.end())
                flow[(size_t)k] += f->second;
        }
    }
    return flow;
}

double Simulation::FieldBank(int slot, int ore) const
{
    const auto it = fieldRegions_.find(slot);
    return it == fieldRegions_.end() || ore < 0 || ore > 2 ? 0.0 : it->second.bank[(size_t)ore];
}

int Simulation::FieldsAlive(int slot) const
{
    int n = 0;
    for (const std::string& id : RegionSystems(slot))
    {
        const SystemState* st = SystemById(id);
        if (st == nullptr)
            continue;
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Field &&
                static_cast<const AsteroidField&>(*e).HasLife())
                n++;
    }
    for (const FieldBirth& b : fieldBirths_)
        n += b.slot == slot ? 1 : 0;
    return n;
}

void Simulation::StepFields()
{
    const WorldRules::Events& r = WorldRules::EventRules();
    if (!(r.chance > 0.0) || FieldEvents::Kinds().empty() || skeleton_.slots.empty())
        return;  // a world of files, or no events in the data: nothing happens
    const double length = std::max(0.01, r.epochHours) * HOUR;
    const int    now = (int)std::floor(time_ / length);
    // A world from before the events has the regions it has as they are: only regions that
    // open from now on are given a past.
    if (fieldsLegacy_)
    {
        for (int slot : openSlots_)
            fieldRegions_.emplace(slot, FieldRegion{ now - 1, { 0.0, 0.0, 0.0 } });
        fieldsLegacy_ = false;
    }
    for (int slot : openSlots_)
    {
        const RegionOpening* o = OpeningOf(slot);
        if (o != nullptr && (o->state == "archived" || o->state == "returned"))
            continue;
        auto it = fieldRegions_.find(slot);
        if (it == fieldRegions_.end())
        {
            // Nothing happens in a region whose systems are not built yet; it is begun when
            // they are, with its past.
            const std::vector<std::string> ids = RegionSystems(slot);
            const SystemState*             first = ids.empty() ? nullptr : SystemById(ids.front());
            if (first == nullptr || first->entities.empty())
                continue;
            it = fieldRegions_.emplace(slot, FieldRegion{ now - r.preRollEpochs - 1, {} }).first;
        }
        FieldRegion& fr = it->second;
        // A long absence is not replayed beyond the past a new region gets.
        if (now - fr.epoch > r.preRollEpochs + 1)
            fr.epoch = now - r.preRollEpochs - 1;
        if (fr.epoch >= now)
            continue;
        const std::array<double, 3> flow = FieldFlow(slot);  // once for all the epochs due
        while (fr.epoch < now)
        {
            ++fr.epoch;
            for (int k = 0; k < 3; k++)
                fr.bank[(size_t)k] = std::min(fr.bank[(size_t)k] + flow[(size_t)k] * r.epochHours,
                                              flow[(size_t)k] * r.bankHours);
            DecideField(slot, fr, fr.epoch);
        }
    }
    BirthFields();
}

void Simulation::DecideField(int slot, FieldRegion& fr, int epoch)
{
    const WorldRules::Events& r = WorldRules::EventRules();
    const double              length = std::max(0.01, r.epochHours) * HOUR;
    Gen::Rng                  rng(
        Gen::Key(regionSeed_, EVENT_KEY, (uint64_t)((int64_t)epoch + EPOCH_BASE), (uint64_t)slot));
    if (rng.Unit() >= r.chance)
        return;
    const double t0 = ((double)epoch + rng.Unit()) * length;
    if (FieldsAlive(slot) >= r.alive)
        return;

    // Where each kind can happen: the region's built systems that have what it needs, weighted
    // towards those with a market or one a gate away, which the economy's own crews can reach.
    std::vector<std::string> ids = RegionSystems(slot);
    std::sort(ids.begin(), ids.end());
    struct Site
    {
        std::string id;
        double      weight = 1.0;
        bool        planet = false;
        double      belt = 0.0;  // the most ore a belt of it that anyone can see holds
    };
    std::vector<Site> sites;
    for (const std::string& id : ids)
    {
        const SystemState* st = SystemById(id);
        if (st == nullptr || st->entities.empty())
            continue;
        Site s;
        s.id = id;
        for (const auto& e : st->entities)
        {
            if (e->GetKind() == EntityKind::Planet)
                s.planet = true;
            if (e->GetKind() == EntityKind::Field &&
                !static_cast<const AsteroidField&>(*e).HasLife() && SignatureOf(*e) <= 0.0f)
                s.belt = std::max(s.belt, static_cast<const AsteroidField&>(*e).GetOre());
        }
        bool near = false;
        for (const std::string& n : Neighbors(id))
        {
            const SystemState* ns = SystemById(n);
            near = near || (ns != nullptr && ns->profile.markets > 0);
        }
        s.weight = 1.0 + (st->profile.markets > 0 ? r.marketWeight : (near ? r.nearWeight : 0.0));
        sites.push_back(std::move(s));
    }

    // A kind, among those that have somewhere to happen and something to spend.
    const std::vector<FieldEvents::Kind>& kinds = FieldEvents::Kinds();
    std::vector<double>                   kindWeights;
    for (const FieldEvents::Kind& k : kinds)
    {
        bool ok = false;
        for (const Site& s : sites)
            ok = ok || (k.needs == "planet" ? s.planet : s.belt * k.moves >= r.minOre);
        if (ok && k.needs == "planet")
        {
            ok = false;  // and an ore of its own that the bank can pay for
            for (int i = 0; i < 3; i++)
            {
                const auto w = k.ores.find(ORES[i]);
                ok = ok || (w != k.ores.end() && w->second > 0.0 && fr.bank[(size_t)i] >= r.minOre);
            }
        }
        kindWeights.push_back(ok ? k.weight : 0.0);
    }
    const int ki = Pick(rng, kindWeights);
    if (ki < 0)
        return;
    const FieldEvents::Kind& kind = kinds[(size_t)ki];

    std::vector<double> siteWeights;
    for (const Site& s : sites)
        siteWeights.push_back(
            (kind.needs == "planet" ? s.planet : s.belt * kind.moves >= r.minOre) ? s.weight : 0.0);
    const int si = Pick(rng, siteWeights);
    if (si < 0)
        return;

    FieldBirth b;
    b.slot = slot;
    b.kind = kind.id;
    b.system = sites[(size_t)si].id;
    b.t0 = t0;
    b.hours = rng.Between(kind.hoursLo, kind.hoursHi);
    b.size = (float)rng.Between(kind.sizeLo, kind.sizeHi);
    b.seed = rng.Next();
    if (kind.needs == "planet")
    {
        // What it is made of, among the ores the bank can pay for; all of a share of that bank.
        std::vector<double> oreWeights;
        for (int i = 0; i < 3; i++)
        {
            const auto w = kind.ores.find(ORES[i]);
            oreWeights.push_back(w != kind.ores.end() && fr.bank[(size_t)i] >= r.minOre ? w->second
                                                                                        : 0.0);
        }
        const int oi = Pick(rng, oreWeights);
        if (oi < 0)
            return;
        const double share = rng.Between(kind.shareLo, kind.shareHi);
        b.ore = ORES[oi];
        b.amount = (int)std::floor(share * fr.bank[(size_t)oi]);
        if ((double)b.amount < r.minOre)
            return;
        fr.bank[(size_t)oi] -= (double)b.amount;
    }
    fieldBirths_.push_back(std::move(b));
}

void Simulation::BirthFields()
{
    if (fieldBirths_.empty())
        return;
    std::vector<FieldBirth> later;
    std::vector<FieldBirth> due;
    for (FieldBirth& b : fieldBirths_)
        (b.t0 <= time_ ? due : later).push_back(std::move(b));
    fieldBirths_ = std::move(later);
    // In the order they were decided, which is the order of their epochs.
    for (const FieldBirth& b : due)
        BirthField(b);
}

bool Simulation::BirthField(const FieldBirth& b)
{
    const WorldRules::Events& r = WorldRules::EventRules();
    const FieldEvents::Kind*  kind = FieldEvents::Find(b.kind);
    SystemState*              st = SystemById(b.system);
    if (kind == nullptr || st == nullptr || st->entities.empty())
        return false;  // the data changed under a saved birth, or the region has gone
    const double ends = b.t0 + b.hours * HOUR;
    if (ends <= time_)
        return false;  // over before anybody could have looked

    Gen::Rng             rng(b.seed);
    std::string          oreId = b.ore;
    int                  amount = b.amount;
    Vector2              at = { 0.0f, 0.0f };
    std::string          where;
    std::optional<Orbit> orbit;
    AsteroidField*       belt = nullptr;
    if (kind->needs == "planet")
    {
        std::vector<const Planet*> planets;
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Planet)
                planets.push_back(static_cast<const Planet*>(e.get()));
        if (planets.empty())
            return false;
        const int     pi = rng.Range(0, (int)planets.size() - 1);
        const Planet& p = *planets[(size_t)pi];
        Orbit         o;
        o.planet = pi;
        o.radius = std::max((float)(p.GetSize() * kind->orbitRadius), p.GetSize() + 2.0f * b.size);
        o.speed = (float)(kind->orbitSpeed * o.radius);
        o.phase = (float)(rng.Unit() * TURN);
        orbit = o;
        at = Orbits::SatellitePosition(o, p.GetPosition(), time_);
        where = p.GetName();
    }
    else
    {
        std::vector<AsteroidField*> belts;
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Field)
            {
                AsteroidField* f = static_cast<AsteroidField*>(e.get());
                if (!f->HasLife() && SignatureOf(*f) <= 0.0f &&
                    f->GetOre() * kind->moves >= r.minOre)
                    belts.push_back(f);
            }
        if (belts.empty())
            return false;
        belt = belts[(size_t)rng.Range(0, (int)belts.size() - 1)];
        // It parts with a share of what it holds; the fragments are what it parted with.
        amount = (int)std::floor(belt->GetOre() * kind->moves);
        oreId = Goods::Id(belt->GetResource());
        const double angle = rng.Unit() * TURN;
        const double away = (double)belt->GetSize() * 1.6 + (double)b.size;
        at = { belt->GetPosition().x + (float)(std::cos(angle) * away),
               belt->GetPosition().y + (float)(std::sin(angle) * away) };
    }
    ResourceType ore;
    if (amount <= 0 || !Goods::Find(oreId, ore))
        return false;
    if (belt != nullptr)
        belt->Take((double)amount);

    const std::string archetype =
        (double)amount >= r.publicOre ? kind->publicArchetype : kind->signatureArchetype;
    auto field = std::make_unique<AsteroidField>(at, b.size, kind->field, ore, amount);
    field->SetArchetype(archetype);
    FieldCurve curve = WorldRules::FieldRules().curve;
    field->SetLife(b.t0, b.hours * HOUR, curve);
    field->Age(time_);
    if (orbit)
        field->SetOrbit(*orbit);
    const int id = AddStatic(b.system, std::move(field), std::string());
    if (id == 0)
        return false;
    if (orbit)
        Orbits::Place(st->entities, time_);

    // The news says what happened and where, never how much; a pilot who is there saw it. Only
    // what is recent is news: a field a new region was given a past with is simply there.
    if (time_ - b.t0 < std::max(0.01, r.epochHours) * HOUR)
    {
        const std::string text =
            Fill(Fill(kind->line, "planet", where), "system", SystemName(b.system));
        if (OnPublicCharts(b.system))
            Announce("field", std::nullopt, b.system, text);
        else
            Record("field", std::nullopt, b.system, text);
        for (auto& kv : sessions_)
            if (kv.second.systemId == b.system)
                kv.second.RecordEvent(Ev::Kind::Notice, text);
    }
    return true;
}

void Simulation::SaveFields(nlohmann::json& j) const
{
    using nlohmann::json;
    json regions = json::object();
    for (const auto& kv : fieldRegions_)
        regions[std::to_string(kv.first)] = {
            { "epoch", kv.second.epoch },
            { "bank", json::array({ kv.second.bank[0], kv.second.bank[1], kv.second.bank[2] }) }
        };
    json births = json::array();
    for (const FieldBirth& b : fieldBirths_)
        births.push_back({ { "slot", b.slot },
                           { "kind", b.kind },
                           { "system", b.system },
                           { "t0", b.t0 },
                           { "hours", b.hours },
                           { "size", b.size },
                           { "seed", b.seed },
                           { "ore", b.ore },
                           { "amount", b.amount } });
    // A section an older build ignores (#20); it would simply have no fields to decide from.
    j["fields"] = { { "regions", std::move(regions) },
                    { "births", std::move(births) },
                    { "events", FieldEvents::Hash() } };
}

void Simulation::LoadFields(const nlohmann::json& j)
{
    fieldRegions_.clear();
    fieldBirths_.clear();
    fieldsLegacy_ = !j.contains("fields") || !j["fields"].is_object();
    if (fieldsLegacy_)
        return;
    const nlohmann::json& f = j["fields"];
    if (f.contains("regions") && f["regions"].is_object())
        for (auto it = f["regions"].begin(); it != f["regions"].end(); ++it)
        {
            if (!it.value().is_object())
                continue;
            FieldRegion fr;
            fr.epoch = it.value().value("epoch", 0);
            const nlohmann::json& bank = it.value().value("bank", nlohmann::json::array());
            for (size_t k = 0; k < 3 && k < bank.size(); k++)
                fr.bank[k] = bank[k].is_number() ? bank[k].get<double>() : 0.0;
            fieldRegions_[std::atoi(it.key().c_str())] = fr;
        }
    if (f.contains("births") && f["births"].is_array())
        for (const nlohmann::json& bj : f["births"])
        {
            if (!bj.is_object())
                continue;
            FieldBirth b;
            b.slot = bj.value("slot", 0);
            b.kind = bj.value("kind", std::string());
            b.system = bj.value("system", std::string());
            b.t0 = bj.value("t0", 0.0);
            b.hours = bj.value("hours", 1.0);
            b.size = bj.value("size", 500.0f);
            b.seed = bj.value("seed", (uint64_t)0);
            b.ore = bj.value("ore", std::string());
            b.amount = bj.value("amount", 0);
            fieldBirths_.push_back(std::move(b));
        }
    // Events edited since this world was made change what is decided from now on, never a
    // field that is alive; the server says so.
    if (f.value("events", std::string()) != FieldEvents::Hash())
        TraceLog(LOG_WARNING,
                 "World: data/events.json is not the file this world was made with; fields "
                 "alive are kept, what is decided from now on follows the new events");
}

bool Simulation::BirthFieldNow(const std::string& kind, const std::string& systemId,
                               FieldBirth& out, std::string& why)
{
    const FieldEvents::Kind* k = FieldEvents::Find(kind);
    if (k == nullptr)
    {
        why = "there is no event called '" + kind + "'";
        return false;
    }
    if (SystemById(systemId) == nullptr)
    {
        why = "there is no system '" + systemId + "'";
        return false;
    }
    out = FieldBirth();
    out.slot = Gen::SlotOf(systemId);
    out.kind = kind;
    out.system = systemId;
    out.t0 = time_;
    out.hours = 0.5 * (k->hoursLo + k->hoursHi);
    out.size = (float)(0.5 * (k->sizeLo + k->sizeHi));
    out.seed = Gen::Key(regionSeed_, EVENT_KEY, (uint64_t)std::llround(time_), (uint64_t)out.slot);
    if (k->needs == "planet")
    {
        // The first of its ores, as much as an event of it usually is: from no bank.
        out.ore = k->ores.begin()->first;
        out.amount = (int)std::llround(WorldRules::EventRules().publicOre * 1.5);
    }
    if (!BirthField(out))
    {
        why = "the system has no " + k->needs + " for it (or the ore is too little)";
        return false;
    }
    return true;
}
