#include "entities/Ship.h"
#include "core/World.h"
#include "render/Textures.h"
#include <cmath>

Ship::Ship(Vector2 startPos, const ShipStats& stats)
    : Entity(startPos, 16.0f, RAYWHITE, EntityKind::PlayerShip), stats_(stats), heading_(0.0f),
      velocity_({ 0.0f, 0.0f })
{
    SetArchetype("ship.player");
}

Render::Item Ship::Describe() const
{
    Render::Item it = Entity::Describe();
    it.heading = heading_;
    it.thrusting = engineActive_;
    it.intensity = maxHull_ > 0.0f ? hull_ / maxHull_ : 1.0f;
    return it;
}

float Ship::GetSpeed() const
{
    return sqrtf(velocity_.x * velocity_.x + velocity_.y * velocity_.y);
}

bool Ship::AddCargo(ResourceType type, int amount)
{
    if (GetCargoUsed() + amount > stats_.cargoCapacity)
        return false;
    cargo_[type] += amount;
    return true;
}

void Ship::RemoveCargo(ResourceType type, int amount)
{
    auto it = cargo_.find(type);
    if (it == cargo_.end())
        return;
    it->second -= amount;
    if (it->second < 0)
        it->second = 0;
}

int Ship::GetCargoAmount(ResourceType type) const
{
    auto it = cargo_.find(type);
    return (it != cargo_.end()) ? it->second : 0;
}

int Ship::GetCargoUsed() const
{
    int sum = 0;
    for (const auto& [type, amount] : cargo_)
        sum += amount;
    return sum;
}

void Ship::ApplyView(Vector2 pos, float heading, Vector2 vel, float hull, float shields)
{
    pos_ = pos;
    heading_ = heading;
    velocity_ = vel;
    hull_ = hull;
    shields_ = shields;
}

void Ship::ApplyNavView(int warpPhase, float warpAlignTimer, Vector2 warpTarget, float warpDrop,
                        bool apActive, Vector2 apTarget, float apStopDistance, int holdMode,
                        int holdTargetId, float holdRange)
{
    warpPhase_ = (WarpPhase)warpPhase;
    warpAlignTimer_ = warpAlignTimer;
    warpTarget_ = warpTarget;
    warpDrop_ = warpDrop;
    warpPrevDist_ = 1e9f;  // reset the stuck guard: the client only coasts in
    apActive_ = apActive;
    apArrived_ = false;
    apTarget_ = apTarget;
    apStopDistance_ = apStopDistance;
    holdMode_ =
        (holdMode == 1) ? HoldMode::Orbit : (holdMode == 2 ? HoldMode::Keep : HoldMode::None);
    holdTargetId_ = holdTargetId;
    holdRange_ = holdRange;
}

void Ship::TakeDamage(float amount)
{
    // Shields absorb the damage first, the remainder hits the hull.
    if (shields_ > 0.0f)
    {
        if (amount <= shields_)
        {
            shields_ -= amount;
            return;
        }
        amount -= shields_;
        shields_ = 0.0f;
    }
    hull_ -= amount;
    if (hull_ < 0.0f)
        hull_ = 0.0f;
}

void Ship::SetControls(bool thrust, float turn, bool brake)
{
    ctrlThrust_ = thrust;
    ctrlTurn_ = turn;
    ctrlBrake_ = brake;
}

void Ship::EngageAutopilot(Vector2 target, float stopDistance)
{
    apActive_ = true;
    apArrived_ = false;
    apTarget_ = target;
    apStopDistance_ = stopDistance;
}

void Ship::EngageHold(HoldMode mode, int targetId, float range)
{
    holdMode_ = mode;
    holdTargetId_ = targetId;
    holdRange_ = range < 1.0f ? 1.0f : range;
    apActive_ = false;  // the hold drives the autopilot from now on
    CancelWarp();
}

void Ship::ReleaseHold()
{
    holdMode_ = HoldMode::None;
    holdTargetId_ = 0;
}

