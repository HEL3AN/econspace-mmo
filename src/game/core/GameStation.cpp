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

#include "core/Actions.h"
#include "core/Archetype.h"
#include "core/Fitting.h"
#include "core/StationList.h"
#include "sim/Observation.h"
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
    c.sellType = type.Index();
    c.sellAmount = amount;
    clientLink_->Send(Proto::EncodeCommand(c));
}

void Game::BuyCargo(ResourceType type, int amount)
{
    if (amount <= 0)
        return;
    // The server counts what the stock, the hold and the money allow; ApplyTradeAcks says what
    // it came to, and a refusal comes back in the journal.
    Proto::Command c;
    c.buyType = type.Index();
    c.buyAmount = amount;
    clientLink_->Send(Proto::EncodeCommand(c));
}

const Entity* Game::DockedStructure() const
{
    if (mode_ != GameMode::Docked || dockedStation_ != nullptr)
        return nullptr;
    const Entity* e = FindEntityById(snapshot_.player.dockedStationId);
    return e != nullptr && e->GetKind() == EntityKind::Structure ? e : nullptr;
}

void Game::MoveStorage(ResourceType type, int amount, bool store)
{
    if (amount <= 0)
        return;
    // The server moves what the hold, the storage and the room allow; the snapshot shows it,
    // and a refusal comes back in the journal.
    Proto::Command c;
    (store ? c.storeType : c.takeType) = type.Index();
    (store ? c.storeAmount : c.takeAmount) = amount;
    clientLink_->Send(Proto::EncodeCommand(c));
}

void Game::DrawStorage(Ui::Layout& L)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme&         t = Ui::CurrentTheme();
    const Proto::PlayerView& p = snapshot_.player;
    if (p.storageCap <= 0)
        return;  // none here, or not this pilot's side's
    int used = 0;
    for (int n : p.storedByType)
        used += n;
    L.Divider();
    L.Text("STORAGE", TextStyle::Label());
    L.Field("Kept", TextFormat("%d / %d", used, p.storageCap), t.colors.text);
    L.Field("Hold", TextFormat("%d / %d", p.cargoUsed, p.cargoCap), t.colors.text);
    const std::vector<ResourceType> types = AllResourceTypes();
    bool                            any = false;
    for (size_t i = 0; i < types.size(); i++)
    {
        const int hold = i < p.cargoByType.size() ? p.cargoByType[i] : 0;
        const int kept = i < p.storedByType.size() ? p.storedByType[i] : 0;
        if (hold <= 0 && kept <= 0)
            continue;
        any = true;
        const std::string id = Goods::Id(types[i]);
        L.Row(Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
              [&]
              {
                  L.Text(TextFormat("%s  %d held, %d kept", ResourceName(types[i]).c_str(), hold,
                                    kept),
                         TextStyle::Body());
                  if (hold > 0 && used < p.storageCap &&
                      L.Button("store_" + id, TextFormat("Store %d", hold)))
                      MoveStorage(types[i], hold, true);
                  if (kept > 0 && p.cargoUsed < p.cargoCap &&
                      L.Button("take_" + id, TextFormat("Take %d", kept)))
                      MoveStorage(types[i], kept, false);
              });
    }
    if (!any)
        L.Text("Nothing in the hold or kept here.", TextStyle::Small().Tint(t.colors.dim));
}

