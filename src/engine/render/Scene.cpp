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
    it.label = a.name;
    return it;
}

void Present(std::vector<Item> items, IBackend& backend)
{
    // Stable, so that two items on the same layer keep the order the world gave them.
    // Without that a backend would reshuffle overlapping objects between frames.
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.layer < b.layer; });

    backend.Begin();
    for (const Item& it : items)
        backend.Draw(it);
    backend.End();
}

}  // namespace Render
