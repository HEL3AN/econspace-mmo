// EconSpace world editor — entry point.
#include "Editor.h"

#include <string>

int main(int argc, char** argv)
{
    Editor editor;
    // `worldeditor gallery` opens on the archetype gallery (#118) instead of the world.
    if (argc > 1 && std::string(argv[1]) == "gallery")
        editor.OpenGallery();
    editor.Run();
    return 0;
}
