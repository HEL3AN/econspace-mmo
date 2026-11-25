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

void Game::TakeWorldDrop()
{
    // Goods let go over no window, in space, are jettisoned into a can (#472): the same move
    // the Jettison button sends, from wherever they were dragged out of.
    if (mode_ != GameMode::Flying || !desk_.WorldOwnsMouse() || !Ui::Drag::Dropping())
        return;
    const Ui::DragPayload* d = Ui::Drag::Take("goods");
    if (d != nullptr && d->item >= 0 && d->item < Goods::Count())
        RequestMove(d->source, "can", ResourceType::FromIndex(d->item), d->amount);
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

void Game::DrawHangarRent(Ui::Layout& L)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme&         t = Ui::CurrentTheme();
    const Proto::HangarView& h = snapshot_.player.hangar;
    if (h.capacity <= 0)
        return;
    // A member's hangar in a corporation's structure has no rent to set (#555): free room.
    if (h.rentPerUnit <= 0.0 || h.periodDays <= 0.0)
    {
        L.Field("Hangar", TextFormat("%d volume, free", h.free), t.colors.text);
        return;
    }
    const int most = std::max(0, h.most - h.free);
    if (rentFor_ != h.rented)  // the field follows what the server says is rented
    {
        rentUnits_ = (float)h.rented;
        rentFor_ = h.rented;
    }
    // Two lines, so the goods below keep the room: what it is, and the field to change it. The
    // terms are the rent button's tooltip.
    const std::string paid =
        h.rented > 0 && h.paidUntil > 0.0
            ? TextFormat(", paid %.0f days more",
                         std::max(0.0, (h.paidUntil - snapshot_.time) / 86400.0))
            : std::string();
    L.Field("Hangar", TextFormat("%d free + %d rented%s", h.free, h.rented, paid.c_str()),
            t.colors.text);
    if (h.owed > 0.0)
    {
        L.Text(TextFormat("Closed: %.0f cr of rent is owed. Docking here pays it when the money "
                          "is there.",
                          h.owed),
               TextStyle::Small().Tint(t.colors.bad).Wrap());
        if (L.Button("payowed", TextFormat("Pay %.0f cr", h.owed), true))
        {
            Proto::Command c;
            c.rentHangar = h.rented;
            clientLink_->Send(Proto::EncodeCommand(c));
        }
        return;
    }
    L.Row(Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
          [&]
          {
              L.Text("Rent", TextStyle::Label());
              L.NumberField("rent", rentUnits_, 0.0f, (float)most);
              const int want = std::clamp((int)rentUnits_, 0, most);
              // A rent begun pays a whole period; more units pay for what is left of it.
              const double period = h.periodDays * 86400.0;
              const double left = h.rented > 0 && h.paidUntil > snapshot_.time && period > 0.0
                                      ? (h.paidUntil - snapshot_.time) / period
                                      : 1.0;
              const double cost = want > h.rented
                                      ? (h.rented > 0 ? (want - h.rented) * h.rentPerUnit * left
                                                      : want * h.rentPerUnit)
                                      : 0.0;
              if (L.Button("setrent",
                           want > h.rented ? TextFormat("Rent %d  ~%.0f cr", want, cost)
                                           : TextFormat("Rent %d", want),
                           true, want != h.rented))
              {
                  Proto::Command c;
                  c.rentHangar = want;
                  clientLink_->Send(Proto::EncodeCommand(c));
              }
              L.Tooltip(
                  "setrent",
                  TextFormat("Volume on top of the %d free, at most %d here. %.1f cr per volume "
                             "every %.0f days, to the station's owner; more is paid now for "
                             "what is left of the period. Unpaid, the hangar closes until "
                             "it is paid; nothing in it is lost.",
                             h.free, most, h.rentPerUnit, h.periodDays));
          });
}

