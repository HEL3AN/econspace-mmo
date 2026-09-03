#include "entities/NpcShip.h"
#include "render/Textures.h"
#include <cmath>

NpcShip::NpcShip(Vector2 pos, FactionId faction, NpcRole role, std::vector<Vector2> waypoints)
    : Entity(pos, 10.0f, FactionColor(faction), EntityKind::Npc), faction_(faction), role_(role),
      waypoints_(std::move(waypoints)), target_(pos), speed_((float)GetRandomValue(120, 180)),
      heading_(0.0f), waitTimer_(0.0f)
{
    SetArchetype("ship.npc");
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
    float step = speed_ * dt;
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
