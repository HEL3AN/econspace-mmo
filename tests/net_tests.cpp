// TCP transport test: a command (client→server) and a snapshot (server→client)
// round-trip over real sockets on the 127.0.0.1 loopback with framing. This used
// to be econserver tcptest.
#include "doctest/doctest.h"

#include "net/Tcp.h"
#include "sim/Protocol.h"

#include "raw_socket.h"

#include <cstdint>
#include <memory>
#include <string>

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

TEST_CASE("a frame header the peer made up does not become memory the server allocates")
{
    // The four length bytes are chosen by whoever is on the other end, and the port is
    // open to anyone now (#3). Believing one is how a single packet becomes an
    // out-of-memory kill (#14). A raw socket is used deliberately: TcpConnection::Send
    // frames correctly, and a hostile peer would not.
    REQUIRE(Net::Startup());
    const unsigned short port = 50791;

    Net::TcpListener listener;
    REQUIRE(listener.Listen(port));

    const unsigned long long raw = RawSocket::Connect(port);
    REQUIRE(raw != 0);

    std::unique_ptr<Net::TcpConnection> server;
    for (int i = 0; i < 100000 && !server; i++)
        server = listener.Accept();
    REQUIRE(server);

    // A frame one byte past the cap: announced, and then never delivered. The length is
    // big-endian on the wire, written out here rather than with htonl so this file needs
    // no winsock of its own.
    const uint32_t    n = (uint32_t)(Net::TcpConnection::MAX_FRAME_BYTES + 1);
    const std::string header = { (char)((n >> 24) & 0xFF), (char)((n >> 16) & 0xFF),
                                 (char)((n >> 8) & 0xFF), (char)(n & 0xFF) };
    REQUIRE(RawSocket::Send(raw, header + "whatever"));

    std::string out;
    for (int i = 0; i < 100000 && server->Alive(); i++)
        server->Poll(out);

    CHECK_FALSE(server->Alive());    // dropped rather than buffered
    CHECK_FALSE(server->Poll(out));  // and nothing was handed up as a message
    RawSocket::Close(raw);
}