void Game::DrawAssetsTab(Ui::Layout& L)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    // Asked for when the tab is open and nothing was heard for a while: the server sends it
    // once, with the next snapshot.
    if (GetTime() - assetsAsked_ > 5.0 && !assetsHeard_)
    {
        Proto::Command c;
        c.requestAssets = true;
        clientLink_->Send(Proto::EncodeCommand(c));
        assetsAsked_ = GetTime();
    }
    L.Row(Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
          [&]
          {
              if (L.Button("assets_refresh", "Refresh"))
              {
                  Proto::Command c;
                  c.requestAssets = true;
                  clientLink_->Send(Proto::EncodeCommand(c));
                  assetsAsked_ = GetTime();
              }
              L.Text(TextFormat("%d places", (int)assets_.size()),
                     TextStyle::Small().Tint(t.colors.dim));
          });
    if (assets_.empty())
    {
        L.Text(assetsHeard_ ? "You keep nothing anywhere." : "Asking where your things are...",
               TextStyle::Small().Tint(t.colors.dim).Wrap());
        return;
    }
    assetSel_ = std::clamp(assetSel_, 0, (int)assets_.size() - 1);
    auto          units = [](const Proto::AssetView& a) { return InventoryList::Volume(a.goods); };
    Ui::TableSpec table;
    table.id = "assets";
    table.columns = { { "place", Size::Grow(), Ui::Align::Start, false },
                      { "system", Size::Fit(90.0f), Ui::Align::Start, false },
                      { "volume", Size::Fit(40.0f), Ui::Align::End, false } };
    table.rows = (int)assets_.size();
    table.cell = [&](int r, int c)
    {
        const Proto::AssetView& a = assets_[(size_t)r];
        const Color             tone = a.locked ? t.colors.bad : t.colors.text;
        if (c == 0)
            return Ui::Cell{ a.name, tone };
        if (c == 1)
            return Ui::Cell{ a.systemName.empty() ? "-" : a.systemName, t.colors.dim };
        return Ui::Cell{ std::to_string(units(a)), tone };
    };
    table.rowFill = [&](int r) { return r == assetSel_ ? t.colors.selected : Color{ 0, 0, 0, 0 }; };
    const Ui::TableEvents ev = L.Table(table);
    if (ev.clicked >= 0)
        assetSel_ = ev.clicked;
    const Proto::AssetView& a = assets_[(size_t)assetSel_];
    L.Text(a.name, TextStyle::Strong());
    if (!a.note.empty())
        L.Text(a.note, TextStyle::Small().Tint(a.locked ? t.colors.bad : t.colors.dim).Wrap());
    const std::vector<ResourceType> types = AllResourceTypes();
    for (size_t i = 0; i < a.goods.size() && i < types.size(); i++)
        if (a.goods[i] > 0)
            L.Field(ResourceName(types[i]), std::to_string(a.goods[i]), t.colors.text);
    if (!a.system.empty() && a.system != snapshot_.systemId &&
        L.Button("assets_route", TextFormat("Route to %s", a.systemName.c_str())))
        PlotMapRoute(a.system);
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

    const std::vector<Container> all =
        InventoryList::Reachable(snapshot_, layout_.byId, pilotName_);
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

    // A module dragged off a socket in the Fit tab lands in any container: it leaves the draft
    // fit, and Apply puts it in the hold or hangar as a refit does (#572).
    for (int i = 0; i < (int)all.size(); i++)
        if (const Ui::DragPayload* d = L.Dropped("cont", (uint32_t)i, "unfit"))
        {
            const size_t slash = d->source.find('/');
            if (slash != std::string::npos)
                ChangeFitDraft(d->source.substr(0, slash), d->source.substr(slash + 1), d->label,
                               -d->amount);
        }

    const Ui::DragPayload* dragged = Ui::Drag::Active() ? Ui::Drag::Current() : nullptr;
    auto                   containers = [&]
    {
        for (int i = 0; i < (int)all.size(); i++)
        {
            const Container& c = all[i];
            const bool       isSel = c.id == inventorySel_;
            const bool       lit = dragged != nullptr && dragged->source != c.id &&
                                   L.DropHover("cont", (uint32_t)i, dragged->kind);
            // A division shows what it holds against the structure's whole room, which is
            // said once above the first of them (#555): its own capacity is what the others
            // leave, and a different number on each line says nothing about the structure.
            const bool  pooled = c.pool > 0;
            const float frac = pooled           ? (float)c.Used() / (float)c.pool
                               : c.capacity > 0 ? (float)c.Used() / (float)c.capacity
                                                : 0.0f;
            if (pooled && (i == 0 || all[i - 1].pool == 0))
                L.Field("Storage", TextFormat("%d / %d", c.poolUsed, c.pool), t.colors.text);
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
                         L.Field(c.closed ? c.name + "  (closed)" : c.name,
                                 pooled ? TextFormat("%d", c.Used())
                                        : TextFormat("%d / %d", c.Used(), c.capacity),
                                 c.closed ? t.colors.bad
                                 : isSel  ? t.colors.text
                                          : t.colors.dim);
                         L.Bar(frac, frac >= 1.0f ? t.colors.warn : t.colors.accent);
                     });
            if (L.Clicked("cont", (uint32_t)i))
                inventorySel_ = c.id;
        }
        // Hangars at stations that refuse the pilot, and goods on their way (#472 slice 6).
        for (size_t i = 0; i < snapshot_.player.locked.size(); i++)
        {
            const Proto::LockedHangarView& lk = snapshot_.player.locked[i];
            const int                      n = InventoryList::Volume(lk.goods);
            L.Text(TextFormat("%s, %s: %d volume%s", lk.name.c_str(), lk.system.c_str(), n,
                              lk.hostile ? " out of reach" : ""),
                   TextStyle::Small().Tint(lk.hostile ? t.colors.bad : t.colors.dim).Wrap());
            if (lk.why.empty())
            {
                if (L.Button("fetch_" + std::to_string(i),
                             TextFormat("Bring here: %.0f cr, %.0f h", lk.fee, lk.hours)))
                {
                    Proto::Command c;
                    c.deliverFrom = lk.key;
                    clientLink_->Send(Proto::EncodeCommand(c));
                }
            }
            else
                L.Text(lk.why, TextStyle::Small().Tint(t.colors.dim).Wrap());
        }
        for (const Proto::ShipmentView& s : snapshot_.player.shipments)
        {
            const int n = InventoryList::Volume(s.goods);
            L.Text(TextFormat("On its way: %d volume from %s, %.0f h to go", n, s.from.c_str(),
                              s.hours),
                   TextStyle::Small().Tint(t.colors.dim).Wrap());
        }
        if (all.size() < 2)
            L.Text(docked ? "Nothing else here holds goods for you."
                          : "Docked at a structure of yours, its storage is listed here too.",
                   TextStyle::Small().Tint(t.colors.dim).Wrap());
    };

    Ui::TableSpec table;
    // One table per container: each fits its own room, which the hangar's rent lines make
    // smaller than the hold's.
    table.id = "goods_" + inventorySel_;
    table.columns = { { "good", Size::Grow(), Ui::Align::Start, false },
                      { "amount", Size::Fit(44.0f), Ui::Align::End, false },
                      { "vol", Size::Fit(28.0f), Ui::Align::End, false },
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
            case 2: return Ui::Cell{ std::to_string(row.volume), t.colors.dim };
            case 3:
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
        if (sel.id == "hangar")
            DrawHangarRent(L);
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
                      if (to.id == sel.id || to.takeOnly)
                          continue;
                      const int n = InventoryList::Movable(sel, to, row.type, amount);
                      if (L.Button("move_" + to.id, TextFormat("Move %d to %s", n, to.name.c_str()),
                                   true, n > 0))
                          RequestMove(sel.id, to.id, row.type, n);
                  }
              });
        // In space, out of the hold into a can beside the ship (#472).
        if (!docked && sel.id == "hold" &&
            L.Button("jettison", TextFormat("Jettison %d into a can", amount), false, amount > 0))
            RequestMove("hold", "can", row.type, amount);
    };

    const bool wide = f.Area().width >= Ui::Px(420.0f);
    L.Column(Box().Grow().Gap(t.metrics.gap),
             [&]
             {
                 L.Tabs("itabs", { "Here", "Assets" }, inventoryTab_);
                 if (inventoryTab_ == 1)
                 {
                     DrawAssetsTab(L);
                     return;
                 }
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
                 L.Text(!docked ? "Drag a good onto a container to move it, or out of the "
                                  "window into space to jettison it. Shift drags all of it."
                        : all.size() > 1 ? "Drag a good onto a container to move it, or onto the "
                                           "market to sell it there. Shift drags all of it."
                                         : "Drag a good onto the market to sell it there.",
                        TextStyle::Small().Tint(t.colors.dim).Wrap());
             });
    L.End();
    L.Draw();
}
