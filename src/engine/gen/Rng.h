#pragma once

#include <cstdint>

// The generator's randomness (#140). The same seed must make the same galaxy after a
// restart, on another machine and under another compiler -- a structure a player built
// stands somewhere in it -- so nothing here is left to the platform:
//
//   - SplitMix64, written out, rather than a std:: engine or distribution (the
//     distributions are implementation-defined);
//   - a stream is derived from a seed and a *key* (which system, which purpose), never
//     from the order things happen to be generated in -- adding a rule for belts must not
//     move every planet;
//   - doubles are made from integers with operations IEEE 754 rounds exactly (+ - * /),
//     so they are the same everywhere. Anything that picks, picks on integers.
namespace Gen
{

// One SplitMix64 step: a good 64-bit mix of a 64-bit state.
inline uint64_t Mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A key from the seed and up to three integers: "system 7, purpose planets".
inline uint64_t Key(uint64_t seed, uint64_t a, uint64_t b = 0, uint64_t c = 0)
{
    return Mix(Mix(Mix(seed ^ Mix(a)) ^ Mix(b + 0x51ull)) ^ Mix(c + 0xA3ull));
}

class Rng
{
public:
    explicit Rng(uint64_t key) : state_(key) {}

    uint64_t Next()
    {
        state_ += 0x9E3779B97F4A7C15ull;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // An integer in [lo, hi], unbiased: draws that would favour the low end are thrown
    // away rather than folded in with a modulo.
    int Range(int lo, int hi)
    {
        if (hi <= lo)
            return lo;
        const uint64_t span = (uint64_t)((int64_t)hi - (int64_t)lo) + 1u;
        const uint64_t limit = UINT64_MAX - UINT64_MAX % span;
        uint64_t       r;
        do
            r = Next();
        while (r >= limit);
        return (int)((int64_t)lo + (int64_t)(r % span));
    }

    // A double in [0, 1) from the top 53 bits -- exact, so the same on every platform.
    double Unit() { return (double)(Next() >> 11) * (1.0 / 9007199254740992.0); }

    double Between(double lo, double hi) { return lo + (hi - lo) * Unit(); }

    // True with probability `percent` / 100, decided on an integer.
    bool Chance(int percent) { return Range(0, 99) < percent; }

private:
    uint64_t state_;
};

}  // namespace Gen