// Where the world would be: the hall the windows stand in. A picture, not a window -- the
// station's name, large and quiet, under whatever the player has open.
void Game::DrawStationHall()
{
    using Ui::Box;
    const Ui::Theme& t = Ui::CurrentTheme();
    DrawRectangle(0, 0, screenWidth_, screenHeight_, t.colors.backdrop);
    const Entity* structure = DockedStructure();
    if ((dockedStation_ == nullptr && structure == nullptr) || hudHidden_)
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
                 if (dockedStation_ == nullptr)  // a player's structure (#433)
                 {
                     L.Text(structure->GetName(), Ui::TextStyle::Heading().Tint(t.colors.dim));
                     L.Text(structure->GetOwner(), Ui::TextStyle::Label());
                     return;
                 }
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
                // A player's structure (#433): whose it is, its storage (#438), the way out.
                const Entity* structure = DockedStructure();
                if (structure == nullptr)
                {
                    L.Text("Not docked", TextStyle::Body().Tint(t.colors.dim));
                    return;
                }
                L.Scroll(Box().Grow().Id("info").Gap(t.metrics.rowGap),
                         [&]
                         {
                             L.Text(structure->GetName(), TextStyle::Title());
                             L.Field("Owner", structure->GetOwner(), t.colors.text);
                             L.Field("Money", TextFormat("%.0f cr", player_.GetMoney()),
                                     t.colors.money);
                             DrawStorage(L);
                             DrawLicenceHere(L);
                         });
                if (L.Button("undock", "Undock", true))
                    Undock();
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
                            c.payBountyFaction = Factions::Id(sf);
                            clientLink_->Send(Proto::EncodeCommand(c));
                        }
                    }
                    DrawLicenceHere(L);  // building in this system (#431 slice 6)
                });
            if (L.Button("undock", "Undock", true))
                Undock();
        });
    L.End();
    L.Draw();
}

