// Game -- the Contracts window (#473): the exchange contracts a pilot may take here or has
// posted, and a form to post one. Presentation only: what a contract gives and asks, what taking
// it costs and whether this pilot may are the server's description (Proto::DealView), a chart
// is told as what is new to the reader, and a price far from the market's is marked. Posting
// and taking are orders to the server (Proto::Act::Board).
// Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "sim/InventoryList.h"
#include "ui/Theme.h"

#include <algorithm>
#include <string>

namespace
{
std::string Said(const std::vector<Exchange::GoodLine>& goods, long long credits,
                 const std::vector<std::string>& charts)
{
    const std::vector<ResourceType> types = AllResourceTypes();
    std::string                     out;
    auto add = [&](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    for (const Exchange::GoodLine& g : goods)
        add(std::to_string(g.amount) + " " +
            (g.type >= 0 && (size_t)g.type < types.size() ? ResourceName(types[(size_t)g.type])
                                                          : std::string("?")));
    if (credits > 0)
        add(std::to_string(credits) + " cr");
    for (const std::string& c : charts)
        add("a chart (" + c + ")");
    return out.empty() ? std::string("nothing") : out;
}
}  // namespace

void Game::SendBoard(const Proto::Act::Board& b)
{
    SendAction(b);
}

void Game::DrawBoardContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme&                    t = Ui::CurrentTheme();
    Ui::Layout&                         L = boardLayout_;
    const std::vector<Proto::DealView>& board = snapshot_.player.board;
    const std::vector<ResourceType>     types = AllResourceTypes();

