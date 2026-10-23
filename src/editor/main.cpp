// EconSpace world editor — entry point.
#include "Editor.h"
#include "core/DataDir.h"
#include "core/Version.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "raylib.h"
#include "render/Scene.h"

// What `worldeditor --help` prints, and an unknown argument under its complaint (#485). The
// words combine: `gallery shapes card planet.rocky zoom 3 shot out.png`.
static void PrintUsage(FILE* to)
{
    std::fprintf(
        to,
        "%s (%s) -- the world editor, and the gallery where a look is judged\n"
        "\n"
        "  worldeditor                       the hand-written world in data/universe.json\n"
        "  worldeditor gallery [shapes] [settings] [card ID|N [zoom Z]]\n"
        "      every archetype at once, or one drawn large; `shapes` starts on the shape\n"
        "      backend, `settings` opens the screen treatment's panel\n"
        "      a card also takes: seed N, light A, livery ID, heading H, damage D, fleet N,\n"
        "      budget MP, sockets, thrusting, truescale\n"
        "  worldeditor modules [pack P] [seeds N] [zoom Z]\n"
        "      the module library, every variant\n"
        "  worldeditor survey [SEED] [card N]\n"
        "      a centre and its frontier per seed, flagged empty, thin or twin\n"
        "  worldeditor region [SEED] [system ID | map]\n"
        "      edit a generated system (Ctrl+S saves the difference as a pin), or the map\n"
        "  worldeditor galaxy [SEED] [slot N | system ID] [export [ID]]\n"
        "      every slot of the seed's skeleton; `export` writes the region as a pack\n"
        "  worldeditor world PATH [slot N | system ID] [export [ID]]\n"
        "      the same for a saved world: world.json or its folder\n"
        "  worldeditor newpack ID | editpack ID [packs DIR]\n"
        "      begin or open an admin pack (in data/packs/ unless DIR is given)\n"
        "\n"
        "  with any of them:\n"
        "      shot FILE [frames N] [treated]   save the view after N frames and exit\n"
        "      perf [frames N] [notreat]        print what N frames cost and exit\n"
        "      time T                           every frame at this moment of the clock\n"
        "      noshaders, nopaint               draw as a machine without shaders would\n"
        "      --data DIR                       the data folder\n"
        "\n"
        "--version prints the version. Examples: CLAUDE.md, \"Build and run\".\n",
        Version::NAME, Version::NUMBER);
}

int main(int argc, char** argv)
{
    // Before data/ is looked for: a version or the usage is asked of a binary that may be
    // anywhere.
    if (argc > 1 && std::string(argv[1]) == "--version")
    {
        std::printf("%s (%s)\n", Version::NAME, Version::NUMBER);
        return 0;
    }
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h"))
    {
        PrintUsage(stdout);
        return 0;
    }
    // The source data/ from the build tree, else data/ beside the exe, or --data <dir> (#399).
    if (DataDir::Resolve(argc, argv, "worldeditor").empty())
        return 1;
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
    // The arguments are read before the window opens -- Editor's constructor opens it -- so a
    // word the editor does not know stops it with the usage instead of being ignored (#485).
    // What an argument asks of the editor is kept, in order, and done once it exists.
    std::vector<std::function<void(Editor&)>> apply;
    std::vector<std::string>                  unknown;
    bool                                      hidden = false;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "gallery")
        {
            gallery = true;
            apply.push_back([](Editor& e) { e.OpenGallery(); });
        }
        else if (arg == "modules")
            modules = true;  // the module library, every variant (#240)
        else if (arg == "shapes")
            apply.push_back([](Editor& e) { e.UseShapes(); });
        else if (arg == "settings")
            apply.push_back([](Editor& e) { e.OpenTreatmentSettings(); });
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
            apply.push_back([dir = std::string(argv[++i])](Editor& e) { e.SetPacksDir(dir); });
        else if (arg == "system" && i + 1 < argc)
            systemId = argv[++i];
        else if (arg == "card" && i + 1 < argc)
            card = argv[++i];
        else if (arg == "seed" && i + 1 < argc)  // which object of a type
            apply.push_back([n = std::atoi(argv[++i])](Editor& e) { e.SetGallerySeed(n); });
        else if (arg == "light" && i + 1 < argc)  // radians: the night side
            apply.push_back([a = (float)std::atof(argv[++i])](Editor& e) { e.SetGalleryLight(a); });
        else if (arg == "livery" && i + 1 < argc)  // whose style (#516)
            apply.push_back([id = std::string(argv[++i])](Editor& e) { e.SetGalleryLivery(id); });
        else if (arg == "heading" && i + 1 < argc)
            apply.push_back([h = (float)std::atof(argv[++i])](Editor& e)
                            { e.SetGalleryHeading(h); });
        else if (arg == "budget" && i + 1 < argc)  // paintBudget, megapixels
            apply.push_back([mp = (float)std::atof(argv[++i])](Editor& e)
                            { e.SetPaintBudget(mp); });
        else if (arg == "fleet" && i + 1 < argc)  // many at once, for `perf`
            apply.push_back([n = std::atoi(argv[++i])](Editor& e) { e.SetGalleryFleet(n); });
        else if (arg == "damage" && i + 1 < argc)
            apply.push_back([d = (float)std::atof(argv[++i])](Editor& e)
                            { e.SetGalleryIntensity(1.0f - d); });
        else if (arg == "sockets")
            apply.push_back([](Editor& e) { e.ShowGallerySockets(); });
        else if (arg == "thrusting")  // engines lit, for a shot of a ship under way
            apply.push_back([](Editor& e) { e.SetGalleryThrusting(); });
        else if (arg == "truescale")  // every card at one scale (#426)
            apply.push_back([](Editor& e) { e.SetGalleryTrueScale(); });
        else if (arg == "pack" && i + 1 < argc)
            pack = argv[++i];
        else if (arg == "seeds" && i + 1 < argc)
            seeds = std::atoi(argv[++i]);
        else if (arg == "zoom" && i + 1 < argc)
            zoom = (float)std::atof(argv[++i]);
        else if (arg == "shot" && i + 1 < argc)
        {
            shot = argv[++i];
            hidden = true;
        }
        else if (arg == "frames" && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (arg == "perf")
        {
            perf = true;
            hidden = true;
        }
        else if (arg == "treated")
            treated = true;  // a shot through the screen treatment, as it is seen
        else if (arg == "time" && i + 1 < argc)
            Render::FreezeLocalClock(std::atof(argv[++i]));  // every frame at this moment
        else if (arg == "notreat")
            notreat = true;
        else if (arg == "noshaders")  // every object as a driver with no shaders draws it
            apply.push_back([](Editor& e) { e.DisableMaterials(); });
        else if (arg == "nopaint")  // the painted bodies' stand-ins, under `hull`
            apply.push_back([](Editor& e) { e.DisablePainting(); });
        else
            unknown.push_back(arg);  // or a word that takes a value, given none
    }
    if (!unknown.empty())
    {
        for (const std::string& u : unknown)
            std::fprintf(stderr, "worldeditor: unknown argument '%s'\n", u.c_str());
        std::fprintf(stderr, "\n");
        PrintUsage(stderr);
        return 2;
    }
    // A shot is taken from a render texture, so it needs no window anyone can see -- and a
    // visible one, opened and closed over and over by a script, steals focus from whoever is
    // using the machine (#293). Decided before the window exists: the constructor opens it.
    if (hidden)
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
    Editor editor;
    for (const auto& a : apply)
        a(editor);
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
