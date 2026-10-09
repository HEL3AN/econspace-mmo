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
    // that is how a screenshot of one is taken.
    bool     survey = false;
    uint64_t seed = 1;
    int      card = -1;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "gallery")
            editor.OpenGallery();
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
            card = std::atoi(argv[++i]);
    }
    if (survey)
        editor.OpenSurvey(seed, card);
    editor.Run();
    return 0;
}
