#include "entities/Structure.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"

Structure::Structure(Vector2 pos, float size, std::string name, std::string builds)
    : Entity(pos, size, Color{ 200, 200, 210, 255 }, EntityKind::Structure), name_(std::move(name)),
      builds_(std::move(builds))
{
    SetArchetype(builds_);
    // The size the instance gives, else the archetype's: a beacon is as large as a beacon.
    if (size_ <= 0.0f && archetype_ != nullptr)
        size_ = archetype_->defaultSize;
}

void Structure::StartBuilding(double startedAt, double completesAt)
{
    startedAt_ = startedAt;
    completesAt_ = completesAt;
    if (IsBuilding())
        SetArchetype(SITE_ARCHETYPE);
}

void Structure::Retask(std::string builds, std::string blueprint, std::string name)
{
    // What it was is a tier it grew from (#438); one that never said (a save from before)
    // keeps nothing here, and EarlierTiers finds its way down by `onto`.
    if (!blueprint_.empty())
        grewFrom_.push_back(blueprint_);
    builds_ = std::move(builds);
    blueprint_ = std::move(blueprint);
    name_ = std::move(name);
    SetArchetype(builds_);
}

void Structure::Complete(double now)
{
    startedAt_ = 0.0;
    completesAt_ = 0.0;
    damagedAt_ = now;  // a site does not mend: what it took while going up, it mends from here
    SetArchetype(builds_);
}

float Structure::MendRate() const
{
    if (IsBuilding() || !mends_)
        return 0.0f;
    const Blueprint* bp = GetBlueprint();
    return bp != nullptr ? bp->mend * bp->hull : 0.0f;
}

float Structure::GetDamage(double now) const
{
    if (damage_ <= 0.0f)
        return 0.0f;
    const double since = now - damagedAt_;
    const double left = damage_ - (since > 0.0 ? since * (double)MendRate() : 0.0);
    return left > 0.0 ? (float)left : 0.0f;
}

void Structure::SetDamage(float d, double at)
{
    damage_ = d < 0.0f ? 0.0f : d;
    damagedAt_ = at;
}

void Structure::SetMends(bool on, double now)
{
    if (on == mends_)
        return;
    SetDamage(GetDamage(now), now);
    mends_ = on;
}

float Structure::Progress(double now) const
{
    if (!IsBuilding())
        return 1.0f;
    const double span = completesAt_ - startedAt_;
    if (span <= 0.0)
        return 1.0f;
    const double f = (now - startedAt_) / span;
    return (float)(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
}

const Blueprint* Structure::GetBlueprint() const
{
    if (!blueprint_.empty())
        return Blueprints::Find(blueprint_);
    return Blueprints::Building(builds_);
}

float Structure::MaxHull(double now) const
{
    const Blueprint* bp = GetBlueprint();
    if (bp == nullptr)
        return 0.0f;
    if (!IsBuilding())
        return bp->hull;
    const float start = Blueprints::Rules().siteHull;
    return bp->hull * (start + (1.0f - start) * Progress(now));
}

float Structure::HullFraction(double now) const
{
    const float max = MaxHull(now);
    if (max <= 0.0f)
        return 1.0f;  // nothing says what it takes: nothing has been taken
    const float left = (max - GetDamage(now)) / max;
    return left < 0.0f ? 0.0f : left;
}

std::vector<const Blueprint*> Structure::EarlierTiers() const
{
    std::vector<const Blueprint*> out;
    for (const std::string& id : grewFrom_)
        if (const Blueprint* b = Blueprints::Find(id))
            out.push_back(b);
    if (!grewFrom_.empty())
        return out;
    // Down the first `onto` of each, a few tiers at most: a file that names a loop is not
    // a reason to spin.
    const Blueprint* b = GetBlueprint();
    for (int depth = 0; b != nullptr && !b->onto.empty() && depth < 8; depth++)
    {
        b = Blueprints::Find(b->onto.front());
        if (b != nullptr)
            out.insert(out.begin(), b);
    }
    return out;
}

std::vector<std::pair<ResourceType, int>> Structure::Refund(double now) const
{
    const Blueprint* bp = GetBlueprint();
    if (bp == nullptr)
        return {};
    return Blueprints::Refund(*bp, Progress(now), HullFraction(now), EarlierTiers());
}

bool Structure::OwnersSide(const std::string& pilot, bool corpMate) const
{
    return !pilot.empty() && (GetOwner() == pilot || corpMate);
}

bool Structure::DocksPilot(const std::string& pilot, bool corpMate) const
{
    if (IsBuilding() || !Has(Component::Dockable))
        return false;
    return Has(Component::Market) || OwnersSide(pilot, corpMate);
}

int Structure::GetStoredAmount(ResourceType type) const
{
    const auto it = stored_.find(type);
    return it == stored_.end() ? 0 : it->second;
}

int Structure::GetStoredUsed() const
{
    int n = 0;
    for (const auto& kv : stored_)
        n += kv.second;
    return n;
}

int Structure::GetStorageCapacity() const
{
    if (IsBuilding() || !Has(Component::Storage))
        return 0;
    return (int)archetype_->storageCapacity;
}

void Structure::AddStored(ResourceType type, int amount)
{
    if (amount > 0)
        stored_[type] += amount;
}

void Structure::RemoveStored(ResourceType type, int amount)
{
    const auto it = stored_.find(type);
    if (it == stored_.end() || amount <= 0)
        return;
    it->second -= amount;
    if (it->second <= 0)
        stored_.erase(it);
}
