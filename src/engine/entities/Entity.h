#pragma once

#include "raylib.h"
#include <memory>
#include <string>

// Base class for everything that exists in space and knows how to draw itself:
// star, planet, station, ship. Only subclasses are instantiated.
class Entity
{
public:
    Entity(Vector2 pos, float size, Color color);

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

    // Stable entity id within the galaxy (for snapshots/network selection, track M).
    // 0 means unassigned; assigned on materialization/spawn from the agent counter.
    int  GetId() const { return id_; }
    void SetId(int id) { id_ = id; }

protected:
    Vector2 pos_;
    float   size_;
    Color   color_;
    int     id_ = 0;
};
