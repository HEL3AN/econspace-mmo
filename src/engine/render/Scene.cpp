#include "render/Scene.h"

#include <algorithm>

namespace Render
{

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
