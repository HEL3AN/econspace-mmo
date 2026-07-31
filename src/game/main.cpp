// EconSpace — entry point. All logic lives in the Game class.
//   econspace                      — single-player (server in-process)
//   econspace connect <host> [port] — client to a remote econserver host over TCP
#include "core/Game.h"

#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    std::string    host;            // empty — single-player mode
    unsigned short port = 50800;
    if (argc >= 3 && std::strcmp(argv[1], "connect") == 0)
    {
        host = argv[2];
        if (argc >= 4)
            port = (unsigned short)std::atoi(argv[3]);
    }

    Game game(host, port);
    game.Run();
    return 0;
}
