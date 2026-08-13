#include "entities/NpcShip.h"
#include "render/Textures.h"
#include <cmath>

NpcShip::NpcShip(Vector2 pos, FactionId faction, NpcRole role, std::vector<Vector2> waypoints)
    : Entity(pos, 10.0f, FactionColor(faction)), faction_(faction), role_(role),
      waypoints_(std::move(waypoints)), target_(pos), speed_((float)GetRandomValue(120, 180)),
      heading_(0.0f), waitTimer_(0.0f)
{
    PickNewTarget();
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

void NpcShip::Draw() const
{
    float c = cosf(heading_);
    float s = sinf(heading_);
    auto  toWorld = [&](float x, float y) -> Vector2
    { return { pos_.x + x * c - y * s, pos_.y + x * s + y * c }; };

    // The sprite is drawn nose-up, so add 90 degrees to the heading.
    if (!Tex::DrawSprite("ship", pos_, size_, heading_ * RAD2DEG + 90.0f, color_))
    {
        Vector2 nose = toWorld(size_, 0.0f);
        Vector2 left = toWorld(-size_ * 0.7f, size_ * 0.6f);
        Vector2 right = toWorld(-size_ * 0.7f, -size_ * 0.6f);
        DrawTriangle(nose, right, left, color_);
    }

    // Hull bar above a damaged ship.
    if (hull_ < maxHull_)
    {
        float   frac = hull_ / maxHull_;
        float   barW = size_ * 2.0f;
        Vector2 barPos = { pos_.x - barW / 2, pos_.y - size_ - 8.0f };
        DrawRectangleV(barPos, { barW, 3.0f }, Fade(GRAY, 0.5f));
        DrawRectangleV(barPos, { barW * frac, 3.0f }, RED);
    }
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
