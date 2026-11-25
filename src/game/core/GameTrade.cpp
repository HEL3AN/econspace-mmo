// Game -- the Trade window (#473): a direct trade with another pilot docked at the same
// station. Presentation only: what each side has put in, who has confirmed and what a chart is
// worth to the reader are the server's description (Proto::TradeView), and a confirm carries
// the revision it was shown, so a change made after it was looked at cannot be confirmed.
// Goods go in by dragging a row out of the inventory onto the window, or by the buttons.
// Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "sim/InventoryList.h"
#include "ui/Drag.h"
#include "ui/Theme.h"

#include <algorithm>
#include <string>

namespace
{
// How long a confirm waits after the offer last moved, so a button under the cursor is not
// pressed on what was there a moment ago.
constexpr double CONFIRM_DELAY = 1.2;
}  // namespace

void Game::SendDeal(const std::string& action, const std::string& with,
                    const Exchange::Bundle& bundle, int rev)
{
    Proto::Act::Deal d;
    d.action = action;
    d.with = with;
    d.bundle = bundle;
    d.rev = rev;
    SendAction(d);
}

void Game::DrawTradeContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme&        t = Ui::CurrentTheme();
    Ui::Layout&             L = tradeLayout_;
    const Proto::TradeView& trade = snapshot_.player.trade;

    if (trade.rev != tradeRevSeen_ || trade.with != tradeWithSeen_)
    {
        tradeRevSeen_ = trade.rev;
        tradeWithSeen_ = trade.with;
        tradeChangedAt_ = GetTime();
    }

    L.Begin(f);
    // Goods dragged out of the inventory onto the window are put in.
    if (const Ui::DragPayload* d = L.Dropped("tradedrop", 0, "goods"))
        if (!trade.with.empty() && d->item >= 0 && d->item < Goods::Count() && d->amount > 0)
        {
            Exchange::Bundle b;
            b.goods.push_back({ d->item, d->amount, d->source == "hold" ? "" : d->source });
            SendDeal("put", "", b);
        }
    const bool lit = L.DropHover("tradedrop", 0, "goods");
    L.Column(
        Box()
            .Grow()
            .Gap(t.metrics.rowGap)
            .Id("tradedrop")
            .Border(lit ? t.colors.accent : Color{}, lit ? 1.0f : 0.0f),
        [&]
        {
            if (mode_ != GameMode::Docked)
            {
                L.Text("Dock at a station to trade with a pilot docked beside you.",
                       TextStyle::Body().Tint(t.colors.dim).Wrap());
                return;
            }
            for (const std::string& who : trade.invites)
                L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                      [&]
                      {
                          L.Text(who + " asks to trade", TextStyle::Strong());
                          L.Spacer();
                          if (L.Button("accept_" + who, "Accept", true))
                              SendDeal("open", who);
                      });
            if (trade.with.empty())
            {
                if (!trade.asking.empty())
                    L.Text("Waiting for " + trade.asking + " to accept...",
                           TextStyle::Body().Tint(t.colors.dim));
                Ui::TextFieldOptions opt;
                opt.placeholder = "pilot's name";
                L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                      [&]
                      {
                          const Ui::EditResult r = L.TextField("tradewith", tradeWith_, opt);
                          const bool           go =
                              L.Button("ask", "Ask to trade", true, !tradeWith_.empty());
                          if ((go || r.submitted) && !tradeWith_.empty())
                              SendDeal("open", tradeWith_);
                      });
                L.Text("Both pilots ask for each other; nothing moves until both confirm.",
                       TextStyle::Small().Tint(t.colors.dim).Wrap());
                return;
            }

            const std::vector<ResourceType> types = AllResourceTypes();
            auto side = [&](const Proto::TradeSideView& v, const std::string& title, bool mine)
            {
                L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                      [&]
                      {
                          L.Text(title, TextStyle::Label());
                          L.Spacer();
                          L.Text(
                              v.confirmed ? "CONFIRMED" : "not confirmed",
                              TextStyle::Small().Tint(v.confirmed ? t.colors.good : t.colors.dim));
                      });
                if (v.goods.empty() && v.credits <= 0 && v.charts.empty())
                    L.Text("nothing", TextStyle::Small().Tint(t.colors.dim));
                for (size_t i = 0; i < v.goods.size(); ++i)
                {
                    const Exchange::GoodLine& g = v.goods[i];
                    const std::string         name = g.type >= 0 && (size_t)g.type < types.size()
                                                         ? ResourceName(types[(size_t)g.type])
                                                         : std::string("?");
                    const std::string         from = g.from.empty() || g.from == "hold"
                                                         ? std::string()
                                                         : " (from " + g.from + ")";
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            L.Text(std::to_string(g.amount) + " " + name + from, TextStyle::Body());
                            L.Spacer();
                            if (mine && L.Button("wd" + std::to_string(i), "Take back"))
                            {
                                Exchange::Bundle b;
                                b.goods.push_back(g);
                                SendDeal("withdraw", "", b);
                            }
                        });
                }
                if (v.credits > 0)
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            L.Text(std::to_string(v.credits) + " cr", TextStyle::Body());
                            L.Spacer();
                            if (mine && L.Button("wdcr", "Take back"))
                            {
                                Exchange::Bundle b;
                                b.credits = v.credits;
                                SendDeal("withdraw", "", b);
                            }
                        });
                for (size_t i = 0; i < v.charts.size(); ++i)
                {
                    const Exchange::ChartLine& c = v.charts[i];
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            L.Text("Chart: " + c.text, TextStyle::Small().Wrap());
                            L.Spacer();
                            if (mine && L.Button("wdch" + std::to_string(i), "Take back"))
                            {
                                Exchange::Bundle b;
                                b.charts.push_back(c.system);
                                SendDeal("withdraw", "", b);
                            }
                        });
                }
            };
            L.Text("Trading with " + trade.with, TextStyle::Strong());
            side(trade.mine, "YOU OFFER", true);
            L.Divider();
            side(trade.theirs, trade.with + " OFFERS", false);
            L.Divider();

            // What goes in: a row of the hold, credits, a chart by name.
            const std::vector<InventoryList::Container> all =
                InventoryList::Reachable(snapshot_.player);
            const InventoryList::Container*       hold = InventoryList::Find(all, "hold");
            const std::vector<InventoryList::Row> rows = hold != nullptr
                                                             ? InventoryList::Rows(*hold, {})
                                                             : std::vector<InventoryList::Row>();
            if (!rows.empty())
            {
                tradeGood_ = std::clamp(tradeGood_, 0, (int)rows.size() - 1);
                const InventoryList::Row& row = rows[(size_t)tradeGood_];
                L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                      [&]
                      {
                          if (L.Button("tgprev", "<"))
                              tradeGood_ = (tradeGood_ + (int)rows.size() - 1) % (int)rows.size();
                          L.Text(row.name + " (" + std::to_string(row.amount) + ")",
                                 TextStyle::Body());
                          if (L.Button("tgnext", ">"))
                              tradeGood_ = (tradeGood_ + 1) % (int)rows.size();
                          L.NumberField("tgamount", tradeAmount_, 1.0f,
                                        (float)std::max(1, row.amount));
                          if (L.Button("tgput", "Put in"))
                          {
                              Exchange::Bundle b;
                              b.goods.push_back(
                                  { row.type.Index(), std::max(1, (int)tradeAmount_), "" });
                              SendDeal("put", "", b);
                          }
                      });
            }
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("Credits", TextStyle::Label());
                      L.NumberField("tcredits", tradeCredits_, 0.0f,
                                    (float)std::max(0.0, snapshot_.player.money));
                      if (L.Button("tcput", "Put in", false, tradeCredits_ >= 1.0f))
                      {
                          Exchange::Bundle b;
                          b.credits = (long long)tradeCredits_;
                          SendDeal("put", "", b);
                      }
                  });
            Ui::TextFieldOptions copt;
            copt.placeholder = "a system you charted";
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.TextField("tchart", tradeChart_, copt);
                      if (L.Button("tchput", "Put chart in", false, !tradeChart_.empty()))
                      {
                          Exchange::Bundle b;
                          b.charts.push_back(tradeChart_);
                          SendDeal("put", "", b);
                          tradeChart_.clear();
                      }
                  });
            L.Text(std::string("The market's fee on the trade, split: ") +
                       TextFormat("%.0f cr each", trade.fee),
                   TextStyle::Small().Tint(t.colors.dim));
            if (!trade.problem.empty())
                L.Text(trade.problem, TextStyle::Small().Tint(t.colors.bad).Wrap());

            const bool ready = GetTime() - tradeChangedAt_ >= CONFIRM_DELAY;
            L.Row(Box().GrowX().Gap(t.metrics.gap),
                  [&]
                  {
                      if (L.Button("tconfirm", trade.mine.confirmed ? "Confirmed" : "Confirm", true,
                                   ready && !trade.mine.confirmed))
                          SendDeal("confirm", "", {}, trade.rev);
                      if (L.Button("tcancel", "Cancel"))
                          SendDeal("cancel");
                  });
        });
    L.End();
    L.Draw();
}
