// EconSpace world editor — entry point.
#include "Editor.h"

#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
    Editor editor;
    // `worldeditor gallery` opens on the archetype gallery (#118) instead of the world,
    // and `shapes` starts on the shape backend. Both are conveniences for the one job the
    // editor now has that is not editing a world: judging a look. `settings` opens the
    // screen treatment's panel, which lists every shader that failed on this machine.
    // `survey [seed] [card N]` opens on the survey of generated systems (#141), from that
    // seed, optionally with card N enlarged -- synthetic clicks do not reach the window, so
    // that is how a screenshot of one is taken. `gallery card ID|N [zoom Z]` does the same
    // for one archetype drawn large (#194), by id or registry index, at Z times fitted.
    bool        survey = false;
    bool        modules = false;
    bool        gallery = false;
    uint64_t    seed = 1;
    std::string card;
    float       zoom = 1.0f;
    std::string shot;  // `shot FILE [frames N]`: save the view after N frames and exit
    int         frames = 30;
    std::string pack;  // `modules pack P seeds N`: one pack, each variant at N seeds
    int         seeds = 3;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "gallery")
        {
            gallery = true;
            editor.OpenGallery();
        }
        else if (arg == "modules")
            modules = true;  // the module library, every variant (#240)
        else if (arg == "shapes")
            editor.UseShapes();
        else if (arg == "settings")
            editor.OpenTreatmentSettings();
        else if (arg == "survey")
        {
            survey = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (arg == "card" && i + 1 < argc)
            card = argv[++i];
        else if (arg == "pack" && i + 1 < argc)
            pack = argv[++i];
        else if (arg == "seeds" && i + 1 < argc)
            seeds = std::atoi(argv[++i]);
        else if (arg == "zoom" && i + 1 < argc)
            zoom = (float)std::atof(argv[++i]);
        else if (arg == "shot" && i + 1 < argc)
            shot = argv[++i];
        else if (arg == "frames" && i + 1 < argc)
            frames = std::atoi(argv[++i]);
    }
    if (modules)
        editor.OpenModules(zoom > 0.0f ? zoom : 1.0f, pack, seeds);
    else if (survey)
        editor.OpenSurvey(seed, card.empty() ? -1 : std::atoi(card.c_str()));
    else if (gallery && !card.empty())
        editor.FocusGallery(card, zoom > 0.0f ? zoom : 1.0f);
    if (!shot.empty())
        editor.TakeShot(shot, frames);
    editor.Run();
    return 0;
}
