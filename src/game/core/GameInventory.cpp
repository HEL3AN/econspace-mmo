// Game -- the inventory window (#472): every container reachable from where the pilot is,
// what each holds, and moving goods between them.
//
// The containers and their rows come from InventoryList, which the agent's inventory tool
// reads too. A move is an order to the server, which answers in the snapshot and the journal;
// nothing here changes a count by itself. Moves go by the buttons under the table -- the same
// ones a keyboard uses -- or by dragging a row onto another container on the left, through
// the window framework's drag and drop (ui/Drag.h). Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "sim/InventoryList.h"
#include "ui/Drag.h"
#include "ui/Theme.h"

#include <algorithm>
#include <string>
#include <vector>

void Game::RequestMove(const std::string& from, const std::string& to, ResourceType type,
                       int amount)
{
    if (amount <= 0 || from == to)
        return;
    // One verb between any two containers here, by the ids InventoryList gives them.
    Proto::Command c;
    c.moveFrom = from;
    c.moveTo = to;
    c.moveType = type.Index();
    c.moveAmount = amount;
    clientLink_->Send(Proto::EncodeCommand(c));
}

void Game::TakeMarketDrop(Ui::Layout& L)
{
    const Ui::DragPayload* d = L.Dropped("marketdrop", 0, "goods");
    if (d == nullptr || d->item < 0 || d->item >= Goods::Count())
        return;
    // The market lists every good in AllResourceTypes order, so a good's index is its row.
    marketSel_ = d->item;
    sellAmount_ = (float)std::max(1, d->amount);
    sellAmountFor_ = marketSel_;
}

