#pragma once

enum class SkillType
{
    Piloting,
    Mining,
    Trading
};

// Character skills. They grow with use (flying, mining, trading) and grant a
// bonus multiplier to the corresponding activity.
class Skills
{
public:
    // constexpr, not const: a static const int declared in a header has no definition, so
    // binding it to a reference (as a test assertion does) fails at link time. In C++17
    // constexpr static members are implicitly inline, which supplies one.
    static constexpr int MAX_LEVEL = 10;

    void  AddXp(SkillType skill, float amount);
    int   GetXp(SkillType skill) const;
    void  SetXp(SkillType skill, float amount) { xp_[(int)skill] = amount; }  // for loading
    int   GetLevel(SkillType skill) const;                                    // 1..MAX_LEVEL
    float GetBonus(SkillType skill) const;                                    // activity multiplier

private:
    float xp_[3] = { 0.0f, 0.0f, 0.0f };
};
