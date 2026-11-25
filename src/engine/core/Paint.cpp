#include "core/Paint.h"

#include <algorithm>
#include <cstdlib>

namespace Paint
{
namespace
{
const float BASE_SAT[SATS] = { 0.05f, 0.20f, 0.35f };
const float BASE_LIGHT[LIGHTS] = { 0.30f, 0.45f, 0.60f, 0.75f, 0.88f };
const float BAND_SAT[SATS] = { 0.35f, 0.60f, 0.85f };
const float BAND_LIGHT[LIGHTS] = { 0.25f, 0.40f, 0.55f, 0.70f, 0.85f };
const float WARMTH[LIGHT_MAX + 1] = { 0.0f, 0.3f, 0.65f, 1.0f };

float Hue(int h)
{
    return (float)h / (float)HUES;
}

float Channel(float p, float q, float t)
{
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

unsigned char Byte(float v)
{
    return (unsigned char)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool ReadHex(const std::string& s, size_t at, Rgb& out)
{
    for (int i = 0; i < 3; i++)
    {
        const int hi = HexDigit(s[at + 2 * i]), lo = HexDigit(s[at + 2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (unsigned char)(hi * 16 + lo);
    }
    return true;
}
}  // namespace

float LightnessOf(Role role, Cell c)
{
    return role == Role::Base ? BASE_LIGHT[c.l] : BAND_LIGHT[c.l];
}

Rgb RgbOf(Role role, Cell c)
{
    const float l = LightnessOf(role, c);
    const float s = role == Role::Base ? BASE_SAT[c.s] : BAND_SAT[c.s];
    const float h = Hue(c.h);
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    return { Byte(Channel(p, q, h + 1.0f / 3.0f)), Byte(Channel(p, q, h)),
             Byte(Channel(p, q, h - 1.0f / 3.0f)) };
}

bool Contrasts(Cell base, Cell band)
{
    const float d = LightnessOf(Role::Band, band) - LightnessOf(Role::Base, base);
    return std::abs(d) >= MIN_CONTRAST - 0.0001f;
}

Cell Nearest(Role role, Rgb rgb)
{
    Cell best;
    int  bestD = 1 << 30;
    for (int h = 0; h < HUES; h++)
        for (int l = 0; l < LIGHTS; l++)
            for (int s = 0; s < SATS; s++)
            {
                const Rgb c = RgbOf(role, { h, l, s });
                int       d = 0;
                for (int i = 0; i < 3; i++)
                    d += (c[i] - rgb[i]) * (c[i] - rgb[i]);
                if (d < bestD)
                {
                    bestD = d;
                    best = { h, l, s };
                }
            }
    return best;
}

float Warmth(int light)
{
    return WARMTH[std::clamp(light, 0, LIGHT_MAX)];
}

Paint Snap(Paint p)
{
    auto clampCell = [](Cell c)
    {
        c.h = ((c.h % HUES) + HUES) % HUES;
        c.l = std::clamp(c.l, 0, LIGHTS - 1);
        c.s = std::clamp(c.s, 0, SATS - 1);
        return c;
    };
    p.base = clampCell(p.base);
    p.band = clampCell(p.band);
    p.pattern = std::clamp(p.pattern, PATTERN_MIN, PATTERN_MAX);
    p.light = std::clamp(p.light, 0, LIGHT_MAX);
    if (!Contrasts(p.base, p.band))
    {
        // The nearest lightness step that does.
        Cell best = p.band;
        int  bestGap = 1 << 30;
        for (int l = 0; l < LIGHTS; l++)
        {
            Cell c = p.band;
            c.l = l;
            const int gap = std::abs(l - p.band.l);
            if (Contrasts(p.base, c) && gap < bestGap)
            {
                bestGap = gap;
                best = c;
            }
        }
        p.band = best;
    }
    return p;
}

std::string Format(const std::string& palette, const Paint& p)
{
    static const char* const HEX = "0123456789abcdef";
    std::string              out = palette + "/";
    for (Role role : { Role::Base, Role::Band })
    {
        const Rgb c = RgbOf(role, role == Role::Base ? p.base : p.band);
        for (int i = 0; i < 3; i++)
        {
            out += HEX[c[i] >> 4];
            out += HEX[c[i] & 15];
        }
    }
    out += "p" + std::to_string(p.pattern) + "l" + std::to_string(p.light);
    return out;
}

std::string PaletteOf(const std::string& spec)
{
    return spec.substr(0, spec.find('/'));
}

bool Split(const std::string& spec, std::string& palette, bool& painted, Paint& p)
{
    palette = PaletteOf(spec);
    painted = false;
    if (palette.size() == spec.size())
        return true;
    const size_t at = palette.size() + 1;
    if (spec.size() != at + 16 || spec[at + 12] != 'p' || spec[at + 14] != 'l')
        return false;
    Rgb base, band;
    if (!ReadHex(spec, at, base) || !ReadHex(spec, at + 6, band))
        return false;
    const int pattern = spec[at + 13] - '0', light = spec[at + 15] - '0';
    if (pattern < 0 || pattern > 9 || light < 0 || light > 9)
        return false;
    p.base = Nearest(Role::Base, base);
    p.band = Nearest(Role::Band, band);
    p.pattern = pattern;
    p.light = light;
    painted = true;
    return true;
}
}  // namespace Paint
