// Fields with a life (#617, documents/mining_and_fields.md): placed by hand for now, worked by
// the system's miners at the rate their design gives, and gone when their time is up.

#include "sim/Simulation.h"

#include "core/Archetype.h"
#include "core/Faction.h"
#include "entities/AsteroidField.h"
#include "entities/Ship.h"
#include "sim/WorldRules.h"

#include <algorithm>
#include <cmath>

void Simulation::WorkFields(SystemState& st, const std::vector<AsteroidField*>& fields,
                            double hours)
{
    const WorldRules::Fields& rules = WorldRules::FieldRules();
    // The doctrine every faction shares: a miner is a design in data, and what it carries and
    // how fast it mines are what a player's ship of it has (#463, one model for both).
    if (minerFrom_ != Archetypes::Generation())
    {
        miner_ =
            Ships::RoleMiner(Archetypes::ShipCatalogue(), Factions::Id(Factions::Unaffiliated()));
        minerFrom_ = Archetypes::Generation();
    }
    const double ships = std::min((double)st.agg.miners, rules.crew);
    if (ships <= 0.0 || miner_.rate <= 0.0f || miner_.hold <= 0.0f)
        return;
    std::vector<AsteroidField*> open;
    for (AsteroidField* f : fields)
        if (f->HasOre())
            open.push_back(f);
    if (open.empty())
        return;
    for (AsteroidField* f : open)
    {
        // A turn is the hold filled at the field's richness, flown to the nearest market and
        // back at cruise, and the time at the dock: ore a second is hold over that.
        const double extract = f->GetArchetype() != nullptr ? f->GetArchetype()->extractRate : 1.0;
        double       nearest = 0.0;
        bool         found = false;
        for (const auto& e : st.entities)
            if (e->Has(Component::Market))
            {
                const double dx = e->GetPosition().x - f->GetPosition().x;
                const double dy = e->GetPosition().y - f->GetPosition().y;
                const double d = std::sqrt(dx * dx + dy * dy);
                if (!found || d < nearest)
                {
                    nearest = d;
                    found = true;
                }
            }
        const double fill = (double)miner_.hold / std::max(1e-6, (double)miner_.rate * extract);
        const double fly = miner_.speed > 0.0f ? 2.0 * nearest / (double)miner_.speed : 0.0;
        const double perShip = (double)miner_.hold / (fill + fly + rules.dockSeconds);
        const double share = ships / (double)open.size();
        // Only what the shelf has room for: a crew does not fill a market past its cap.
        const double want =
            std::min(perShip * 3600.0 * hours * share, st.market.Room(f->GetResource()));
        if (want <= 0.0)
            continue;
        const double mined = f->Take(want);
        st.market.SetStock(f->GetResource(), st.market.Stock(f->GetResource()) + mined);
        st.market.NoteMined(f->GetResource(), mined, true);
    }
}

int Simulation::PlaceField(const std::string& systemId, ResourceType ore, int amount, double hours,
                           float size, Vector2 pos)
{
    if (SystemById(systemId) == nullptr || amount <= 0 || !(hours > 0.0) || !(size > 0.0f))
        return 0;
    auto field =
        std::make_unique<AsteroidField>(pos, size, "Loose " + ResourceName(ore), ore, amount);
    field->SetLife(time_, hours * 3600.0, WorldRules::FieldRules().curve);
    field->Age(time_);
    return AddStatic(systemId, std::move(field), std::string());
}

Vector2 Simulation::FieldSpot(const std::string& systemId, const ClientSession* pilot) const
{
    if (pilot != nullptr && pilot->ship)
    {
        const float h = pilot->ship->GetHeading();
        return { pilot->ship->GetPosition().x + std::cos(h) * 2000.0f,
                 pilot->ship->GetPosition().y + std::sin(h) * 2000.0f };
    }
    const SystemState* st = SystemById(systemId);
    if (st != nullptr)
        for (const auto& e : st->entities)
            if (e->Has(Component::Market))
                return { e->GetPosition().x + 4000.0f, e->GetPosition().y };
    return { 4000.0f, 0.0f };
}
