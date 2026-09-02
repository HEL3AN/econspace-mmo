#include "render/Scene.h"

#include <algorithm>

namespace Render
{

Item FromArchetype(const Archetype& a, Vector2 pos, float size)
{
    Item it;
    it.kind = a.kind;
    it.pos = pos;
    it.size = size > 0.0f ? size : a.defaultSize;
    it.color = a.visual.color;
    it.glyph = a.visual.glyph;
    it.sprite = a.visual.sprite;
    it.layer = a.visual.layer;
    it.style = a.visual.style;
    it.lightRadius = a.visual.lightRadius;
    it.lightIntensity = a.visual.lightIntensity;
    it.label = a.name;
    return it;
}

void Present(std::vector<Item> items, const Lighting& lighting, IBackend& backend)
{
    // Stable, so that two items on the same layer keep the order the world gave them.
    // Without that a backend would reshuffle overlapping objects between frames.
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.layer < b.layer; });

    // Stated every time, including when there is nothing to state. A backend outlives the
    // scene it drew, so leaving the previous frame's lights in place would light one view
    // by the star of another -- which is what a preview drawn beside a system would get.
    backend.SetLighting(lighting);

    backend.Begin();
    for (const Item& it : items)
        backend.Draw(it);
    backend.End();
}

void Present(std::vector<Item> items, IBackend& backend)
{
    Present(std::move(items), Lighting{}, backend);
}

}  // namespace Render