void Game::DrawInventoryContent(const Ui::Frame& f)
{
    using InventoryList::Container;
    using InventoryList::Preset;
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = inventoryLayout_;

    const std::vector<Container> all = InventoryList::Reachable(snapshot_.player);
    if (InventoryList::Find(all, inventorySel_) == nullptr)
        inventorySel_ = all.front().id;  // the hold is always there
    const Container&                      sel = *InventoryList::Find(all, inventorySel_);
    static const std::vector<float>       NO_PRICES;
    const bool                            docked = mode_ == GameMode::Docked;
    const std::vector<InventoryList::Row> rows =
        InventoryList::Rows(sel, docked ? snapshot_.marketPrices : NO_PRICES, inventoryFilter_);

    // The good chosen; a new choice -- another good, another container -- starts at all of it.
    int chosen = -1;
    for (int i = 0; i < (int)rows.size(); i++)
        if (rows[i].type.Index() == inventoryGood_)
            chosen = i;
    if (chosen < 0 && !rows.empty())
    {
        chosen = 0;
        inventoryGood_ = rows[0].type.Index();
    }
    const std::string key = inventorySel_ + ":" + std::to_string(inventoryGood_);
    if (chosen >= 0 && inventoryAmountFor_ != key)
    {
        inventoryAmount_ = (float)rows[chosen].amount;
        inventoryAmountFor_ = key;
    }
    const int  available = chosen >= 0 ? rows[chosen].amount : 0;
    const int  amount = std::clamp((int)inventoryAmount_, std::min(1, available), available);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    L.Begin(f);
    // A row dropped on a container: moved there, the amount it carried. Against where the
    // containers were last laid out, as every question to a layout is.
    for (int i = 0; i < (int)all.size(); i++)
        if (const Ui::DragPayload* d = L.Dropped("cont", (uint32_t)i, "goods"))
            if (d->source != all[i].id && d->item >= 0 && d->item < Goods::Count())
                RequestMove(d->source, all[i].id, ResourceType::FromIndex(d->item), d->amount);

    const Ui::DragPayload* dragged = Ui::Drag::Active() ? Ui::Drag::Current() : nullptr;
    auto                   containers = [&]
    {
        for (int i = 0; i < (int)all.size(); i++)
        {
            const Container& c = all[i];
            const bool       isSel = c.id == inventorySel_;
            const bool       lit = dragged != nullptr && dragged->source != c.id &&
                                   L.DropHover("cont", (uint32_t)i, "goods");
            const float      frac = c.capacity > 0 ? (float)c.Used() / (float)c.capacity : 0.0f;
            L.Column(Box()
                         .Id("cont", (uint32_t)i)
                         .GrowX()
                         .Pad(t.metrics.gap)
                         .Gap(t.metrics.rowGap)
                         .Fill(isSel ? t.colors.selected
                               : lit ? t.colors.hover
                                     : Color{ 0, 0, 0, 0 })
                         .Border(lit ? t.colors.accent : t.colors.border, lit ? 2.0f : 1.0f)
                         .ThemeCorners(),
                     [&]
                     {
                         L.Field(c.name, TextFormat("%d / %d", c.Used(), c.capacity),
                                 isSel ? t.colors.text : t.colors.dim);
                         L.Bar(frac, frac >= 1.0f ? t.colors.warn : t.colors.accent);
                     });
            if (L.Clicked("cont", (uint32_t)i))
                inventorySel_ = c.id;
        }
        if (all.size() < 2)
            L.Text(docked ? "Nothing else here holds goods for you."
                          : "Docked at a structure of yours, its storage is listed here too.",
                   TextStyle::Small().Tint(t.colors.dim).Wrap());
    };

    Ui::TableSpec table;
    table.id = "goods";
    table.columns = { { "good", Size::Grow(), Ui::Align::Start, false },
                      { "amount", Size::Fit(44.0f), Ui::Align::End, false },
                      { "price", Size::Fit(40.0f), Ui::Align::End, false },
                      { "value", Size::Fit(48.0f), Ui::Align::End, false } };
    table.rows = (int)rows.size();
    table.empty = inventoryFilter_.empty() ? "Empty." : "Nothing by that name.";
    table.cell = [&](int r, int c)
    {
        const InventoryList::Row& row = rows[r];
        switch (c)
        {
            case 0: return Ui::Cell{ row.name, t.colors.text };
            case 1: return Ui::Cell{ std::to_string(row.amount), t.colors.text };
            case 2:
                return Ui::Cell{ row.price > 0.0f ? TextFormat("%.1f", row.price) : "-",
                                 t.colors.dim };
            default:
                return Ui::Cell{ row.value > 0.0 ? TextFormat("%.0f", row.value) : "-",
                                 t.colors.money };
        }
    };
    table.rowFill = [&](int r) { return r == chosen ? t.colors.selected : Color{ 0, 0, 0, 0 }; };
    // A row carries the amount set for it if it is the chosen one, everything otherwise; Shift
    // carries everything always.
    table.drag = [&](int r, Ui::DragPayload& out)
    {
        const InventoryList::Row& row = rows[r];
        out.kind = "goods";
        out.source = inventorySel_;
        out.item = row.type.Index();
        out.available = row.amount;
        out.amount = r == chosen && !shift ? amount : row.amount;
        out.label = std::to_string(out.amount) + " " + row.name;
        return true;
    };

    auto details = [&]
    {
        L.TextField("filter", inventoryFilter_, { "Filter by name", 32, Size::Grow() });
        const Ui::TableEvents ev = L.Table(table);
        if (ev.clicked >= 0)
            inventoryGood_ = rows[ev.clicked].type.Index();
        if (chosen < 0)
            return;
        const InventoryList::Row& row = rows[chosen];
        L.Row(Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
              [&]
              {
                  L.Text(row.name, TextStyle::Strong());
                  L.NumberField("amount", inventoryAmount_, 1.0f, (float)available);
                  const struct
                  {
                      const char* id;
                      const char* label;
                      Preset      preset;
                  } presets[] = { { "p1", "1", Preset::One },
                                  { "p10", "10", Preset::Ten },
                                  { "phalf", "Half", Preset::Half },
                                  { "pall", "All", Preset::All } };
                  for (const auto& p : presets)
                      if (L.Button(p.id, p.label))
                          inventoryAmount_ =
                              (float)InventoryList::PresetAmount(p.preset, available);
              });
        L.Row(Box().GrowX().Gap(t.metrics.rowGap),
              [&]
              {
                  for (const Container& to : all)
                  {
                      if (to.id == sel.id)
                          continue;
                      const int n = InventoryList::Movable(sel, to, row.type, amount);
                      if (L.Button("move_" + to.id, TextFormat("Move %d to %s", n, to.name.c_str()),
                                   true, n > 0))
                          RequestMove(sel.id, to.id, row.type, n);
                  }
              });
    };

    const bool wide = f.Area().width >= Ui::Px(420.0f);
    L.Column(Box().Grow().Gap(t.metrics.gap),
             [&]
             {
                 if (wide)
                     L.Row(Box().Grow().Gap(t.metrics.padding),
                           [&]
                           {
                               L.Column(Box()
                                            .Width(Size::Percent(0.32f))
                                            .Height(Size::Grow())
                                            .Gap(t.metrics.rowGap),
                                        containers);
                               L.Column(Box().Grow().Gap(t.metrics.rowGap), details);
                           });
                 else
                 {
                     L.Column(Box().GrowX().Gap(t.metrics.rowGap), containers);
                     L.Column(Box().Grow().Gap(t.metrics.rowGap), details);
                 }
                 L.Text(all.size() > 1 ? "Drag a good onto a container to move it, or onto the "
                                         "market to sell it there. Shift drags all of it."
                                       : "Drag a good onto the market to sell it there.",
                        TextStyle::Small().Tint(t.colors.dim).Wrap());
             });
    L.End();
    L.Draw();
}
