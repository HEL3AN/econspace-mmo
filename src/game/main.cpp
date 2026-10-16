// EconSpace — entry point. All logic lives in the Game class.
//   econspace connect <host> <port> <name> <secret> [--zoom Z] [--warp X Y] [--map]
//   [--select] [--menu] [--sensor [RANGE]]
//   [--shot FILE [--frames N]] [--nohud] [--treated] [--perf] [--notreat] [--size W H]
//   [--uiscale S] [--open IDS] [--theme ID] [--blur] [--cblind] [--click X Y FRAME]...
//   [--say TEXT FRAME]... [--start ID|random] —
//   connect to an econserver host
//
// Connecting is mandatory: the world lives on an authoritative server and the
// client is a renderer plus an input source. The connection is established here,
// before the window opens, so Game cannot be constructed without one.
#include "core/DataDir.h"
#include "core/Game.h"
#include "net/Tcp.h"
#include "sim/Auth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int Usage(const char* exe)
{
    std::fprintf(stderr,
                 "EconSpace — connect to a server:\n"
                 "  %s connect <host> <port> <name> <secret>\n"
                 "      the secret is this account's; the first login sets it\n"
                 "      --zoom Z   start the camera at zoom Z (1 = a unit is a pixel)\n"
                 "      --warp X Y warp to the point (X, Y) two seconds after joining\n"
                 "      --map      start with the galaxy map open (G/Esc closes it)\n"
                 "      --select   select the nearest thing shortly after joining\n"
                 "      --menu     ...and open its right-click menu\n"
                 "      --dock     fly to the nearest station and dock, shortly after joining\n"
                 "      --sensor [RANGE]\n"
                 "                 start with the sensor screen open (V/Esc closes it), RANGE\n"
                 "                 world units across\n"
                 "      --shot FILE [--frames N]\n"
                 "                 save frame N (default 60) to FILE as a PNG and exit\n"
                 "      --treated  ...through the screen treatment, as the player sees it\n"
                 "      --nohud    draw the world without the HUD\n"
                 "      --perf     run N frames (--frames, default 60) uncapped and print\n"
                 "                 what they cost, CPU and GPU per phase, to stderr; F9\n"
                 "                 shows the same in game\n"
                 "      --notreat  start with the screen treatment off (look.json untouched)\n"
                 "      --size W H open the window at W x H\n"
                 "      --uiscale S\n"
                 "                 the interface at scale S, whatever the display asks for\n"
                 "      --open IDS start with these windows open, e.g. radar,missions,settings\n"
                 "      --theme ID the interface in data/ui_themes/ID.json for this run\n"
                 "      --blur     blur the world behind the windows, in a theme that offers it\n"
                 "      --cblind   the colour-blind standing palette, orange and blue\n"
                 "      --start ID|random\n"
                 "                 a new pilot begins at this start (a system id) or any,\n"
                 "                 without being shown the choice\n"
                 "      --click X Y FRAME\n"
                 "                 a left click at (X, Y) on frame FRAME; repeatable\n"
                 "      --scan FRAME\n"
                 "                 a pass of the ship's scanner on frame FRAME; repeatable\n"
                 "      --say TEXT FRAME\n"
                 "                 send TEXT to chat on frame FRAME, as typed (/w pilot ...\n"
                 "                 whispers); repeatable\n"
                 "\n"
                 "Start a server first:\n"
                 "  econserver host 50800\n",
                 exe);
    return 2;
}

