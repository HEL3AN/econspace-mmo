// Game -- docked: the station's windows on the desk (#297).
//
// The station used to be one screen that covered everything. Now it is windows like any
// other -- the station itself, its market, its hangar, and the missions window that is open
// in space as well -- each resizable, and grouped or stacked as tabs however the player
// likes. They belong to the "docked" context: they are there while the ship is berthed and
// gone, remembered, when it undocks. What they list comes from StationList and MissionList;
// every button is an order to the server, which answers in the snapshot and the journal.
// Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "core/StationList.h"
#include "entities/Station.h"
#include "player/Skills.h"
#include "ui/Theme.h"

#include "raymath.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
// A sum of credits short enough for a column: 950, 12.5k, 1.2M.
std::string Credits(double v)
{
    char buf[32];
    if (v >= 1000000.0)
        std::snprintf(buf, sizeof(buf), "%.1fM", v / 1000000.0);
    else if (v >= 10000.0)
        std::snprintf(buf, sizeof(buf), "%.1fk", v / 1000.0);
    else
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}
}  // namespace

RepTier Game::DockedTier() const
{
    if (dockedStation_ == nullptr)
        return RepTier::Neutral;
    return Factions::TierOf(player_.GetReputation(dockedStation_->GetFaction()));
}

void Game::SellCargo(ResourceType type, int amount)
{
    if (amount <= 0)
        return;
    // An order to the server; ApplyTradeAcks credits the revenue on its acknowledgement, at
    // the server's price, and a refusal comes back in the journal (#219).
    Proto::Command c;
    c.sellType = (int)type;
    c.sellAmount = amount;
    clientLink_->Send(Proto::EncodeCommand(c));
}

// Where the world would be: the hall the windows stand in. A picture, not a window -- the
// station's name, large and quiet, under whatever the player has open.
void Game::DrawStationHall()
{
    using Ui::Box;
    const Ui::Theme& t = Ui::CurrentTheme();
    DrawRectangle(0, 0, screenWidth_, screenHeight_, t.colors.backdrop);
    if (dockedStation_ == nullptr || hudHidden_)
        return;
    Ui::Layout& L = hallLayout_;
    L.Begin({ 0.0f, 0.0f, (float)screenWidth_, (float)screenHeight_ }, Ui::Scale());
    L.Column(Box()
                 .Grow()
                 .Pad(t.metrics.padding * 1.5f)
                 .Gap(t.metrics.rowGap)
                 .Align(Ui::Align::Center, Ui::Align::End),
             [&]
             {
                 L.Text(dockedStation_->GetName(), Ui::TextStyle::Heading().Tint(t.colors.dim));
                 L.Text(StationRoleName(dockedStation_->GetRole()) + "  ·  " +
                            FactionName(dockedStation_->GetFaction()),
                        Ui::TextStyle::Label());
             });
    L.End();
    L.Draw();
}

// The station's own window: whose it is and what they think of this pilot, the account,
// a bounty to pay if there is one, and the way out.
void Game::DrawStationContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = stationLayout_;

    L.Begin(f);
    L.Column(
        Box().Grow().Gap(t.metrics.gap),
        [&]
        {
            if (dockedStation_ == nullptr)
            {
                L.Text("Not docked", TextStyle::Body().Tint(t.colors.dim));
                return;
            }
            const FactionId sf = dockedStation_->GetFaction();
            const float     rep = player_.GetReputation(sf);
            const RepTier   tier = Factions::TierOf(rep);
            L.Scroll(
                Box().Grow().Id("info").Gap(t.metrics.rowGap),
                [&]
                {
                    L.Text(dockedStation_->GetName(), TextStyle::Title());
                    L.Text(StationRoleName(dockedStation_->GetRole()), TextStyle::Label());
                    // Who runs it, as this pilot sees them (#117).
                    L.Field("Owner", FactionName(sf), StandingColor(sf));
                    L.Field("Standing",
                            TextFormat("%s  %d", Factions::TierName(tier).c_str(), (int)rep),
                            StandingColor(sf));
                    // Docked means attached (#298): the ship is berthed on the station and goes
                    // where it goes. Said, because a station on an orbit moves the whole time
                    // the player is reading its market, and the ship with it.
                    const float carried = Vector2Length(snapshot_.player.vel);
                    L.Field("Berth",
                            carried > 0.5f ? TextFormat("moving with it, %.0f u/s", carried)
                                           : "attached",
                            t.colors.text);
                    L.Divider();
                    L.Field("Money", TextFormat("%.0f cr", player_.GetMoney()), t.colors.money);
                    const Skills& sk = player_.GetSkills();
                    L.Field("Skills",
                            TextFormat(
                                "Pilot %d   Mining %d   Trade %d", sk.GetLevel(SkillType::Piloting),
                                sk.GetLevel(SkillType::Mining), sk.GetLevel(SkillType::Trading)),
                            t.colors.dim);

                    // Wanted here: the bounty is paid to this station's owner. Whether it was
                    // paid, or why not, comes back in the journal (#219), which flashes;
                    // saying "paid" here would be a guess.
                    if (player_.IsWanted(sf))
                    {
                        const double bounty = player_.GetBounty(sf);
                        L.Divider();
                        L.Text(TextFormat("Wanted by %s: a bounty of %.0f cr",
                                          FactionName(sf).c_str(), bounty),
                               TextStyle::Body().Tint(t.colors.bad).Wrap());
                        if (L.Button("bounty", TextFormat("Pay the bounty  %.0f cr", bounty)))
                        {
                            Proto::Command c;
                            c.payBountyFaction = (int)sf;
                            clientLink_->Send(Proto::EncodeCommand(c));
                        }
                    }
                });
            if (L.Button("undock", "Undock", true))
                Undock();
        });
    L.End();
    L.Draw();
}

