#include "entities/NpcShip.h"
#include "core/Archetype.h"
#include "core/World.h"
#include "render/Textures.h"
#include <cmath>

const char* NpcRoleId(NpcRole role)
{
    switch (role)
    {
        case NpcRole::Trader: return "trader";
        case NpcRole::Miner: return "miner";
        case NpcRole::Police: return "patrol";
        case NpcRole::Pirate: return "pirate";
        case NpcRole::Warship: return "warship";
    }
    return "trader";
}

NpcShip::NpcShip(Vector2 pos, FactionId faction, NpcRole role, std::vector<Vector2> waypoints,
                 const std::string& design)
    : Entity(pos, 10.0f, LIGHTGRAY, EntityKind::Npc), faction_(faction), role_(role),
      waypoints_(std::move(waypoints)), target_(pos), speed_(150.0f), heading_(0.0f),
      waitTimer_(0.0f)
{
    // The design it flies is its look and its numbers (#279 step 4). There used to be one
    // shape for every role, `ship.npc`, and a speed rolled between 120 and 180; both are the
    // design's now, and `ship.npc` is gone -- a design nobody knows falls back to the
    // doctrine's own pick, never to a shape that says nothing about the ship.
    const Archetype* a = Archetypes::ForDesign(design);
    if (a == nullptr)
        a = Archetypes::ForDesign(
            Archetypes::ShipCatalogue().Pick(Factions::Id(faction), NpcRoleId(role), 0));
    if (a != nullptr)
    {
        design_ = a->design;
        SetArchetype(a->id);
        size_ = a->defaultSize;
        speed_ = a->ship.cruise;
        damage_ = a->ship.damage;
        maxHull_ = hull_ = a->ship.hull;
    }
    PickNewTarget();
}

Render::Item NpcShip::Describe() const
{
    Render::Item it = Entity::Describe();
    it.heading = heading_;
    it.intensity = maxHull_ > 0.0f ? hull_ / maxHull_ : 1.0f;
    return it;
}

void NpcShip::TakeDamage(float amount)
{
    hull_ -= amount;
    if (hull_ < 0.0f)
        hull_ = 0.0f;
}

void NpcShip::Engage(Vector2 targetPos)
{
    state_ = AiState::Pursue;
    aiPoint_ = targetPos;
}

void NpcShip::FleeFrom(Vector2 threatPos)
{
    state_ = AiState::Flee;
    aiPoint_ = threatPos;
}

void NpcShip::StandDown()
{
    state_ = AiState::Patrol;
}

void NpcShip::PickNewTarget()
{
    if (!waypoints_.empty())
        target_ = waypoints_[GetRandomValue(0, (int)waypoints_.size() - 1)];
}

// Move toward dest; heading turns toward it, braking to a stop at stopDist.
void NpcShip::MoveToward(Vector2 dest, float dt, float stopDist)
{
    float dx = dest.x - pos_.x;
    float dy = dest.y - pos_.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 1.0f)
        heading_ = atan2f(dy, dx);
    if (dist > stopDist)
    {
        float step = speed_ * dt;
        if (step > dist - stopDist)
            step = dist - stopDist;
        pos_.x += dx / dist * step;
        pos_.y += dy / dist * step;
    }
}

void NpcShip::Update(float dt)
{
    if (fireTimer_ > 0.0f)
        fireTimer_ -= dt;

    // Pursuit: hold firing range so we shoot rather than ram.
    if (state_ == AiState::Pursue)
    {
        MoveToward(aiPoint_, dt, 170.0f);
        return;
    }

    // Flee: run from the threat point at full speed.
    if (state_ == AiState::Flee)
    {
        float dx = pos_.x - aiPoint_.x;
        float dy = pos_.y - aiPoint_.y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > 1.0f)
        {
            heading_ = atan2f(dy, dx);
            float step = speed_ * 1.2f * dt;  // panic — a bit faster
            pos_.x += dx / dist * step;
            pos_.y += dy / dist * step;
        }
        return;
    }

    // Patrol: fly between system waypoints, pausing on arrival.
    if (waitTimer_ > 0.0f)
    {
        waitTimer_ -= dt;
        return;
    }

    float dx = target_.x - pos_.x;
    float dy = target_.y - pos_.y;
    float dist = sqrtf(dx * dx + dy * dy);

    if (dist < 14.0f)
    {
        waitTimer_ = (float)GetRandomValue(1, 4);
        PickNewTarget();
        return;
    }

    heading_ = atan2f(dy, dx);
    // A long leg is crossed at warp, as a player would (#159): at 150 units a second the
    // next station is half an hour away and the lanes would be empty. The last stretch is
    // flown, so an NPC arrives the way a player sees one arrive.
    constexpr float WARP_FROM = World::WARP_WORTH_IT;
    constexpr float WARP_DROP = 3000.0f;
    constexpr float NPC_WARP_SPEED = 60000.0f;
    float           step = (dist > WARP_FROM ? NPC_WARP_SPEED : speed_) * dt;
    if (dist > WARP_FROM && step > dist - WARP_DROP)
        step = dist - WARP_DROP;
    if (step > dist)
        step = dist;
    pos_.x += dx / dist * step;
    pos_.y += dy / dist * step;
}

std::string NpcShip::GetName() const
{
    const char* roleName = "ship";
    switch (role_)
    {
        case NpcRole::Trader: roleName = "trader"; break;
        case NpcRole::Miner: roleName = "miner"; break;
        case NpcRole::Police: roleName = "patrol"; break;
        case NpcRole::Pirate: roleName = "raider"; break;
        case NpcRole::Warship: roleName = "warship"; break;
    }
    return FactionName(faction_) + " " + roleName;
}
