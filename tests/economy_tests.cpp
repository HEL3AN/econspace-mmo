#include <doctest/doctest.h>

#include "economy/Market.h"
#include "economy/Resource.h"
#include "player/Player.h"
#include "player/Skills.h"
#include "factions_named.h"

// Game logic, not the wire. Until now the only covered code was the protocol and the
// transport, so the rules a player actually feels — what selling does to a price, whether
// a bounty ever clears, what a skill is worth — could change without anything noticing.

TEST_CASE("selling moves the price and the market recovers")
{
    Market             m;
    const ResourceType ore = AllResourceTypes()[0];
    const double       base = m.GetBasePrice(ore);
    REQUIRE(base > 0.0);

    SUBCASE("a sale depresses the price")
    {
        const double revenue = m.Sell(ore, 20);
        CHECK(revenue > 0.0);
        CHECK(m.GetPrice(ore) < base);

        // Selling into a market you have already flooded pays less per unit. This is the
        // whole reason price impact exists, so it is worth pinning rather than assuming.
        Market       fresh;
        const double firstBatch = fresh.Sell(ore, 10) / 10.0;
        const double secondBatch = fresh.Sell(ore, 10) / 10.0;
        CHECK(secondBatch < firstBatch);
    }

    SUBCASE("a price stays where trade left it until something moves the stock (#343)")
    {
        m.Sell(ore, 40);
        const double depressed = m.GetPrice(ore);
        m.Step(MarketRates{}, 1.0);  // an hour of a system that makes and uses nothing
        CHECK(m.GetPrice(ore) == doctest::Approx(depressed));  // no trade nobody flies
    }

    SUBCASE("selling nothing changes nothing")
    {
        const double before = m.GetPrice(ore);
        CHECK(m.Sell(ore, 0) == doctest::Approx(0.0));
        CHECK(m.GetPrice(ore) == doctest::Approx(before));
    }
}

TEST_CASE("an account tracks money, standing and what it owes")
{
    Player p(500.0);
    CHECK(p.GetMoney() == doctest::Approx(500.0));
    CHECK(p.CanAfford(500.0));
    CHECK_FALSE(p.CanAfford(500.01));

    p.AddMoney(-200.0);
    CHECK(p.GetMoney() == doctest::Approx(300.0));

    SUBCASE("a bounty makes you wanted until it is paid off")
    {
        CHECK_FALSE(p.IsWanted(Fac::TradersGuild()));
        p.AddBounty(Fac::TradersGuild(), 50.0);
        CHECK(p.IsWanted(Fac::TradersGuild()));
        // Other factions do not care what one of them thinks of you.
        CHECK_FALSE(p.IsWanted(Fac::Pirates()));

        p.SetBounty(Fac::TradersGuild(), 0.0);
        CHECK_FALSE(p.IsWanted(Fac::TradersGuild()));
    }

    SUBCASE("bounty decays but never goes negative")
    {
        p.AddBounty(Fac::TradersGuild(), 5.0);
        for (int i = 0; i < 100; i++)
            p.DecayBounty(1.0);
        CHECK(p.GetBounty(Fac::TradersGuild()) == doctest::Approx(0.0));
        CHECK(p.GetBounty(Fac::TradersGuild()) >= 0.0);
    }

    SUBCASE("reputation is per faction and signed")
    {
        p.AddReputation(Fac::TradersGuild(), 4.0f);
        p.AddReputation(Fac::Pirates(), -6.0f);
        CHECK(p.GetReputation(Fac::TradersGuild()) == doctest::Approx(4.0f));
        CHECK(p.GetReputation(Fac::Pirates()) == doctest::Approx(-6.0f));
    }
}

TEST_CASE("skills level up and pay off")
{
    Skills      s;
    const float base = s.GetBonus(SkillType::Mining);
    CHECK(s.GetLevel(SkillType::Mining) == 1);
    CHECK(base >= 1.0f);  // an untrained pilot is never worse than the baseline

    s.AddXp(SkillType::Mining, 5000.0f);
    CHECK(s.GetLevel(SkillType::Mining) > 1);
    CHECK(s.GetBonus(SkillType::Mining) > base);

    SUBCASE("levels are capped, and the cap holds under absurd input")
    {
        s.AddXp(SkillType::Mining, 1.0e9f);
        CHECK(s.GetLevel(SkillType::Mining) <= Skills::MAX_LEVEL);
    }

    SUBCASE("training one skill does not train another")
    {
        Skills fresh;
        fresh.AddXp(SkillType::Trading, 5000.0f);
        CHECK(fresh.GetLevel(SkillType::Trading) > 1);
        CHECK(fresh.GetLevel(SkillType::Piloting) == 1);
    }
}