// The market: what the station pays for each commodity, and how much of it is in the hold;
// the chosen one in full, with how much to sell. Side by side when wide, one above the other
// when narrow, as the missions window is.
void Game::DrawMarketContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = marketLayout_;

    const std::vector<StationList::MarketRow> rows =
        StationList::Market(snapshot_.marketPrices, snapshot_.player.cargoByType,
                            player_.GetSkills().GetBonus(SkillType::Trading), DockedTier());
    marketSel_ = rows.empty() ? -1 : std::clamp(marketSel_, 0, (int)rows.size() - 1);
    // A new row starts at all of it: the usual sale is the whole hold.
    if (marketSel_ >= 0 && sellAmountFor_ != marketSel_)
    {
        sellAmount_ = (float)rows[marketSel_].hold;
        sellAmountFor_ = marketSel_;
    }

    Ui::TableSpec table;
    table.id = "market";
    table.columns = { { "commodity", Size::Grow(), Ui::Align::Start, false },
                      { "price", Size::Fit(48.0f), Ui::Align::End, false },
                      { "hold", Size::Fit(40.0f), Ui::Align::End, false },
                      { "worth", Size::Fit(56.0f), Ui::Align::End, false } };
    table.rows = (int)rows.size();
    table.cell = [&](int r, int c)
    {
        const StationList::MarketRow& row = rows[r];
        const Color                   tone = row.hold > 0 ? t.colors.text : t.colors.dim;
        switch (c)
        {
            case 0: return Ui::Cell{ row.name, tone };
            case 1: return Ui::Cell{ TextFormat("%.1f", row.price), tone };
            case 2: return Ui::Cell{ std::to_string(row.hold), tone };
            default: return Ui::Cell{ row.hold > 0 ? Credits(row.worth) : "-", t.colors.money };
        }
    };
    table.rowFill = [&](int r)
    { return r == marketSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };

    auto details = [&]
    {
        if (marketSel_ < 0)
            return;
        const StationList::MarketRow& m = rows[marketSel_];
        L.Text(m.name, TextStyle::Title());
        L.Field("Pays you", TextFormat("%.1f cr a unit", m.perUnit), t.colors.money);
        L.Field("In the hold", std::to_string(m.hold), t.colors.text);
        if (m.hold <= 0)
        {
            L.Text("None in the hold. Fields of it are on the overview.",
                   TextStyle::Small().Tint(t.colors.dim).Wrap());
            return;
        }
        L.Field("All of it", TextFormat("%.0f cr", m.worth), t.colors.money);
        L.Divider();
        L.Row(Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
              [&]
              {
                  L.Text("Sell", TextStyle::Label());
                  L.NumberField("amount", sellAmount_, 1.0f, (float)m.hold);
              });
        const int amount = std::clamp((int)sellAmount_, 1, m.hold);
        L.Row(Box().GrowX().Gap(t.metrics.rowGap),
              [&]
              {
                  if (L.Button("sell", TextFormat("Sell %d", amount)))
                      SellCargo(m.type, amount);
                  if (L.Button("sellall", "Sell all", true))
                      SellCargo(m.type, m.hold);
              });
        L.Text("A sale lowers the station's price for the next one.",
               TextStyle::Small().Tint(t.colors.dim).Wrap());
    };

    const bool wide = f.Area().width >= Ui::Px(480.0f);
    L.Begin(f);
    L.Column(
        Box().Grow().Gap(t.metrics.gap),
        [&]
        {
            L.Field("Hold",
                    TextFormat("%d / %d", snapshot_.player.cargoUsed, snapshot_.player.cargoCap),
                    t.colors.text);
            auto list = [&]
            {
                const Ui::TableEvents ev = L.Table(table);
                if (ev.clicked >= 0)
                    marketSel_ = ev.clicked;
            };
            auto pane = [&](Box box)
            { L.Scroll(box.Id("details").Gap(t.metrics.rowGap), details); };
            if (wide)
                L.Row(Box().Grow().Gap(t.metrics.padding),
                      [&]
                      {
                          L.Column(Box().Width(Size::Percent(0.55f)).Height(Size::Grow()), list);
                          pane(Box().Grow());
                      });
            else
            {
                L.Column(Box().GrowX().Height(Size::Percent(0.42f)), list);
                L.Divider();
                pane(Box().Grow());
            }
        });
    L.End();
    L.Draw();
}

