#include "ui/TextCache.h"

#include <cstring>

namespace Ui
{

namespace
{
uint64_t Hash(Face face, float px, std::string_view text)
{
    // FNV-1a over the size, the face and the letters.
    uint64_t h = 1469598103934665603ull;
    auto     mix = [&h](const void* data, size_t n)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 1099511628211ull;
    };
    uint32_t bits = 0;
    std::memcpy(&bits, &px, sizeof(bits));
    const uint8_t f = (uint8_t)face;
    mix(&bits, sizeof(bits));
    mix(&f, 1);
    mix(text.data(), text.size());
    return h;
}
}  // namespace

Vector2 MeasureCache::Get(Face face, float px, std::string_view text, const Measure& measure)
{
    const uint64_t key = Hash(face, px, text);
    auto           it = entries_.find(key);
    if (it != entries_.end() && it->second.face == face && it->second.px == px &&
        it->second.text == text)
        return it->second.size;
    const Vector2 size = measure(face, px, text);
    if (it != entries_.end())
    {
        it->second = Entry{ face, px, std::string(text), size };  // a collision: the newer wins
        return size;
    }
    if (entries_.size() >= LIMIT)
        entries_.clear();
    entries_.emplace(key, Entry{ face, px, std::string(text), size });
    return size;
}

}  // namespace Ui
