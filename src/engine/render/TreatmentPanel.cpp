#include "render/TreatmentPanel.h"

#include "ui/Theme.h"

#include <cstdio>
#include <string>

namespace Render
{
namespace
{
std::string Format(const char* format, float v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), format, v);
    return buf;
}
}  // namespace

bool TreatmentPanel::Draw(const Ui::Frame& f, Treatment& t, const MaterialLibrary* materials)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& th = Ui::CurrentTheme();
    Ui::Layout&      L = layout_;
    TreatmentConfig& cfg = t.Config();
    bool             changed = false;

    // A button as wide as it is tall: the arrows that reorder a pass.
    auto square = [&](const std::string& id, const char* label)
    {
        bool clicked = false;
        L.Row(Box().Width(Size::Fixed(th.metrics.buttonHeight)),
              [&] { clicked = L.Button(id, label); });
        return clicked;
    };
    // An on/off switch: a button lit while it is on, saying which it is.
    auto toggle = [&](const std::string& id, const std::string& label, bool& on)
    {
        if (!L.Button(id, label + (on ? ": on" : ": off"), on))
            return;
        on = !on;
        changed = true;
    };

    L.Begin(f);
    L.Scroll(
        Box().Grow().Id("page").Gap(th.metrics.gap),
        [&]
        {
            if (!t.Available())
                // Said plainly and in the place a player would look, rather than only in a
                // log nobody reads. A machine whose driver refused the shaders is a machine
                // that plays the game without them, and it should say so instead of looking
                // broken.
                L.Text("No passes on this machine: the picture is drawn without them.",
                       TextStyle::Body().Tint(th.colors.warn).Wrap());
            else
                L.Text(std::to_string(cfg.chain.size()) + " passes, applied top to bottom",
                       TextStyle::Label());

            // Every shader that failed on this machine, by name, at the top: this is the part
            // that explains why the picture looks the way it does (#190).
            const bool plain = materials != nullptr && !materials->Problems().empty();
            if (!t.Problems().empty() || plain)
                L.Column(Box().GrowX().Gap(th.metrics.rowGap),
                         [&]
                         {
                             for (const std::string& problem : t.Problems())
                                 L.Text(problem, TextStyle::Small().Tint(th.colors.warn).Wrap());
                             if (!plain)
                                 return;
                             L.Text("MATERIALS DRAWN PLAIN", TextStyle::Label());
                             for (const std::string& problem : materials->Problems())
                                 L.Text(problem, TextStyle::Small().Tint(th.colors.warn).Wrap());
                         });

            // The HUD exclusion is a separate switch and not a pass, because it is not about
            // how the effect looks: the HUD carries numbers people fly by, and a pixelated
            // fuel gauge is a worse game whatever the chain is doing.
            L.Row(Box().GrowX().Gap(th.metrics.rowGap),
                  [&]
                  {
                      toggle("enabled", "Treatment", cfg.enabled);
                      toggle("hud", "HUD treated", cfg.treatHud);
                  });
            if (!cfg.enabled)
                L.Text("Off: the raw picture, every pass skipped.",
                       TextStyle::Small().Tint(th.colors.dim).Wrap());

            L.Divider();
            for (size_t i = 0; i < cfg.chain.size(); i++)
            {
                Pass&             p = cfg.chain[i];
                const std::string n = std::to_string(i);
                L.Column(Box().GrowX().Gap(th.metrics.rowGap),
                         [&]
                         {
                             // Order is a decision: bloom before pixelation gives soft fat
                             // pixels, after it gives hard pixel edges that glow. So the
                             // arrows sit on every row rather than the order being fixed in
                             // code.
                             L.Row(Box().GrowX().Gap(th.metrics.rowGap),
                                   [&]
                                   {
                                       std::string name = PassName(p.kind);
                                       if (!t.Compiled(p.kind))
                                           name += " (unavailable)";
                                       toggle("pass" + n, name, p.enabled);
                                       if (square("up" + n, "^") && i > 0)
                                       {
                                           cfg.MoveUp(i);
                                           changed = true;
                                       }
                                       if (square("down" + n, "v") && i + 1 < cfg.chain.size())
                                       {
                                           cfg.MoveDown(i);
                                           changed = true;
                                       }
                                   });
                             if (!p.enabled)
                                 return;
                             L.Field("amount", Format("%.2f", p.amount), th.colors.text);
                             changed |= L.Slider("amount" + n, p.amount, 0.0f, 1.0f);
                             L.Field(ScaleMeaning(p.kind), Format("%.2f", p.scale), th.colors.text);
                             // By ratio: 0.5 to 1 is as large a step as 4 to 8.
                             changed |= L.Slider("scale" + n, p.scale, 0.5f, 8.0f, true);
                         });
            }

            L.Divider();
            if (L.Button("reset", "Reset to default"))
            {
                const bool hud = cfg.treatHud;
                cfg = TreatmentConfig::Default();
                cfg.treatHud = hud;  // a preference about the HUD, not part of the look
                changed = true;
            }
        });
    L.End();
    L.Draw();
    return changed;
}

}  // namespace Render
