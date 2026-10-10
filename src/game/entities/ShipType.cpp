#include "entities/ShipType.h"

#include "core/Archetype.h"
#include "core/ShipDesign.h"

// Player ships are designs (#279 step 5). They used to be six floats each, typed here; now a
// hull is a design in data/ships.json and its numbers are what its parts add up to, the same
// way an NPC's are. Built from the catalogue the archetypes were loaded with, and rebuilt
// when they are loaded again -- the editor reloads, and the tests load other data.
const std::vector<ShipType>& GetShipCatalog()
{
    static std::vector<ShipType> catalog;
    static unsigned              builtFrom = 0;
    static bool                  built = false;
    if (built && builtFrom == Archetypes::Generation())
        return catalog;

    catalog.clear();
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    for (const Ships::HangarEntry& h : ships.hangar)
    {
        // The catalogue that loaded validated every design, so each one derives.
        const Ships::Design* d = ships.FindDesign(h.design);
        Ships::Stats         s;
        std::string          why;
        if (d == nullptr || !Ships::Derive(ships, *d, s, why))
            continue;
        ShipType t;
        t.name = d->name;
        t.price = h.price;
        t.stats.thrustPower = s.acceleration;
        t.stats.turnSpeed = s.turnSpeed;
        t.stats.maxSpeed = s.maxSpeed;
        t.stats.rcsAccel = s.rcsAccel;
        t.stats.cargoCapacity = s.cargoCapacity;
        t.stats.miningRate = s.miningRate;
        t.stats.hull = s.playerHull;
        t.stats.damage = s.playerDamage;
        t.stats.design = d->id;
        catalog.push_back(std::move(t));
    }
    builtFrom = Archetypes::Generation();
    built = true;
    return catalog;
}

const ShipType* ShipTypeOf(const std::string& design)
{
    for (const ShipType& t : GetShipCatalog())
        if (t.stats.design == design)
            return &t;
    return nullptr;
}

// Each switch names every tier and has no default, so a new tier is a compiler warning
// rather than a silent fall to list price -- which is how Hated came to be cheaper than
// Hostile (#219).
float ShipPriceMultiplier(RepTier standing)
{
    switch (standing)
    {
        case RepTier::Hated: return 1.30f;
        case RepTier::Hostile: return 1.15f;
        case RepTier::Neutral: return 1.0f;
        case RepTier::Liked: return 0.92f;
        case RepTier::Allied: return 0.85f;
    }
    return 1.0f;
}

float SellPriceMultiplier(RepTier standing)
{
    switch (standing)
    {
        case RepTier::Hated: return 0.70f;
        case RepTier::Hostile: return 0.85f;
        case RepTier::Neutral: return 1.0f;
        case RepTier::Liked: return 1.10f;
        case RepTier::Allied: return 1.20f;
    }
    return 1.0f;
}

float MissionRewardMultiplier(RepTier standing)
{
    switch (standing)
    {
        case RepTier::Hated: return 0.6f;
        case RepTier::Hostile: return 0.8f;
        case RepTier::Neutral: return 1.0f;
        case RepTier::Liked: return 1.15f;
        case RepTier::Allied: return 1.3f;
    }
    return 1.0f;
}
