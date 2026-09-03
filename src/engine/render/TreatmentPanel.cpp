#include "render/TreatmentPanel.h"

#include "ui/Controls.h"
#include "ui/UiTheme.h"

namespace Render
{
namespace
{
const float ROW = 22.0f;      // one line of text
const float SLIDER = 32.0f;   // a slider and its label
const float TOGGLE = 30.0f;   // a toggle and the gap after it
const float PASS_GAP = 8.0f;  // between one pass block and the next

float PassBlockHeight(bool expanded)
{
    return expanded ? (TOGGLE + SLIDER * 2.0f + PASS_GAP) : (TOGGLE + PASS_GAP);
}
}  // namespace

float TreatmentPanelHeight(const Treatment& t)
{
    float h = ROW * 2.0f + TOGGLE * 2.0f + 16.0f;  // title, availability, two master toggles
    for (const Pass& p : t.Config().chain)
        h += PassBlockHeight(p.enabled);
    h += ROW * (float)t.Problems().size();
    h += TOGGLE;  // the reset row
    return h;
}

bool DrawTreatmentPanel(Rectangle area, Treatment& t)
{
    TreatmentConfig& cfg = t.Config();
    bool             changed = false;

    const float x = area.x;
    const float w = area.width;
    float       y = area.y;

    Ui::Text("SCREEN TREATMENT", (int)x, (int)y, 13, Ui::ACCENT);
    y += ROW;

    if (!t.Available())
    {
        // Said plainly and in the place a player would look, rather than only in a log
        // nobody reads. A machine whose driver refused the shaders is a machine that plays
        // the game without them, and it should say so instead of looking broken.
        Ui::Text("no passes on this machine - drawing without them", (int)x, (int)y, 11,
                 Ui::TEXT_DIM);
        y += ROW;
    }
    else
    {
        Ui::Text(TextFormat("%d passes in the chain", (int)cfg.chain.size()), (int)x, (int)y, 11,
                 Ui::TEXT_DIM);
        y += ROW;
    }

    changed |=
        Ui::Toggle({ x, y, w, 24.0f }, cfg.enabled ? "treatment on" : "treatment off (raw picture)",
                   cfg.enabled);
    y += TOGGLE;

    // The HUD exclusion is a separate switch and not a pass, because it is not about how
    // the effect looks: the HUD carries numbers people fly by, and a pixelated fuel gauge
    // is a worse game whatever the chain is doing.
    changed |= Ui::Toggle({ x, y, w, 24.0f }, cfg.treatHud ? "HUD treated too" : "HUD kept clean",
                          cfg.treatHud);
    y += TOGGLE + 6.0f;

    for (size_t i = 0; i < cfg.chain.size(); i++)
    {
        Pass&      p = cfg.chain[i];
        const bool have = t.Compiled(p.kind);

        // Order is a decision: bloom before pixelation gives soft fat pixels, after it
        // gives hard pixel edges that glow. So the arrows sit on every row rather than the
        // order being fixed in code.
        const float arrowW = 22.0f;
        Rectangle   up{ x + w - arrowW * 2.0f - 4.0f, y, arrowW, 24.0f };
        Rectangle   down{ x + w - arrowW, y, arrowW, 24.0f };

        Rectangle   row{ x, y, w - arrowW * 2.0f - 8.0f, 24.0f };
        const char* label =
            have ? PassName(p.kind) : TextFormat("%s (unavailable)", PassName(p.kind));
        bool on = p.enabled;
        if (Ui::Toggle(row, label, on))
        {
            p.enabled = on;
            changed = true;
        }

        if (Ui::SmallButton(up, "^") && i > 0)
        {
            cfg.MoveUp(i);
            changed = true;
        }
        if (Ui::SmallButton(down, "v") && i + 1 < cfg.chain.size())
        {
            cfg.MoveDown(i);
            changed = true;
        }
        y += TOGGLE;

        if (p.enabled)
        {
            changed |=
                Ui::Slider({ x + 12.0f, y, w - 12.0f, 28.0f }, "amount", p.amount, 0.0f, 1.0f);
            y += SLIDER;
            changed |= Ui::Slider({ x + 12.0f, y, w - 12.0f, 28.0f }, ScaleMeaning(p.kind), p.scale,
                                  0.5f, 8.0f);
            y += SLIDER;
        }
        y += PASS_GAP;
    }

    for (const std::string& problem : t.Problems())
    {
        Ui::Text(problem.c_str(), (int)x, (int)y, 10, Ui::TEXT_DIM);
        y += ROW;
    }

    if (Ui::SmallButton({ x, y, 120.0f, 22.0f }, "reset to default"))
    {
        const bool hud = cfg.treatHud;
        cfg = TreatmentConfig::Default();
        cfg.treatHud = hud;  // a preference about the HUD, not part of the look
        changed = true;
    }

    return changed;
}

}  // namespace Render
