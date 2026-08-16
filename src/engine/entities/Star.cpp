#include "entities/Star.h"
#include "render/Textures.h"

static Color ColorForStarType(StarType type)
{
    switch (type)
    {
        case StarType::Yellow: return YELLOW;
        case StarType::Red: return RED;
        case StarType::Blue: return SKYBLUE;
    }
    return WHITE;
}

static const char* ArchetypeIdForStarType(StarType type)
{
    switch (type)
    {
        case StarType::Yellow: return "star.yellow";
        case StarType::Red: return "star.red";
        case StarType::Blue: return "star.blue";
    }
    return "star.yellow";
}

Star::Star(Vector2 pos, float size, StarType type)
    : Entity(pos, size, ColorForStarType(type), EntityKind::Star), type_(type)
{
    SetArchetype(Archetypes::Find(ArchetypeIdForStarType(type)));
}
