#pragma once

#include "entities/Entity.h"
#include <string>

// Something a player put into the world (#39). What it is and what it can do come from its
// archetype entirely -- a beacon and a depot are the same class -- so a new kind of
// deployable is a line of data, not a subclass. The class adds only what an archetype
// cannot say: the name its owner gave it, and its time line.
//
// The time line is three instants of the world's clock, not a timer (#136). From
// `startedAt` until `completesAt` it is a construction site: it wears the site's archetype
// and can do nothing yet. After that it is what it was built to be, until `expiresAt`, if
// it has one. Nothing accumulates, so a site is as far along on every client as on the
// server, and a restart in the middle of building costs nothing.
class Structure : public Entity
{
public:
    // How every site looks, whatever it will become.
    static constexpr const char* SITE_ARCHETYPE = "structure.site";

    Structure(Vector2 pos, float size, std::string name, std::string builds);

    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Structure>(*this); }
    std::string             GetName() const override { return name_; }

    // The archetype it is, or will be once built.
    const std::string& GetBuilds() const { return builds_; }

    // Makes it a site until `completesAt`. Wears the site's look meanwhile.
    void StartBuilding(double startedAt, double completesAt);
    // The site is finished: from now on it is what it was built to be.
    void Complete();
    bool IsBuilding() const { return completesAt_ > 0.0; }
    // 0..1 at world time `now`; 1 for something already built.
    float Progress(double now) const;

    double GetStartedAt() const { return startedAt_; }
    double GetCompletesAt() const { return completesAt_; }

    // When it is taken out of the world by itself (#41's decay, in its simplest form); 0
    // means it stays until somebody removes it.
    double GetExpiresAt() const { return expiresAt_; }
    void   SetExpiresAt(double t) { expiresAt_ = t; }

private:
    std::string name_;
    std::string builds_;
    double      startedAt_ = 0.0;
    double      completesAt_ = 0.0;  // 0: built
    double      expiresAt_ = 0.0;    // 0: permanent
};
