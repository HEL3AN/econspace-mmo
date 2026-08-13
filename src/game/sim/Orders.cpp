#include "sim/Orders.h"

namespace Orders
{

const char* KindName(Kind k)
{
    switch (k)
    {
        case Kind::None: return "none";
        case Kind::MoveTo: return "move";
        case Kind::Dock: return "dock";
        case Kind::Undock: return "undock";
        case Kind::Mine: return "mine";
    }
    return "none";
}

}  // namespace Orders
