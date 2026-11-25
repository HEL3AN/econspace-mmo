#pragma once

#include "entities/Entity.h"
#include "economy/Resource.h"

// How much a field with a life can hold as it ages (#617), as a share of what it was made with,
// against the share of its life gone: it rises through bloom, holds while it is rich, thins to
// a quarter of itself and is nothing at the end. The shares are the world rules' (and later an
// event kind's) at the moment it is born, and are saved with it, so a rule tuned afterwards
// never changes a field that is alive.
struct FieldCurve
{
    double bloomEnd = 0.10;   // share of life at which it is first full
    double bloomFrom = 0.40;  // what it holds when it begins
    double richEnd = 0.35;    // it holds full until here
    double thinEnd = 0.80;    // ...falls to `thinTo` by here
    double thinTo = 0.25;     // ...and to nothing by the end
    // The cap at `share` of the life gone: 0 before it began and after it ended.
    double Cap(double share) const;
    bool   operator==(const FieldCurve& o) const
    {
        return bloomEnd == o.bloomEnd && bloomFrom == o.bloomFrom && richEnd == o.richEnd &&
               thinEnd == o.thinEnd && thinTo == o.thinTo;
    }
};

// Asteroid field: holds ore of a single type that depletes as it's mined -- by players and by
// the system's miners alike -- and grows back over time (#348). A field may instead have a
// life (#617): it holds `cap(age)` less what was `taken`, never grows back, and is gone at its
// end. Ore it loses to the cap falling is dispersed -- a sink, never a mint.
class AsteroidField : public Entity
{
public:
    AsteroidField(Vector2 pos, float size, std::string name, ResourceType resource, int ore);

    std::unique_ptr<Entity> Clone() const override
    {
        return std::make_unique<AsteroidField>(*this);
    }
    Render::Item Describe() const override;
    std::string  GetName() const override { return name_; }

    ResourceType GetResource() const { return resource_; }
    bool         HasOre() const { return oreRemaining_ >= 1.0; }
    int          GetOreMax() const { return oreMax_; }  // what it was made with, and holds full
    double       GetOre() const { return oreRemaining_; }
    void         SetOre(double ore);  // clamped to 0..GetOreMax()

    // Takes up to amount whole units of ore, returns the amount actually extracted.
    int Extract(int amount);
    // Takes up to `amount` (a fraction is fine: the coarse pass's miners), returns what it got.
    double Take(double amount);
    // `hours` of growing back at `share` of the full belt an hour, up to full. A field with a
    // life does not.
    void Regrow(double share, double hours);

    // --- A field with a life (#617) -----------------------------------------------------
    // Born at world time `bornAt`, gone `life` seconds later (0: a belt, which has neither).
    // Until `Age` is called it holds nothing.
    void SetLife(double bornAt, double life, const FieldCurve& curve);
    bool HasLife() const { return life_ > 0.0; }
    // The world time it came to be, when it ends, how long it lives, and the curve it follows.
    double            BornAt() const { return bornAt_; }
    double            EndsAt() const { return bornAt_ + life_; }
    double            Life() const { return life_; }
    const FieldCurve& Curve() const { return curve_; }
    // What it can hold at world time `now`: the whole of what it was made with for a belt.
    double CapAt(double now) const;
    // What it holds at `now`: the cap less what was taken. A belt's is what is left.
    double OreAt(double now) const;
    // Brings what is held to `now` and says how much left unmined because the cap fell (the
    // dispersal); 0 for a belt.
    double Age(double now);
    // How much has been taken from it, by anyone; saved with it. A belt's is not kept.
    double Taken() const { return taken_; }
    void   SetTaken(double taken) { taken_ = taken > 0.0 ? taken : 0.0; }
    // How rich a pull is at `now`, between `floor` and 1: a thinning field is slower to work.
    double Density(double now, double floor) const;

private:
    std::string  name_;
    ResourceType resource_;
    double       oreRemaining_;
    int          oreMax_;
    double       bornAt_ = 0.0;
    double       life_ = 0.0;  // seconds; 0 -- a belt
    double       taken_ = 0.0;
    FieldCurve   curve_;
};