// The hangar: every hull, which one is flown, which are owned (switching is free), and what
// the rest cost here; the chosen one in full, with the one thing that can be done about it
// -- or why it cannot be, before the server says so.
void Game::DrawHangarContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = hangarLayout_;

    const std::vector<StationList::HangarRow> rows =
        StationList::Hangar(GetShipCatalog(), snapshot_.player.ownedShips, CurrentShipIndex(),
                            player_.GetMoney(), snapshot_.player.cargoUsed, DockedTier());
    if (hangarSel_ < 0)
        hangarSel_ = CurrentShipIndex();
    hangarSel_ = rows.empty() ? -1 : std::clamp(hangarSel_, 0, (int)rows.size() - 1);

    auto state = [&](const StationList::HangarRow& r)
    {
        switch (r.berth)
        {
            case StationList::Berth::Current: return std::string("flying");
            case StationList::Berth::Owned: return std::string("owned");
            case StationList::Berth::ForSale: break;
        }
        return Credits(r.price);
    };

    Ui::TableSpec table;
    table.id = "hangar";
    table.columns = { { "hull", Size::Grow(), Ui::Align::Start, false },
                      { "speed", Size::Fit(40.0f), Ui::Align::End, false },
                      { "hold", Size::Fit(36.0f), Ui::Align::End, false },
                      { "", Size::Fit(56.0f), Ui::Align::End, false } };
    table.rows = (int)rows.size();
    table.cell = [&](int r, int c)
    {
        const StationList::HangarRow& row = rows[r];
        const bool                    flying = row.berth == StationList::Berth::Current;
        switch (c)
        {
            case 0: return Ui::Cell{ row.name, flying ? t.colors.good : t.colors.text };
            case 1: return Ui::Cell{ TextFormat("%.0f", row.speed), t.colors.text };
            case 2: return Ui::Cell{ std::to_string(row.cargo), t.colors.text };
            default:
                return Ui::Cell{ state(row), flying ? t.colors.good
                                             : row.berth == StationList::Berth::Owned
                                                 ? t.colors.text
                                             : row.affordable ? t.colors.money
                                                              : t.colors.dim };
        }
    };
    table.rowFill = [&](int r)
    { return r == hangarSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };

    auto details = [&]
    {
        if (hangarSel_ < 0)
            return;
        const StationList::HangarRow& h = rows[hangarSel_];
        L.Text(h.name, TextStyle::Title());
        L.Field("Top speed", TextFormat("%.0f u/s", h.speed), t.colors.text);
        L.Field("Hold", std::to_string(h.cargo), t.colors.text);
        L.Field("Mining", TextFormat("%.1f a second", h.mining), t.colors.text);
        if (h.berth == StationList::Berth::ForSale)
            L.Field("Price here", TextFormat("%.0f cr", h.price), t.colors.money);
        L.Divider();
        if (h.berth == StationList::Berth::Current)
        {
            L.Text("The ship you are flying.", TextStyle::Small().Tint(t.colors.dim).Wrap());
            return;
        }
        // Said before the server would refuse it: a hold smaller than the cargo (#219), money
        // short of the price.
        if (!h.holdFits)
        {
            L.Text(TextFormat("The hold carries %d; this hull takes %d. Sell some first.",
                              snapshot_.player.cargoUsed, h.cargo),
                   TextStyle::Small().Tint(t.colors.warn).Wrap());
            return;
        }
        if (!h.affordable)
        {
            L.Text(TextFormat("%.0f cr short.", h.price - player_.GetMoney()),
                   TextStyle::Small().Tint(t.colors.warn).Wrap());
            return;
        }
        Proto::Command c;
        if (h.berth == StationList::Berth::Owned)
        {
            if (L.Button("switch", "Switch to it", true))
            {
                c.refitShip = h.index;
                clientLink_->Send(Proto::EncodeCommand(c));
            }
        }
        // The purchase is the server's: it charges, records the ship as owned and refits,
        // and the snapshot brings all three back (#5).
        else if (L.Button("buy", TextFormat("Buy  %.0f cr", h.price), true))
        {
            c.buyShip = h.index;
            clientLink_->Send(Proto::EncodeCommand(c));
        }
    };

    const bool wide = f.Area().width >= Ui::Px(480.0f);
    L.Begin(f);
    L.Column(Box().Grow().Gap(t.metrics.gap),
             [&]
             {
                 auto list = [&]
                 {
                     const Ui::TableEvents ev = L.Table(table);
                     if (ev.clicked >= 0)
                         hangarSel_ = ev.clicked;
                 };
                 auto pane = [&](Box box)
                 { L.Scroll(box.Id("details").Gap(t.metrics.rowGap), details); };
                 if (wide)
                     L.Row(Box().Grow().Gap(t.metrics.padding),
                           [&]
                           {
                               L.Column(Box().Width(Size::Percent(0.55f)).Height(Size::Grow()),
                                        list);
                               pane(Box().Grow());
                           });
                 else
                 {
                     L.Column(Box().GrowX().Height(Size::Percent(0.42f)), list);
                     L.Divider();
                     pane(Box().Grow());
                 }
             });
    L.End();
    L.Draw();
}
