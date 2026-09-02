// EconSpace world editor — entry point.
#include "Editor.h"

#include <string>

int main(int argc, char** argv)
{
    Editor editor;
    // `worldeditor gallery` opens on the archetype gallery (#118) instead of the world,
    // and `shapes` starts on the shape backend. Both are conveniences for the one job the
    // editor now has that is not editing a world: judging a look.
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "gallery")
            editor.OpenGallery();
        else if (arg == "shapes")
            editor.UseShapes();
    }
    editor.Run();
    return 0;
}