void Ship::UpdateHold(Vector2 targetPos)
{
    if (holdMode_ == HoldMode::None)
        return;

    float dx = pos_.x - targetPos.x;
    float dy = pos_.y - targetPos.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.001f)
    {
        // Sitting exactly on top of it: pick a direction rather than dividing by zero. Any
        // direction is as good as any other and the next tick will have a real one.
        dx = 1.0f;
        dy = 0.0f;
        dist = 1.0f;
    }
    float ux = dx / dist, uy = dy / dist;

    if (holdMode_ == HoldMode::Orbit)
    {
        // Aim at a point further round the circle than the ship currently is, so it keeps
        // moving instead of converging. Aiming *at* the ring would stop the ship on it.
        const float a = ORBIT_LEAD_DEGREES * DEG2RAD;
        const float cs = cosf(a), sn = sinf(a);
        const float rx = ux * cs - uy * sn;
        const float ry = ux * sn + uy * cs;
        ux = rx;
        uy = ry;
    }

    // Fly to the ring and stop there. For an orbit the aim point keeps moving ahead, so
    // arriving never happens and the ship circles; for a keep it does, and stopping is
    // exactly right.
    EngageAutopilot({ targetPos.x + ux * holdRange_, targetPos.y + uy * holdRange_ },
                    holdRange_ * 0.08f);
}

void Ship::EngageWarp(Vector2 target, float dropDistance)
{
    // Don't allow a target outside the system boundary — warp couldn't reach it.
    float d2 = target.x * target.x + target.y * target.y;
    float lim = World::SYSTEM_RADIUS - 5.0f;
    if (d2 > lim * lim)
    {
        float d = sqrtf(d2);
        target.x = target.x / d * lim;
        target.y = target.y / d * lim;
    }

    apActive_ = false;  // warp cancels the autopilot
    warpPhase_ = WarpPhase::Aligning;
    warpTarget_ = target;
    warpDrop_ = dropDistance;
    warpAlignTimer_ = WARP_ALIGN_TIME;  // spin-up before the jump (alignment)
    warpPrevDist_ = 1e9f;
}

// Aborting warp (manual control, etc.). We clamp the huge warp speed back to the
// normal max, otherwise the ship is nearly impossible to control.
void Ship::CancelWarp()
{
    if (warpPhase_ == WarpPhase::None)
        return;
    warpPhase_ = WarpPhase::None;

    float sp = GetSpeed();
    if (sp > MaxSpeed())
    {
        float k = MaxSpeed() / sp;
        velocity_.x *= k;
        velocity_.y *= k;
    }
}

// Warp: alignment phase (nose on target, spin-up), then a high-speed jump that
// drops out at warpDrop_.
void Ship::RunWarp(float dt)
{
    static const float WARP_SPEED = 5000.0f;

    float dx = warpTarget_.x - pos_.x;
    float dy = warpTarget_.y - pos_.y;
    float dist = sqrtf(dx * dx + dy * dy);

    if (dist <= warpDrop_)
    {
        warpPhase_ = WarpPhase::None;  // drop out of warp
        velocity_ = { 0.0f, 0.0f };
        engineActive_ = false;
        return;
    }

    float dirX = dx / dist, dirY = dy / dist;
    heading_ = atan2f(dirY, dirX);  // nose locked on target

    if (warpPhase_ == WarpPhase::Aligning)
    {
        velocity_ = { 0.0f, 0.0f };
        warpAlignTimer_ -= dt;
        if (warpAlignTimer_ <= 0.0f)
            warpPhase_ = WarpPhase::Warping;
        return;
    }

    // Stuck guard: if the distance stops shrinking (hit the boundary, etc.)
    // then drop out of warp.
    if (dist >= warpPrevDist_)
    {
        warpPhase_ = WarpPhase::None;
        velocity_ = { 0.0f, 0.0f };
        engineActive_ = false;
        return;
    }
    warpPrevDist_ = dist;

    // Jump: cap the speed so we don't overshoot the drop-out point in one frame.
    float speed = WARP_SPEED;
    float maxBeforeDrop = (dist - warpDrop_) / dt;
    if (speed > maxBeforeDrop)
        speed = maxBeforeDrop;
    velocity_ = { dirX * speed, dirY * speed };
    engineActive_ = true;
}