// The market (#343): every good the system trades, what the market pays and asks for it, how
// many it holds and how much is in the hold; the chosen one in full, with how much to sell and
// to buy. Side by side when wide, one above the other when narrow, as the missions window is.
void Game::DrawMarketContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = marketLayout_;

    StationList::MarketInput in;
    in.prices = snapshot_.marketPrices;
    in.asks = snapshot_.marketAsk;
    in.stock = snapshot_.marketStock;
    in.cargo = snapshot_.player.cargoByType;
    in.tradeBonus = player_.GetSkills().GetBonus(SkillType::Trading);
    in.standing = DockedTier();
    in.money = player_.GetMoney();
    in.holdFree = std::max(0, snapshot_.player.cargoCap - snapshot_.player.cargoUsed);
    in.fee = MarketFeeHere();
    const std::vector<StationList::MarketRow> rows = StationList::Market(in);
    marketSel_ = rows.empty() ? -1 : std::clamp(marketSel_, 0, (int)rows.size() - 1);
    // A new row starts at all of it: the usual sale is the whole hold.
    if (marketSel_ >= 0 && sellAmountFor_ != marketSel_)
    {
        sellAmount_ = (float)rows[marketSel_].hold;
        buyAmount_ = (float)std::min(10, std::max(1, rows[marketSel_].canBuy));
        sellAmountFor_ = marketSel_;
    }
    // ...and a row bought into since it was chosen offers what is now in the hold.
    if (marketSel_ >= 0 && sellAmount_ < 1.0f)
        sellAmount_ = (float)rows[marketSel_].hold;

    Ui::TableSpec table;
    table.id = "market";
    table.columns = { { "good", Size::Grow(), Ui::Align::Start, false },
                      { "bid", Size::Fit(44.0f), Ui::Align::End, false },
                      { "ask", Size::Fit(44.0f), Ui::Align::End, false },
                      { "stock", Size::Fit(40.0f), Ui::Align::End, false },
                      { "hold", Size::Fit(32.0f), Ui::Align::End, false } };
    table.rows = (int)rows.size();
    table.cell = [&](int r, int c)
    {
        const StationList::MarketRow& row = rows[r];
        const Color                   tone = row.hold > 0 ? t.colors.text : t.colors.dim;
        switch (c)
        {
            case 0: return Ui::Cell{ row.name, row.hold > 0 ? t.colors.text : t.colors.dim };
            case 1: return Ui::Cell{ TextFormat("%.1f", row.price), tone };
            case 2: return Ui::Cell{ TextFormat("%.1f", row.ask), t.colors.money };
            case 3:
                return Ui::Cell{ std::to_string(row.stock),
                                 row.stock > 0 ? t.colors.text : t.colors.bad };
            default: return Ui::Cell{ row.hold > 0 ? std::to_string(row.hold) : "-", tone };
        }
    };
    table.rowFill = [&](int r)
    { return r == marketSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };

    static const char* const TIERS[] = { "raw", "refined", "component" };
    auto                     details = [&]
    {
        if (marketSel_ < 0)
            return;
        const StationList::MarketRow& m = rows[marketSel_];
        L.Text(m.name, TextStyle::Title());
        L.Text(TIERS[std::clamp(m.tier, 0, 2)], TextStyle::Label());
        L.Field("Pays you", TextFormat("%.1f cr a unit", m.perUnit), t.colors.money);
        L.Field("Costs you", TextFormat("%.1f cr a unit", m.buyUnit), t.colors.money);
        L.Field("In the hold", std::to_string(m.hold), t.colors.text);
        L.Field("The market holds", std::to_string(m.stock), t.colors.text);

        if (m.hold > 0)
        {
            L.Divider();
            L.Field("All of it", TextFormat("%.0f cr", m.worth), t.colors.money);
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
        }

        L.Divider();
        if (m.canBuy <= 0)
        {
            const char* why = m.stock <= 0 ? "The market has none."
                              : snapshot_.player.cargoUsed >= snapshot_.player.cargoCap
                                  ? "The hold is full."
                                  : "Not enough money for one.";
            L.Text(why, TextStyle::Small().Tint(t.colors.dim).Wrap());
        }
        else
        {
            L.Row(Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("Buy", TextStyle::Label());
                      L.NumberField("buyamount", buyAmount_, 1.0f, (float)m.canBuy);
                  });
            const int amount = std::clamp((int)buyAmount_, 1, m.canBuy);
            L.Row(
                Box().GrowX().Gap(t.metrics.rowGap),
                [&]
                {
                    if (L.Button("buy", TextFormat("Buy %d  ~%.0f cr", amount, m.buyUnit * amount)))
                        BuyCargo(m.type, amount);
                    if (L.Button("buymax", TextFormat("Buy %d", m.canBuy), true))
                        BuyCargo(m.type, m.canBuy);
                });
        }
        L.Text(TextFormat("Every unit traded moves the price of the next. A fee of %.1f%% is "
                          "paid on every trade.",
                          MarketFeeHere() * 100.0),
               TextStyle::Small().Tint(t.colors.dim).Wrap());
    };

    const bool wide = f.Area().width >= Ui::Px(480.0f);
    L.Begin(f);
    L.Column(
        Box().Grow().Gap(t.metrics.gap),
        [&]
        {
            L.Row(Box().GrowX().Gap(t.metrics.padding),
                  [&]
                  {
                      L.Field("Hold",
                              TextFormat("%d / %d", snapshot_.player.cargoUsed,
                                         snapshot_.player.cargoCap),
                              t.colors.text);
                      L.Field("Money", TextFormat("%.0f cr", player_.GetMoney()), t.colors.money);
                  });
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
                          L.Column(Box().Width(Size::Percent(0.58f)).Height(Size::Grow()), list);
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

// A ship was lost (#344): what went with it, what insurance paid and where the pilot woke --
// the snapshot's last loss, in full, until it is put away.
void Game::DrawLossContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme&       t = Ui::CurrentTheme();
    Ui::Layout&            L = lossLayout_;
    const Proto::LossView& loss = snapshot_.player.loss;
    const std::string starter = GetShipCatalog().empty() ? "starter" : GetShipCatalog()[0].name;
    L.Begin(f);
    L.Column(Box().Grow().Gap(t.metrics.rowGap),
             [&]
             {
                 L.Text(TextFormat("Your %s was destroyed", loss.hull.c_str()),
                        TextStyle::Title().Tint(t.colors.warn));
                 L.Field("Where", loss.system, t.colors.text);
                 if (loss.value > 0.0)
                     L.Field("Lost", TextFormat("the hull and its fit, %.0f cr", loss.value),
                             t.colors.text);
                 else
                     L.Field("Lost", "nothing of value: the starter is free", t.colors.dim);
                 if (loss.cargo > 0)
                     L.Field("Cargo lost", TextFormat("%d units", loss.cargo), t.colors.text);
                 if (loss.paid > 0.0)
                     L.Field("Insurance paid", TextFormat("%.0f cr", loss.paid), t.colors.good);
                 else if (loss.value > 0.0)
                     L.Field("Insurance", "none: nothing paid", t.colors.warn);
                 L.Field("You woke", loss.wokeAt.empty() ? std::string("in space") : loss.wokeAt,
                         t.colors.text);
                 L.Divider();
                 L.Text(TextFormat("You fly the %s now: it is free, and always in the hangar. "
                                   "A wreck where you fell holds a share of what was lost. "
                                   "Insure a ship in the hangar before you take it out.",
                                   starter.c_str()),
                        TextStyle::Small().Tint(t.colors.dim).Wrap());
                 if (L.Button("ok", "Understood", true))
                     desk_.SetOpen(WIN_LOSS, false);
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

    Obs::View view;
    view.snapshot = &snapshot_;
    view.layout = &layout_.byId;
    // Whether a hull of this account is insured (#344): the snapshot's word, per owned hull.
    auto insured = [&](int index)
    {
        const Proto::PlayerView& p = snapshot_.player;
        for (size_t k = 0; k < p.ownedShips.size() && k < p.insured.size(); k++)
            if (p.ownedShips[k] == index)
                return p.insured[k] > 0.0;
        return false;
    };
    auto state = [&](const StationList::HangarRow& r)
    {
        switch (r.berth)
        {
            case StationList::Berth::Current:
                return std::string(insured(r.index) ? "flying, ins." : "flying");
            case StationList::Berth::Owned:
                return std::string(insured(r.index) ? "owned, ins." : "owned");
            case StationList::Berth::ForSale: break;
        }
        return Credits(r.price);
    };

    Ui::TableSpec table;
    table.id = "hangar";
    table.columns = { { "hull", Size::Grow(), Ui::Align::Start, false },
                      { "speed", Size::Fit(40.0f), Ui::Align::End, false },
                      { "hold", Size::Fit(36.0f), Ui::Align::End, false },
                      { "", Size::Fit(76.0f), Ui::Align::End, false } };
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
        // Read off the hull's design (#279 step 5), as the server flies it.
        L.Field("Top speed", TextFormat("%.0f u/s", h.speed), t.colors.text);
        L.Field("Turn", TextFormat("%.1f rad/s", h.turn), t.colors.text);
        L.Field("Hold", std::to_string(h.cargo), t.colors.text);
        L.Field("Mining", TextFormat("%.1f a second", h.mining), t.colors.text);
        L.Field("Hull", TextFormat("%.0f", h.hull), t.colors.text);
        L.Field("Guns", TextFormat("%.0f a shot", h.damage), t.colors.text);
        if (h.berth == StationList::Berth::ForSale)
            L.Field("Price here", TextFormat("%.0f cr", h.price), t.colors.money);
        L.Divider();
        // What losing it would cost, and the policy against it (#344): the server's sums
        // (Obs::QuoteInsurance), refused here for the reasons the server would give.
        if (h.berth != StationList::Berth::ForSale)
        {
            const Obs::InsuranceQuote q = Obs::QuoteInsurance(view, h.index);
            L.Field("Worth here", TextFormat("%.0f cr", q.value), t.colors.text);
            if (q.cover > 0.0)
                L.Field("Insured",
                        TextFormat("%.0f cr, pays %.0f if lost", q.cover,
                                   InsurancePayout(q.cover, q.value)),
                        t.colors.good);
            else
                L.Field("Insured", q.value > 0.0 ? "no" : "free to replace",
                        q.value > 0.0 ? t.colors.warn : t.colors.dim);
            if (q.problem.empty())
            {
                if (L.Button("insure", TextFormat("Insure  %.0f cr", q.premium), true))
                {
                    Proto::Command ic;
                    ic.insureShip = h.index;
                    clientLink_->Send(Proto::EncodeCommand(ic));
                }
                L.Text(TextFormat("Pays %.0f cr if it is lost. A lost ship is gone with its fit "
                                  "and cargo.",
                                  q.payout),
                       TextStyle::Small().Tint(t.colors.dim).Wrap());
            }
            L.Divider();
        }
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
                 L.Tabs("tabs", { "Ships", "Fit" }, hangarTab_);
                 if (hangarTab_ == 1)
                 {
                     DrawFitTab(L, wide);
                     return;
                 }
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

// The Fit tab (#279): the ship being flown, socket by socket. A fit is drawn up here and
// nothing is sent until Apply; the numbers beside it are Obs::QuoteFit's, the server's own
// rules and price, so what the tab says is what the server does.
void Game::DrawFitTab(Ui::Layout& L, bool wide)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();

    Obs::View view;
    view.snapshot = &snapshot_;
    view.layout = &layout_.byId;
    const Ships::Design* hull = Obs::CurrentHull(view);
    const Ships::Design* now = Obs::CurrentDesign(view);
    if (hull == nullptr || now == nullptr)
    {
        L.Text("No ship yet.", TextStyle::Small().Tint(t.colors.dim));
        return;
    }
    if (fitDraftFor_ != now->id)
    {
        fitDraft_ = Ships::PlayerLines(*now);  // a player's fit sets no scale
        fitDraftFor_ = now->id;
    }

    // The design library (#279 step 3): the saved designs, the one chosen, and what it is.
    const std::vector<Obs::SavedDesignRow> saved = Obs::SavedDesigns(view);
    const Obs::SavedDesignRow*             chosen = nullptr;
    for (const Obs::SavedDesignRow& r : saved)
        if (r.name == designSel_)
            chosen = &r;
    if (chosen == nullptr)
        designSel_.clear();

    const Ships::Catalogue&               ships = Archetypes::ShipCatalogue();
    const std::vector<Ships::SocketGroup> groups = Ships::Sockets(ships, *hull, fitDraft_);
    fitSel_ = groups.empty() ? 0 : std::clamp(fitSel_, 0, (int)groups.size() - 1);
    const Obs::FitQuote q = Obs::QuoteFit(view, fitDraft_);

    Ui::TableSpec table;
    table.id = "sockets";
    table.columns = { { "socket", Size::Grow(), Ui::Align::Start, false },
                      { "used", Size::Fit(44.0f), Ui::Align::End, false } };
    table.rows = (int)groups.size();
    table.cell = [&](int r, int c)
    {
        const Ships::SocketGroup& g = groups[r];
        if (c == 0)
            return Ui::Cell{ g.in + " " + g.on, t.colors.text };
        return Ui::Cell{ TextFormat("%d/%d", g.used, g.capacity), g.used > g.capacity ? t.colors.bad
                                                                  : g.used == 0 ? t.colors.dim
                                                                                : t.colors.text };
    };
    table.rowFill = [&](int r) { return r == fitSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };

    // Changes how many of `module` the draft puts on the chosen socket.
    auto change = [&](const Ships::SocketGroup& g, const std::string& module, int by)
    {
        for (size_t k = 0; k < fitDraft_.size(); k++)
            if (fitDraft_[k].module == module && fitDraft_[k].in == g.in && fitDraft_[k].on == g.on)
            {
                fitDraft_[k].count += by;
                if (fitDraft_[k].count <= 0)
                    fitDraft_.erase(fitDraft_.begin() + (long)k);
                return;
            }
        if (by > 0)
        {
            Ships::FitLine l;
            l.module = module;
            l.in = g.in;
            l.on = g.on;
            l.count = by;
            fitDraft_ = Ships::Normalized(fitDraft_);
            fitDraft_.push_back(l);
            fitDraft_ = Ships::Normalized(fitDraft_);
        }
    };

    auto details = [&]
    {
        L.Text(q.hull + (now->id == hull->id ? "" : "  (refitted)"), TextStyle::Title());
        if (!groups.empty())
        {
            const Ships::SocketGroup& g = groups[fitSel_];
            // What is mirrored comes in pairs (Ships::Validate), so it moves two at a time.
            const int step = g.on == "side" || g.on == "edge" ? 2 : 1;
            L.Text(TextFormat("%s %s: %d of %d", g.in.c_str(), g.on.c_str(), g.used, g.capacity),
                   TextStyle::Small().Tint(t.colors.dim));
            for (size_t k = 0; k < g.modules.size(); k++)
            {
                const std::string& m = g.modules[k];
                int                n = 0;
                for (const Ships::FitLine& l : fitDraft_)
                    if (l.module == m && l.in == g.in && l.on == g.on)
                        n = l.count;
                L.Row(Box().GrowX().Gap(t.metrics.gap),
                      [&]
                      {
                          // The name in a column of its own, so the buttons line up row to row.
                          L.Row(Box().Width(Size::Percent(0.45f)),
                                [&]
                                {
                                    L.Text(m, TextStyle::Body().Tint(n > 0 ? t.colors.text
                                                                           : t.colors.dim));
                                });
                          if (L.Button("dec" + std::to_string(k), "-") && n > 0)
                              change(g, m, -std::min(step, n));
                          L.Text(std::to_string(n), TextStyle::Body().Tint(t.colors.text));
                          if (L.Button("inc" + std::to_string(k), "+"))
                              change(g, m, step);
                      });
            }
        }
        L.Divider();
        // Before and after, as the server would fly it.
        auto line = [&](const char* label, float a, float b, const char* fmt)
        {
            const std::string text = std::string(TextFormat(fmt, a)) + "  >  " + TextFormat(fmt, b);
            L.Field(label, text, b > a ? t.colors.good : b < a ? t.colors.warn : t.colors.text);
        };
        line("Top speed", q.before.maxSpeed, q.after.maxSpeed, "%.0f");
        line("Turn", q.before.turnSpeed, q.after.turnSpeed, "%.2f");
        line("Hold", (float)q.before.cargoCapacity, (float)q.after.cargoCapacity, "%.0f");
        line("Mining", q.before.miningRate, q.after.miningRate, "%.1f");
        line("Hull", q.before.hull, q.after.hull, "%.0f");
        line("Guns", q.before.damage, q.after.damage, "%.0f");
        if (q.changed && !q.design.id.empty())
            L.Field(q.price >= 0.0 ? "Price here" : "Refund", Credits(std::abs(q.price)) + " cr",
                    t.colors.money);
        // The verdict as the fit is edited, the server's own (Obs::QuoteFit): whether the fit
        // is one a ship may fly, and whether this yard would take it now -- but not the
        // "nothing has changed" of a tab just opened.
        const bool drafted = q.changed || q.design.id.empty();
        if (drafted && q.ok)
            L.Text("The yard would take this fit.", TextStyle::Small().Tint(t.colors.good).Wrap());
        else if (drafted)
            L.Text(
                q.problem,
                TextStyle::Small().Tint(q.design.id.empty() ? t.colors.bad : t.colors.warn).Wrap());
        // A saved design drawn up here is applied as that design; any other fit as a refit.
        // Apply is there once something has changed, and lit only when the server would take
        // it: a fit that would be refused says why above instead of being sent.
        const bool draftIsChosen = chosen != nullptr && q.design.id == chosen->id;
        L.Row(Box().GrowX().Gap(t.metrics.gap),
              [&]
              {
                  if (!draftIsChosen && drafted)
                  {
                      std::string label = "Apply";
                      if (!q.design.id.empty())
                          label += q.price >= 0.0 ? "  " + Credits(q.price) + " cr"
                                                  : "  refund " + Credits(-q.price) + " cr";
                      const std::vector<Actions::Action> apply =
                          Actions::ForRefit(fitDraft_, q.ok, q.changed);
                      if (L.Button("apply", label, !apply.empty(), !apply.empty()))
                          Perform(apply.front(), 0, { 0.0f, 0.0f });
                  }
                  if (L.Button("reset", "Reset"))
                      fitDraftFor_.clear();
              });

        // The library: a name, and what can be done with it and with the design chosen.
        L.Divider();
        L.Text(TextFormat("Saved designs  %d of %d", (int)saved.size(), Ships::LIBRARY_MAX),
               TextStyle::Small().Tint(t.colors.dim));
        if (chosen != nullptr)
        {
            std::string about;
            if (!chosen->known)
                about = "Not known here yet.";
            else if (chosen->flown)
                about = "The ship you fly is this design.";
            else if (!chosen->thisHull)
                about = "For the " + chosen->hull + ": switch to one to apply it.";
            else if (!chosen->quote.ok)
                about = chosen->quote.problem;
            if (!about.empty())
                L.Text(about,
                       TextStyle::Small()
                           .Tint(chosen->thisHull && !chosen->flown ? t.colors.warn : t.colors.dim)
                           .Wrap());
        }
        Ui::TextFieldOptions opt;
        opt.placeholder = "a name for this fit";
        opt.maxLength = Ships::DESIGN_NAME_MAX;
        L.TextField("design_name", designName_, opt);
        Actions::LibraryTarget lib;
        std::string            why;
        lib.typed = designName_;
        lib.typedOk = Ships::ValidDesignName(designName_, why);
        lib.fit = fitDraft_;
        lib.room = (int)saved.size() < Ships::LIBRARY_MAX;
        for (const Obs::SavedDesignRow& r : saved)
            lib.typedTaken = lib.typedTaken || r.name == designName_;
        if (chosen != nullptr)
        {
            lib.selected = chosen->name;
            lib.canApply = chosen->thisHull && chosen->quote.ok;
        }
        L.Row(Box().GrowX().Gap(t.metrics.gap),
              [&]
              {
                  for (const Actions::Action& a : Actions::ForLibrary(lib))
                  {
                      std::string label = a.label;
                      if (a.verb == Actions::Verb::ApplyDesign)  // the price, before the click
                          label += chosen->quote.price >= 0.0
                                       ? "  " + Credits(chosen->quote.price) + " cr"
                                       : "  refund " + Credits(-chosen->quote.price) + " cr";
                      if (a.verb == Actions::Verb::SaveDesign && lib.typedTaken)
                          label = "Save over";
                      if (L.Button("lib" + std::to_string((int)a.verb), label,
                                   a.verb == Actions::Verb::ApplyDesign))
                      {
                          Perform(a, 0, { 0.0f, 0.0f });
                          if (a.verb == Actions::Verb::DeleteDesign)
                              designSel_.clear();
                          if (a.verb == Actions::Verb::RenameDesign)
                              designSel_ = a.renameTo;
                      }
                  }
              });
        if (!designName_.empty() && !lib.typedOk)
            L.Text(why, TextStyle::Small().Tint(t.colors.warn).Wrap());
        else if (lib.typedOk && !lib.room && !lib.typedTaken)
            L.Text("The library is full: delete a design to save another.",
                   TextStyle::Small().Tint(t.colors.warn).Wrap());
    };

    // The saved designs, each with the hull it is for and how fast it would fly.
    Ui::TableSpec library;
    library.id = "designs";
    library.columns = { { "design", Size::Grow(), Ui::Align::Start, false },
                        { "hull", Size::Fit(48.0f), Ui::Align::Start, false },
                        { "speed", Size::Fit(36.0f), Ui::Align::End, false } };
    library.rows = (int)saved.size();
    library.cell = [&](int r, int c)
    {
        const Obs::SavedDesignRow& row = saved[r];
        const Color ink = row.flown ? t.colors.good : row.thisHull ? t.colors.text : t.colors.dim;
        switch (c)
        {
            case 0: return Ui::Cell{ row.name, ink };
            case 1: return Ui::Cell{ row.known ? row.hull : std::string("?"), ink };
            default:
                return Ui::Cell{ row.known ? TextFormat("%.0f", row.stats.maxSpeed) : "", ink };
        }
    };
    library.rowFill = [&](int r)
    { return saved[r].name == designSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };

    auto list = [&]
    {
        const Ui::TableEvents ev = L.Table(table);
        if (ev.clicked >= 0)
            fitSel_ = ev.clicked;
    };
    auto designs = [&]
    {
        const Ui::TableEvents ev = L.Table(library);
        if (ev.clicked < 0)
            return;
        // Chosen: its name is in the field to rename from, and a design of this hull is drawn
        // up in the tab, so its price and numbers are the ones beside it.
        const Obs::SavedDesignRow& r = saved[ev.clicked];
        designSel_ = r.name;
        designName_ = r.name;
        if (r.thisHull)
            if (const Ships::Design* d = Archetypes::FindDesign(r.id))
                fitDraft_ = Ships::PlayerLines(*d);
    };
    auto pane = [&](Box box) { L.Scroll(box.Id("fit").Gap(t.metrics.rowGap), details); };
    if (wide)
        L.Row(Box().Grow().Gap(t.metrics.padding),
              [&]
              {
                  L.Column(Box().Width(Size::Percent(0.4f)).Height(Size::Grow()).Gap(t.metrics.gap),
                           [&]
                           {
                               L.Column(Box().GrowX().Height(Size::Percent(0.55f)), list);
                               L.Column(Box().Grow(), designs);
                           });
                  pane(Box().Grow());
              });
    else
    {
        L.Row(Box().GrowX().Height(Size::Percent(0.35f)).Gap(t.metrics.padding),
              [&]
              {
                  L.Column(Box().Width(Size::Percent(0.5f)).Height(Size::Grow()), list);
                  L.Column(Box().Grow(), designs);
              });
        L.Divider();
        pane(Box().Grow());
    }
}
