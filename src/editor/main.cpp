// EconSpace world editor — entry point.
#include "Editor.h"
#include "core/DataDir.h"

#include <cstdlib>
#include <string>

#include "raylib.h"
#include "render/Scene.h"

int main(int argc, char** argv)
{
    // The source data/ from the build tree, else data/ beside the exe, or --data <dir> (#399).
    if (DataDir::Resolve(argc, argv, "worldeditor").empty())
        return 1;
    // A shot is taken from a render texture, so it needs no window anyone can see -- and a
    // visible one, opened and closed over and over by a script, steals focus from whoever is
    // using the machine (#293). Decided before the window exists: the constructor opens it.
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "shot" || std::string(argv[i]) == "perf")
            SetConfigFlags(FLAG_WINDOW_HIDDEN);
    Editor editor;
    // `worldeditor gallery` opens on the archetype gallery (#118) instead of the world,
    // and `shapes` starts on the shape backend. Both are conveniences for the one job the
    // editor now has that is not editing a world: judging a look. `settings` opens the
    // screen treatment's panel, which lists every shader that failed on this machine.
    // `survey [seed] [card N]` opens on the survey of generated systems (#141), from that
    // seed, optionally with card N enlarged -- synthetic clicks do not reach the window, so
    // that is how a screenshot of one is taken. `gallery card ID|N [zoom Z]` does the same
    // for one archetype drawn large (#194), by id or registry index, at Z times fitted.
    // `region SEED [system ID]` opens a generated system for editing (#237) -- the one
    // through the wormhole unless named -- and Ctrl+S saves the difference as a pin.
    // `region SEED map` opens the whole region as a map instead, `system ID` ringed on it.
    // `galaxy SEED` opens the world's galaxy, every slot of its skeleton (#342 S8), and
    // `world PATH` does the same for a saved world (world.json or its folder), its regions
    // read from their seals; with either, `slot N` opens that slot's region, `system ID` that
    // system, and `export ID` writes the region open on the map as a pack and opens it.
    // `newpack ID` begins a region pack, `editpack ID` opens one; `packs DIR` puts them
    // somewhere other than data/packs/, which a server weaves into a new world.
    bool        galaxy = false;  // `galaxy SEED` or `world PATH`
    std::string worldPath;
    int         slot = -1;
    std::string exportId, newPack, editPack;
    bool        exporting = false;
    bool        survey = false;
    bool        modules = false;
    bool        gallery = false;
    bool        region = false;
    bool        regionMap = false;  // `region SEED map`
    std::string systemId;           // `region SEED system ID`
    uint64_t    seed = 1;
    std::string card;
    float       zoom = 1.0f;
    std::string shot;             // `shot FILE [frames N]`: save the view after N frames and exit
    bool        perf = false;     // `perf [frames N]`: print what N frames cost and exit (#296)
    bool        notreat = false;  // `notreat`: ...with the screen treatment off
    bool        treated = false;  // `treated`: `shot` through the screen treatment
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
        else if (arg == "region")
        {
            region = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (arg == "map")
            regionMap = true;
        else if (arg == "galaxy")
        {
            galaxy = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (arg == "world" && i + 1 < argc)
        {
            galaxy = true;
            worldPath = argv[++i];
        }
        else if (arg == "slot" && i + 1 < argc)
            slot = std::atoi(argv[++i]);
        else if (arg == "export")
        {
            exporting = true;  // the id is optional: region-<slot> without one
            if (i + 1 < argc && std::string(argv[i + 1]) != "shot" &&
                std::string(argv[i + 1]) != "frames")
                exportId = argv[++i];
        }
        else if (arg == "newpack" && i + 1 < argc)
            newPack = argv[++i];
        else if (arg == "editpack" && i + 1 < argc)
            editPack = argv[++i];
        else if (arg == "packs" && i + 1 < argc)
            editor.SetPacksDir(argv[++i]);
        else if (arg == "system" && i + 1 < argc)
            systemId = argv[++i];
        else if (arg == "card" && i + 1 < argc)
            card = argv[++i];
        else if (arg == "seed" && i + 1 < argc)
            editor.SetGallerySeed(std::atoi(argv[++i]));  // which object of a type
        else if (arg == "light" && i + 1 < argc)
            editor.SetGalleryLight((float)std::atof(argv[++i]));  // radians: the night side
        else if (arg == "livery" && i + 1 < argc)
            editor.SetGalleryLivery(argv[++i]);  // whose style (#516)
        else if (arg == "heading" && i + 1 < argc)
            editor.SetGalleryHeading((float)std::atof(argv[++i]));
        else if (arg == "budget" && i + 1 < argc)
            editor.SetPaintBudget((float)std::atof(argv[++i]));  // paintBudget, megapixels
        else if (arg == "fleet" && i + 1 < argc)
            editor.SetGalleryFleet(std::atoi(argv[++i]));  // many at once, for `perf`
        else if (arg == "damage" && i + 1 < argc)
            editor.SetGalleryIntensity(1.0f - (float)std::atof(argv[++i]));
        else if (arg == "sockets")
            editor.ShowGallerySockets();
        else if (arg == "thrusting")
            editor.SetGalleryThrusting();  // engines lit, for a shot of a ship under way
        else if (arg == "truescale")
            editor.SetGalleryTrueScale();  // every card at one scale (#426)
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
        else if (arg == "perf")
            perf = true;
        else if (arg == "treated")
            treated = true;  // a shot through the screen treatment, as it is seen
        else if (arg == "time" && i + 1 < argc)
            Render::FreezeLocalClock(std::atof(argv[++i]));  // every frame at this moment
        else if (arg == "notreat")
            notreat = true;
        else if (arg == "noshaders")
            editor.DisableMaterials();  // every object as a driver with no shaders draws it
        else if (arg == "nopaint")
            editor.DisablePainting();  // the painted bodies' stand-ins, under `hull`
    }
    if (!worldPath.empty() && editor.OpenWorldFile(worldPath))
        seed = editor.WorldSeed();
    if (!newPack.empty())
        editor.OpenPack(newPack, true);
    else if (!editPack.empty())
        editor.OpenPack(editPack, false);
    else if ((galaxy || region) && slot >= 0)
        editor.OpenRegionSlot(seed, slot);
    else if (galaxy && !systemId.empty())
        editor.OpenGenerated(seed, systemId);
    else if (galaxy && !exporting)
        editor.OpenWorldMap(seed);
    else if ((region && regionMap) || exporting)
        editor.OpenRegionMap(seed, systemId);
    else if (region)
        editor.OpenGenerated(seed, systemId);
    else if (modules)
        editor.OpenModules(zoom > 0.0f ? zoom : 1.0f, pack, seeds);
    else if (survey)
        editor.OpenSurvey(seed, card.empty() ? -1 : std::atoi(card.c_str()));
    else if (gallery && !card.empty())
        editor.FocusGallery(card, zoom > 0.0f ? zoom : 1.0f);
    if (exporting && newPack.empty() && editPack.empty())
        editor.ExportRegion(exportId);
    if (!shot.empty())
        editor.TakeShot(shot, frames, treated);
    if (perf)
        editor.MeasurePerf(frames, !notreat);
    editor.Run();
    return 0;
}
