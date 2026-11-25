#include "core/Lock.h"

#include <algorithm>
#include <cmath>

namespace Ships
{
float LockTime(const Rules& rules, float signature, float scanRes)
{
    const float sig = signature > 1.0f ? signature : 1.0f;
    const float res = scanRes > 0.01f ? scanRes : 0.01f;
    const float t = rules.lockBase * std::sqrt(rules.lockRef / sig) / res;
    return std::clamp(t, rules.lockMin, rules.lockMax);
}
}  // namespace Ships