    L.Begin(f);
    L.Column(
        Box().Grow().Gap(t.metrics.rowGap),
        [&]
        {
            if (mode_ != GameMode::Docked)
            {
                L.Text("Dock at a station to read its contracts.",
                       TextStyle::Body().Tint(t.colors.dim).Wrap());
                return;
            }
            if (board.empty())
                L.Text("No contracts here or yours.", TextStyle::Body().Tint(t.colors.dim));
            else
            {
                boardSel_ = std::clamp(boardSel_, 0, (int)board.size() - 1);
                Ui::TableSpec table;
                table.id = "board";
                table.columns = { { "by", Size::Fit(90.0f), Ui::Align::Start, false },
                                  { "gives", Size::Grow(), Ui::Align::Start, false },
                                  { "asks", Size::Grow(), Ui::Align::Start, false } };
                table.rows = (int)board.size();
                table.cell = [&](int r, int c)
                {
                    const Proto::DealView& d = board[(size_t)r];
                    const Color            tone = d.mine        ? t.colors.accent
                                                  : d.canAccept ? t.colors.good
                                                                : t.colors.dim;
                    if (c == 0)
                        return Ui::Cell{ d.issuer, tone };
                    if (c == 1)
                        return Ui::Cell{ Said(d.give, d.giveCredits, {}) +
                                             (d.giveCharts.empty() ? "" : " + chart"),
                                         t.colors.text };
                    return Ui::Cell{ Said(d.ask, d.askCredits, {}), t.colors.text };
                };
                table.rowFill = [&](int r)
                { return r == boardSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };
                const Ui::TableEvents ev = L.Table(table);
                if (ev.clicked >= 0)
                    boardSel_ = ev.clicked;
                const Proto::DealView& d = board[(size_t)boardSel_];
                L.Text("#" + std::to_string(d.id) + " at " + d.place +
                           (d.to.empty() ? "" : ", offered to " + d.to),
                       TextStyle::Strong().Wrap());
                L.Field("Gives", Said(d.give, d.giveCredits, d.giveCharts), t.colors.text);
                L.Field("Asks", Said(d.ask, d.askCredits, {}), t.colors.text);
                const double left = (d.deadline - snapshot_.time) / 86400.0;
                L.Field("Time left", TextFormat("%.1f days", std::max(0.0, left)), t.colors.dim);
                if (!d.mine)
                    L.Field("Market fee", TextFormat("%.0f cr, paid by whoever takes it", d.fee),
                            t.colors.dim);
                if (!d.note.empty())
                    L.Text("Check the price: it " + d.note,
                           TextStyle::Small().Tint(t.colors.bad).Wrap());
                if (!d.why.empty() && !d.mine)
                    L.Text(d.why, TextStyle::Small().Tint(t.colors.dim).Wrap());
                L.Row(Box().GrowX().Gap(t.metrics.gap),
                      [&]
                      {
                          if (!d.mine && L.Button("take", "Take it", true, d.canAccept))
                          {
                              Proto::Act::Board b;
                              b.action = "accept";
                              b.id = d.id;
                              SendBoard(b);
                          }
                          if (!d.mine &&
                              L.Button("takecorp", "Take for corporation", false, d.here))
                          {
                              Proto::Act::Board b;
                              b.action = "accept";
                              b.id = d.id;
                              b.corp = true;
                              SendBoard(b);
                          }
                          if (d.mine && L.Button("cancel", "Take back"))
                          {
                              Proto::Act::Board b;
                              b.action = "cancel";
                              b.id = d.id;
                              SendBoard(b);
                          }
                      });
            }
            L.Divider();
            L.Text("POST A CONTRACT", TextStyle::Label());
            // What it gives: a good of the hold, credits.
            const std::vector<InventoryList::Container> all =
                InventoryList::Reachable(snapshot_.player);
            const InventoryList::Container* hold =
                InventoryList::Find(all, boardCorp_ ? "storage" : "hold");
            const std::vector<InventoryList::Row> rows = hold != nullptr
                                                             ? InventoryList::Rows(*hold, {})
                                                             : std::vector<InventoryList::Row>();
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("Give", TextStyle::Label());
                      if (rows.empty())
                          L.Text("(an empty hold)", TextStyle::Small().Tint(t.colors.dim));
                      else
                      {
                          giveGood_ = std::clamp(giveGood_, 0, (int)rows.size() - 1);
                          if (L.Button("ggprev", "<"))
                              giveGood_ = (giveGood_ + (int)rows.size() - 1) % (int)rows.size();
                          L.Text(rows[(size_t)giveGood_].name + " (" +
                                     std::to_string(rows[(size_t)giveGood_].amount) + ")",
                                 TextStyle::Body());
                          if (L.Button("ggnext", ">"))
                              giveGood_ = (giveGood_ + 1) % (int)rows.size();
                          L.NumberField("gamount", giveAmount_, 0.0f,
                                        (float)rows[(size_t)giveGood_].amount);
                      }
                  });
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("   and credits", TextStyle::Small());
                      L.NumberField("gcredits", giveCredits_, 0.0f,
                                    (float)std::max(0.0, snapshot_.player.money));
                  });
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("Ask", TextStyle::Label());
                      askGood_ = std::clamp(askGood_, 0, std::max(0, (int)types.size() - 1));
                      if (L.Button("agprev", "<"))
                          askGood_ = (askGood_ + (int)types.size() - 1) % (int)types.size();
                      L.Text(types.empty() ? "" : ResourceName(types[(size_t)askGood_]),
                             TextStyle::Body());
                      if (L.Button("agnext", ">"))
                          askGood_ = (askGood_ + 1) % std::max(1, (int)types.size());
                      L.NumberField("aamount", askAmount_, 0.0f, 100000.0f);
                  });
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("   and credits", TextStyle::Small());
                      L.NumberField("acredits", askCredits_, 0.0f, 1.0e9f);
                  });
            Ui::TextFieldOptions opt;
            opt.placeholder = "to anyone, or a pilot's name, or corp:TICKER";
            L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.TextField("boardto", boardTo_, opt);
                      L.Text("days", TextStyle::Small());
                      L.NumberField("boarddays", boardDays_, 1.0f, 14.0f);
                  });
            L.Row(Box().GrowX().Gap(t.metrics.gap),
                  [&]
                  {
                      if (L.Button("boardcorp",
                                   boardCorp_ ? "As corporation: on" : "As corporation: off"))
                          boardCorp_ = !boardCorp_;
                      const bool anything =
                          (!rows.empty() && giveAmount_ >= 1.0f) || giveCredits_ >= 1.0f;
                      if (L.Button("post", "Post", true, anything))
                      {
                          Proto::Act::Board b;
                          b.action = "post";
                          b.corp = boardCorp_;
                          b.to = boardTo_;
                          b.days = (int)boardDays_;
                          if (!rows.empty() && giveAmount_ >= 1.0f)
                              b.giving.goods.push_back({ rows[(size_t)giveGood_].type.Index(),
                                                         (int)giveAmount_,
                                                         boardCorp_ ? "storage" : "" });
                          b.giving.credits = (long long)giveCredits_;
                          if (askAmount_ >= 1.0f && !types.empty())
                              b.asking.goods.push_back(
                                  { types[(size_t)askGood_].Index(), (int)askAmount_, "" });
                          b.asking.credits = (long long)askCredits_;
                          SendBoard(b);
                      }
                  });
            L.Text(
                "Held by the server from the moment it is posted. A flat fee to post; the market's "
                "fee is paid by whoever takes it. What is not taken goes back to you.",
                TextStyle::Small().Tint(t.colors.dim).Wrap());
        });
    L.End();
    L.Draw();
}
