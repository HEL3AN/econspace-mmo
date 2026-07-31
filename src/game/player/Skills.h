#pragma once

enum class SkillType { Piloting, Mining, Trading };

// Character skills. They grow with use (flying, mining, trading) and grant a
// bonus multiplier to the corresponding activity.
class Skills
{
public:
    static const int MAX_LEVEL = 10;

    void  AddXp(SkillType skill, float amount);
    int   GetXp(SkillType skill) const;
    void  SetXp(SkillType skill, float amount) { xp_[(int)skill] = amount; }  // for loading
    int   GetLevel(SkillType skill) const;  // 1..MAX_LEVEL
    float GetBonus(SkillType skill) const;  // activity multiplier

private:
    float xp_[3] = { 0.0f, 0.0f, 0.0f };
};
