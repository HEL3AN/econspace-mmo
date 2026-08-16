#pragma once

#include "entities/EntityKind.h"
#include "core/Archetype.h"
#include "raylib.h"
#include <memory>
#include <string>

// Base class for everything that exists in space and knows how to draw itself:
// star, planet, station, ship. Only subclasses are instantiated.
class Entity
{
public:
    Entity(Vector2 pos, float size, Color color, EntityKind kind = EntityKind::Unknown);

    // Virtual: subclasses are deleted through an Entity* pointer.
    virtual ~Entity() = default;

    virtual void Update(float dt);
    virtual void Draw() const;

    virtual std::string GetName() const { return "Object"; }

    // Client proxy: a copy for client-side rendering from the snapshot/layout
    // (M4c). World entities override this; others return nullptr.
    virtual std::unique_ptr<Entity> Clone() const { return nullptr; }

    Vector2 GetPosition() const { return pos_; }
    void    SetPosition(Vector2 p) { pos_ = p; }  // for reconciling the proxy from a snapshot
    float   GetSize() const { return size_; }
    Color   GetColor() const { return color_; }

    // What this object is. Set once by the subclass constructor rather than probed with
    // dynamic_cast: the answer never changes, and asking costs nothing.
    EntityKind GetKind() const { return kind_; }

    // What this object CAN DO, from the registry (#34). Null when the archetype file has
    // not been loaded or names no archetype for this object, which is why every caller
    // asks through Has() rather than dereferencing.
    const Archetype* GetArchetype() const { return archetype_; }
    void             SetArchetype(const Archetype* a) { archetype_ = a; }

    // The question simulation passes actually want to ask. "Everything dockable within
    // range" survives a player inventing a new kind of dock; "everything that is a
    // Station" does not.
    bool Has(Component c) const { return archetype_ != nullptr && archetype_->Has(c); }

    // Stable entity id within the galaxy (for snapshots/network selection, track M).
    // 0 means unassigned; assigned on materialization/spawn from the agent counter.
    int  GetId() const { return id_; }
    void SetId(int id) { id_ = id; }

protected:
    Vector2    pos_;
    float      size_;
    Color      color_;
    EntityKind kind_ = EntityKind::Unknown;
    int        id_ = 0;
    // Borrowed from the process-wide registry. Valid until the next Archetypes::Load(),
    // which every executable calls once at startup, before any world is built.
    const Archetype* archetype_ = nullptr;
};
