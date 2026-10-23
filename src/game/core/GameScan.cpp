// Game -- the scanner window (#346): a pass of the ship's scanner, and the signatures of the
// system as this pilot's scans have them -- a code and how much of it is resolved, then what it
// is. Presentation only: what is hidden and what a pass finds is the server's (sim/
// Simulation_Scan.cpp), and a hidden object is not in this client's layout until it is found.
// Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "ui/Theme.h"

#include <string>

void Game::DrawScannerContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme&       t = Ui::CurrentTheme();
    Ui::Layout&            L = scannerLayout_;
    const Proto::ScanView& scan = snapshot_.player.scan;
    const double           now = worldClock_.Now(GetTime());
    auto                   systemName = [&](const std::string& id)
    {
        for (const auto& si : universe_.systems)
            if (si.id == id)
                return si.name;
        return id;
    };

    L.Begin(f);
    L.Column(Box().Grow().Gap(t.metrics.rowGap),
             [&]
             {
                 const bool running = scan.until > now;
                 if (running && scan.seconds > 0.0)
                 {
                     const double left = scan.until - now;
                     L.Text(TextFormat("Scanning... %.0f s", left), TextStyle::Strong());
                     L.Bar((float)(1.0 - left / scan.seconds), t.colors.accent);
                 }
                 else
                 {
                     const bool can = mode_ == GameMode::Flying && !snapshot_.player.docked;
                     if (L.Button("scan", "Scan the system", true, can))
                         cmd_.scan = true;
                     L.Text(can ? "A pass covers the whole system. What is hidden -- a wormhole, a "
                                  "hidden base, a rich belt -- shows as a signature, and each "
                                  "pass resolves more of it."
                                : "Undock to scan.",
                            TextStyle::Small().Tint(t.colors.dim).Wrap());
                 }
                 L.Divider();
                 if (scan.sigs.empty())
                 {
                     L.Text("No signatures found here yet", TextStyle::Body().Tint(t.colors.dim));
                     return;
                 }
                 L.Text("SIGNATURES", TextStyle::Label());
                 L.Scroll(
                     Box().Id("sigs").Grow().Gap(t.metrics.rowGap),
                     [&]
                     {
                         for (const Proto::SignatureView& g : scan.sigs)
                         {
                             if (g.progress < 1.0f)
                             {
                                 L.Field(g.code, TextFormat("unknown  %.0f%%", g.progress * 100.0f),
                                         t.colors.dim);
                                 continue;
                             }
                             const std::string what =
                                 g.dest.empty() ? g.kind : g.kind + " to " + systemName(g.dest);
                             L.Row(Box()
                                       .GrowX()
                                       .Gap(t.metrics.gap)
                                       .Align(Ui::Align::Start, Ui::Align::Center),
                                   [&]
                                   {
                                       L.Text(g.code, TextStyle::Strong().Tint(t.colors.good));
                                       L.Text(what, TextStyle::Body());
                                       L.Spacer();
                                       if (L.Button("sel" + g.code, "Select"))
                                           selected_ = FindEntityById(g.id);
                                   });
                         }
                     });
             });
    L.End();
    L.Draw();
}
