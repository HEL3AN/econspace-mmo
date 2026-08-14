#include "sim/Events.h"

namespace Ev
{

const char* KindName(Kind k)
{
    switch (k)
    {
        case Kind::Notice: return "notice";
        case Kind::OrderDone: return "order_done";
        case Kind::OrderFailed: return "order_failed";
        case Kind::Docked: return "docked";
        case Kind::Undocked: return "undocked";
        case Kind::Jumped: return "jumped";
        case Kind::CargoFull: return "cargo_full";
        case Kind::UnderAttack: return "under_attack";
        case Kind::ShipDestroyed: return "ship_destroyed";
    }
    return "notice";
}

}  // namespace Ev
