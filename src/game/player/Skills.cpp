#include "player/Skills.h"

static const float XP_PER_LEVEL = 150.0f;

void Skills::AddXp(SkillType skill, float amount)
{
    xp_[(int)skill] += amount;
}

int Skills::GetXp(SkillType skill) const
{
    return (int)xp_[(int)skill];
}

int Skills::GetLevel(SkillType skill) const
{
    int level = 1 + (int)(xp_[(int)skill] / XP_PER_LEVEL);
    return (level > MAX_LEVEL) ? MAX_LEVEL : level;
}

float Skills::GetBonus(SkillType skill) const
{
    // +4% for each level above the first.
    return 1.0f + 0.04f * (GetLevel(skill) - 1);
}
