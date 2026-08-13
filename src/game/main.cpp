// EconSpace — entry point. All logic lives in the Game class.
//   econspace connect <host> [port] — connect to an econserver host over TCP
//
// Connecting is mandatory: the world lives on an authoritative server and the
// client is a renderer plus an input source. The connection is established here,
// before the window opens, so Game cannot be constructed without one.
#include "core/Game.h"
#include "net/Tcp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

static int Usage(const char* exe)
{
    std::fprintf(stderr,
                 "EconSpace — connect to a server:\n"
                 "  %s connect <host> [port]     port defaults to 50800\n"
                 "\n"
                 "Start a server first:\n"
                 "  econserver host 50800\n",
                 exe);
    return 2;
}

int main(int argc, char** argv)
{
    if (argc < 3 || std::strcmp(argv[1], "connect") != 0)
        return Usage(argv[0]);

    const std::string    host = argv[2];
    const unsigned short port = (argc >= 4) ? (unsigned short)std::atoi(argv[3]) : 50800;

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

    {
        Game game(std::move(conn));
        game.Run();
    }  // the socket closes with Game, before winsock is unloaded

    Net::Shutdown();
    return 0;
}
