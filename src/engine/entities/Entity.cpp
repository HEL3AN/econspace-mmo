#include "entities/Entity.h"

#include "raylib.h"
#include <set>
#include <string>

Entity::Entity(Vector2 pos, float size, Color color, EntityKind kind)
    : pos_(pos), size_(size), color_(color), kind_(kind)
{
}

void Entity::SetArchetype(const std::string& id)
{
    archetype_ = Archetypes::Find(id);
    if (archetype_ != nullptr)
        return;

    // Two ways to get here, and the loud one is the second: either world data names an
    // archetype that does not exist, or this entity was built before Archetypes::Load
    // ran. The second is easy to do and impossible to see -- the object just has no
    // glyph, no sprite and no components -- and the client shipped with the player's own
    // ship in exactly that state (#127).
    //
    // Once per id: an unloaded registry means every entity in the world misses, and a
    // thousand identical lines hide the one that matters.
    static std::set<std::string> reported;
    if (reported.insert(id).second)
        TraceLog(LOG_WARNING,
                 "Entity: no archetype '%s' -- it has no look and no components. Either the "
                 "id is wrong or this was built before Archetypes::Load()",
                 id.c_str());
}

void Entity::Update(float dt)
{
    (void)dt;  // the base object updates nothing
}

Render::Item Entity::Describe() const
{
    // The archetype supplies the look, colour included: colour in the world view is an art
    // decision about what a thing is (#117). It used to be the instance's, which meant a
    // station wore its faction -- and allegiance depends on who is looking, so one colour
    // was wrong for half the players in the system. Allegiance lives in the instruments
    // now (radar, overview, target panel). The instance's colour is only the fallback for
    // an entity with no archetype.
    Render::Item it;
    if (archetype_ != nullptr)
        it = Render::FromArchetype(*archetype_, pos_, size_);
    else
        it.color = color_;
    it.id = id_;
    it.kind = kind_;
    it.pos = pos_;
    it.size = size_;
    it.label = GetName();
    return it;
}
