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
    // The archetype supplies the look; the instance overrides what only it knows. The
    // colour is the instance's on purpose -- faction paint and planet type are chosen per
    // object, and the archetype's colour is the default they start from.
    Render::Item it;
    if (archetype_ != nullptr)
        it = Render::FromArchetype(*archetype_, pos_, size_);
    it.id = id_;
    it.kind = kind_;
    it.pos = pos_;
    it.size = size_;
    it.color = color_;
    it.label = GetName();
    return it;
}