void Ship::Update(float dt)
{
    if (warpPhase_ != WarpPhase::None)
    {
        // Warp drives velocity directly, bypassing the normal flight models.
        RunWarp(dt);
    }
    else
    {
        if (apActive_)
        {
            RunAutopilot(dt);
        }
        else
        {
            // Manual control: turn the nose and derive desired velocity from input.
            heading_ += ctrlTurn_ * stats_.turnSpeed * dt;

            if (ctrlThrust_ && !ctrlBrake_)
                desiredVelocity_ = { cosf(heading_) * MaxSpeed(), sinf(heading_) * MaxSpeed() };
            else
                desiredVelocity_ = { 0.0f, 0.0f };
        }

        // Autopilot always flies via the stabilizer; in manual mode it's optional.
        if (stabilizerOn_ || apActive_)
            ApplyStabilizer(dt);
        else
            ApplyNewtonFlight(dt);
    }

    pos_.x += velocity_.x * dt;
    pos_.y += velocity_.y * dt;

    // Soft system boundary: keep the ship inside the radius, cancel outward
    // velocity (flying inward is still allowed).
    float distSq = pos_.x * pos_.x + pos_.y * pos_.y;
    if (distSq > World::SYSTEM_RADIUS * World::SYSTEM_RADIUS)
    {
        float d = sqrtf(distSq);
        float nx = pos_.x / d, ny = pos_.y / d;
        pos_.x = nx * World::SYSTEM_RADIUS;
        pos_.y = ny * World::SYSTEM_RADIUS;
        float vOut = velocity_.x * nx + velocity_.y * ny;
        if (vOut > 0.0f)
        {
            velocity_.x -= vOut * nx;
            velocity_.y -= vOut * ny;
        }
    }

    // Shields regenerate gradually.
    shields_ += shieldRegen_ * dt;
    if (shields_ > maxShields_)
        shields_ = maxShields_;
}

// Autopilot sets the desired velocity toward the target; the stabilizer handles
// canceling inertia, so there's no need to turn around to brake.
void Ship::RunAutopilot(float dt)
{
    float dx = apTarget_.x - pos_.x;
    float dy = apTarget_.y - pos_.y;
    float dist = sqrtf(dx * dx + dy * dy);

    if (apArrived_ || dist <= apStopDistance_)
    {
        apArrived_ = true;
        desiredVelocity_ = { 0.0f, 0.0f };
        if (GetSpeed() < 12.0f)
        {
            velocity_ = { 0.0f, 0.0f };
            apActive_ = false;
        }
        return;
    }

    float dirX = dx / dist;
    float dirY = dy / dist;

    // Desired speed shrinks as the target nears — the ship brakes itself.
    float desiredSpeed = dist * 1.5f;
    if (desiredSpeed > MaxSpeed())
        desiredSpeed = MaxSpeed();
    desiredVelocity_ = { dirX * desiredSpeed, dirY * desiredSpeed };

    // The nose eases around toward the flight heading (cosmetic).
    float desired = atan2f(dirY, dirX);
    float diff = desired - heading_;
    while (diff > PI)
        diff -= 2.0f * PI;
    while (diff < -PI)
        diff += 2.0f * PI;
    float step = stats_.turnSpeed * dt;
    heading_ += (fabsf(diff) <= step) ? diff : (diff > 0 ? step : -step);
}

// Stabilizer: omni-thrusters pull velocity toward desiredVelocity_ in any
// direction, capped by the acceleration limit.
void Ship::ApplyStabilizer(float dt)
{
    float dvx = desiredVelocity_.x - velocity_.x;
    float dvy = desiredVelocity_.y - velocity_.y;
    float dvMag = sqrtf(dvx * dvx + dvy * dvy);

    float maxStep = RcsAccel() * dt;
    if (dvMag <= maxStep)
        velocity_ = desiredVelocity_;
    else
    {
        velocity_.x += dvx * (maxStep / dvMag);
        velocity_.y += dvy * (maxStep / dvMag);
    }

    // The flame burns while velocity is being corrected (accel/maneuver) OR while
    // the player holds thrust — otherwise at cruise speed (dvMag~0) it would go out.
    engineActive_ = dvMag > 20.0f || ctrlThrust_;
}

// Classic inertia: thrust only along the nose, velocity is conserved.
void Ship::ApplyNewtonFlight(float dt)
{
    if (ctrlThrust_)
    {
        Vector2 dir = { cosf(heading_), sinf(heading_) };
        velocity_.x += dir.x * ThrustPower() * dt;
        velocity_.y += dir.y * ThrustPower() * dt;
    }

    if (ctrlBrake_)
    {
        float factor = 1.0f - 1.8f * dt;
        if (factor < 0.0f)
            factor = 0.0f;
        velocity_.x *= factor;
        velocity_.y *= factor;
    }

    float speed = GetSpeed();
    if (speed > MaxSpeed())
    {
        float k = MaxSpeed() / speed;
        velocity_.x *= k;
        velocity_.y *= k;
    }

    // Light drag: without thrust the ship coasts down very slowly.
    float drag = 1.0f - 0.18f * dt;
    velocity_.x *= drag;
    velocity_.y *= drag;

    engineActive_ = ctrlThrust_;
}