int main(int argc, char** argv)
{
    // data/ is found at runtime -- beside the exe in a copied build (#399). Before the
    // arguments are read: --data is taken out of them.
    if (DataDir::Resolve(argc, argv, "econspace").empty())
        return 1;
    if (argc < 6 || std::strcmp(argv[1], "connect") != 0)
        return Usage(argv[0]);

    const std::string    host = argv[2];
    const unsigned short port = (unsigned short)std::atoi(argv[3]);
    // Who this is, and what proves it (#3, #106). The secret itself is never sent: the
    // server challenges, and the client answers with something good for one connection.
    const std::string account = argv[4];
    const std::string secret = argv[5];
    float             startZoom = 0.0f;  // 0: the default
    bool              warp = false;
    bool              map = false;
    bool              select = false, menu = false;
    bool              dock = false;
    bool              fit = false;  // --fit: the hangar open on its Fit tab (#279)
    std::string       market;       // --market GOOD: the market in front, on that good (#343)
    bool              sensor = false;
    float             sensorRange = 0.0f;  // 0: the default
    Vector2           warpTo = { 0.0f, 0.0f };
    std::string       shot;
    int               frames = 60;
    bool              nohud = false;
    bool              perf = false;
    bool              notreat = false;
    bool              treated = false;
    int               width = 0, height = 0;
    std::string       theme;  // --theme: for this run only
    bool              blur = false;
    bool              cblind = false;
    struct Click
    {
        float x, y;
        int   frame;
    };
    std::vector<Click> clicks;  // --click X Y FRAME, any number of them
    struct Say
    {
        std::string text;
        int         frame;
    };
    std::vector<Say> says;            // --say TEXT FRAME (#347)
    std::vector<int> scans;           // --scan FRAME (#346)
    float            uiScale = 0.0f;  // 0: the player's setting times the display's
    std::string      openWindows;     // --open radar,missions,settings
    std::string      startAt;         // --start ID|random (#342 S2)
    for (int i = 6; i < argc; i++)
        if (std::strcmp(argv[i], "--zoom") == 0 && i + 1 < argc)
            startZoom = (float)std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--shapes") == 0)
        {
            // accepted and ignored: shapes are the only look now (#123)
        }
        else if (std::strcmp(argv[i], "--map") == 0)
            map = true;
        else if (std::strcmp(argv[i], "--select") == 0)
            select = true;
        else if (std::strcmp(argv[i], "--menu") == 0)
            select = menu = true;
        else if (std::strcmp(argv[i], "--dock") == 0)
            dock = true;
        else if (std::strcmp(argv[i], "--fit") == 0)
            fit = true;
        else if (std::strcmp(argv[i], "--market") == 0 && i + 1 < argc)
            market = argv[++i];
        else if (std::strcmp(argv[i], "--sensor") == 0)
        {
            sensor = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                sensorRange = (float)std::atof(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--nohud") == 0)
            nohud = true;
        else if (std::strcmp(argv[i], "--perf") == 0)
            perf = true;
        else if (std::strcmp(argv[i], "--notreat") == 0)
            notreat = true;
        else if (std::strcmp(argv[i], "--treated") == 0)
            treated = true;
        else if (std::strcmp(argv[i], "--size") == 0 && i + 2 < argc)
        {
            width = std::atoi(argv[i + 1]);
            height = std::atoi(argv[i + 2]);
            i += 2;
        }
        else if (std::strcmp(argv[i], "--uiscale") == 0 && i + 1 < argc)
            uiScale = (float)std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--open") == 0 && i + 1 < argc)
            openWindows = argv[++i];
        else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc)
            theme = argv[++i];
        else if (std::strcmp(argv[i], "--blur") == 0)
            blur = true;
        else if (std::strcmp(argv[i], "--cblind") == 0)
            cblind = true;
        else if (std::strcmp(argv[i], "--start") == 0 && i + 1 < argc)
            startAt = argv[++i];
        else if (std::strcmp(argv[i], "--click") == 0 && i + 3 < argc)
        {
            clicks.push_back({ (float)std::atof(argv[i + 1]), (float)std::atof(argv[i + 2]),
                               std::atoi(argv[i + 3]) });
            i += 3;
        }
        else if (std::strcmp(argv[i], "--scan") == 0 && i + 1 < argc)
            scans.push_back(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--say") == 0 && i + 2 < argc)
        {
            says.push_back({ argv[i + 1], std::atoi(argv[i + 2]) });
            i += 2;
        }
        else if (std::strcmp(argv[i], "--warp") == 0 && i + 2 < argc)
        {
            warpTo = { (float)std::atof(argv[i + 1]), (float)std::atof(argv[i + 2]) };
            warp = true;
            i += 2;
        }

    if (!Net::Startup())
    {
        std::fprintf(stderr, "Network startup failed (winsock).\n");
        return 1;
    }

    std::unique_ptr<Net::TcpConnection> conn = Net::Dial(host, port);
    if (!conn)
    {
        // No fallback: there is no offline mode to fall back to.
        std::fprintf(stderr, "Could not connect to %s:%u — is econserver running there?\n",
                     host.c_str(), port);
        Net::Shutdown();
        return 1;
    }

    // Log in before anything else. Until this completes the server has a socket with
    // nobody behind it, and it drops one that stays silent.
    {
        std::string why;
        if (!Auth::ClientHandshake(*conn, account, secret, 10.0, why))
        {
            std::fprintf(stderr, "Could not log in as %s: %s\n", account.c_str(), why.c_str());
            Net::Shutdown();
            return 1;
        }
    }

    {
        // A shot comes from a render texture; a hidden window keeps a scripted run from
        // stealing focus from whoever is using the machine (#293).
        if (!shot.empty() || perf)
            SetConfigFlags(FLAG_WINDOW_HIDDEN);
        Game game(std::move(conn));
        if (width > 0 && height > 0)
            game.SetResolution(width, height);
        game.SetPilotName(account);
        if (uiScale > 0.0f)
            game.SetUiScale(uiScale);
        if (startZoom > 0.0f)
            game.SetStartZoom(startZoom);
        if (warp)
            game.StartWithWarp(warpTo);
        if (map)
            game.StartOnMap();
        if (select)
            game.StartWithSelection(menu);
        if (dock)
            game.StartDocking();
        if (sensor)
            game.StartOnSensor(sensorRange);
        if (!openWindows.empty())
            game.StartWithWindows(openWindows);
        if (!theme.empty())
            game.SetThemeForRun(theme);
        if (blur)
            game.SetBlurForRun(true);
        if (cblind)
            game.SetColourBlindForRun(true);
        if (!startAt.empty())
            game.AnswerStartWith(startAt);
        for (const Click& c : clicks)
            game.AddScriptedClick(c.x, c.y, c.frame);
        for (const Say& s : says)
            game.AddScriptedSay(s.text, s.frame);
        for (int f : scans)
            game.AddScriptedScan(f);
        if (fit)
            game.StartOnFit();
        if (!market.empty())
            game.StartOnMarket(market);
        if (!shot.empty())
            game.TakeShot(shot, frames, treated);
        if (nohud)
            game.HideHud();
        if (notreat)
            game.DisableTreatment();
        if (perf)
            game.MeasurePerf(frames);
        game.Run();
    }  // the socket closes with Game, before winsock is unloaded

    Net::Shutdown();
    return 0;
}
