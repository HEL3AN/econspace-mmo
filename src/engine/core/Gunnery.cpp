#include "core/Gunnery.h"

#include <algorithm>
#include <cmath>

namespace Ships
{
float AngularVelocity(float dx, float dy, float dvx, float dvy)
{
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 1.0f)
        return 0.0f;
    return std::fabs(dx * dvy - dy * dvx) / (d * d);
}

float HitQuality(const GunSpec& gun, float distance, float omega, float signature)
{
    if (distance > ReachOf(gun))
        return 0.0f;
    const float over = std::max(0.0f, distance - gun.optimal);
    const float range = std::exp2(-over / gun.falloff);
    const float sig = signature > 1.0f ? signature : 1.0f;
    const float x = (omega / gun.tracking) * (gun.signatureRes / sig);
    return range * std::exp2(-(x * x));
}

Shot Gunnery(const std::vector<Gun>& guns, float distance, float omega, float signature)
{
    Shot  out;
    float whole = 0.0f;
    for (const Gun& g : guns)
    {
        if (g.damage <= 0.0f)
            continue;
        const float dealt = g.damage * HitQuality(g.spec, distance, omega, signature);
        whole += g.damage;
        out.damage += dealt;
        for (size_t t = 0; t < out.mix.size(); ++t)
            out.mix[t] += dealt * g.mix[t];
    }
    if (out.damage > 0.0f)
        for (float& m : out.mix)
            m /= out.damage;  // shares of what got through
    out.quality = whole > 0.0f ? out.damage / whole : 0.0f;
    return out;
}

Profile ProfileOf(const std::vector<Gun>& guns)
{
    Profile p;
    float   whole = 0.0f;
    for (const Gun& g : guns)
    {
        if (g.damage <= 0.0f)
            continue;
        p.optimal += g.damage * g.spec.optimal;
        p.falloff += g.damage * g.spec.falloff;
        p.reach = std::max(p.reach, ReachOf(g.spec));
        whole += g.damage;
    }
    if (whole > 0.0f)
    {
        p.optimal /= whole;
        p.falloff /= whole;
    }
    return p;
}
}  // namespace Ships
