// TCP transport test: a command (client→server) and a snapshot (server→client)
// round-trip over real sockets on the 127.0.0.1 loopback with framing. This used
// to be econserver tcptest.
#include "doctest/doctest.h"

#include "net/Tcp.h"
#include "sim/Protocol.h"

#include <memory>

TEST_CASE("TCP transport round-trips command and snapshot over loopback")
{
    REQUIRE(Net::Startup());
    const unsigned short port = 50790;

    Net::TcpListener listener;
    REQUIRE(listener.Listen(port));

    std::unique_ptr<Net::TcpConnection> client = Net::Dial("127.0.0.1", port);
    REQUIRE(client);

    std::unique_ptr<Net::TcpConnection> server;
    for (int i = 0; i < 1000 && !server; i++)
        server = listener.Accept();
    REQUIRE(server);

    // Client → server.
    Proto::Command c;
    c.thrust = true;
    c.targetId = 42;
    client->Send(Proto::EncodeCommand(c));

    std::string    msg;
    Proto::Command rc;
    bool           got = false;
    for (int i = 0; i < 100000 && !got; i++)
    {
        client->Poll(msg);
        if (server->Poll(msg))
            got = Proto::DecodeCommand(msg, rc);
    }
    REQUIRE(got);
    CHECK(rc.thrust);
    CHECK(rc.targetId == 42);

    // Server → client.
    Proto::Snapshot s;
    s.systemId = "core";
    s.player.hull = 80.0f;
    server->Send(Proto::EncodeSnapshot(s));

    Proto::Snapshot rs;
    bool            gotSnap = false;
    for (int i = 0; i < 100000 && !gotSnap; i++)
    {
        server->Poll(msg);
        if (client->Poll(msg))
            gotSnap = Proto::DecodeSnapshot(msg, rs);
    }
    REQUIRE(gotSnap);
    CHECK(rs.systemId == "core");

    Net::Shutdown();
}
