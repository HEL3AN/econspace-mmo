#pragma once

#include "net/Transport.h"

#include <memory>
#include <string>

// TCP implementation of the client<->server transport (track M, M4d-3). Messages
// are framed with a length prefix (uint32, network byte order) over the TCP byte
// stream. Sockets are non-blocking; I/O is pumped in Poll/Send. The platform's socket
// type is hidden in the .cpp (stored as unsigned long long) so the header pulls in
// neither <winsock2.h> nor <sys/socket.h> -- which is also what let the second platform
// be added without touching anything that includes this (#12).
namespace Net
{

// Per-process socket-library init/teardown (call once each). Startup before any sockets,
// Shutdown on exit. Winsock needs both; on POSIX they do nothing, and callers should not
// have to know which platform they are on.
bool Startup();
void Shutdown();

// An established TCP connection as an ITransport: Send frames and queues the
// message, Poll pumps the exchange and returns one complete message (false if none yet).
class TcpConnection : public ITransport
{
public:
    explicit TcpConnection(unsigned long long sock);
    ~TcpConnection() override;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    void Send(const std::string& msg) override;
    bool Poll(std::string& out) override;

    bool Alive() const { return alive_; }  // false — connection closed/dropped

    // Limits, enforced here rather than left to the peer's good manners (#14). A frame
    // header is four bytes the sender chose; believing it is how a single packet turns
    // into an out-of-memory kill. The send cap is the other direction: a client that
    // stops reading must not make the server buffer snapshots for it forever.
    //
    // The values are generous next to real traffic -- a full GalaxyState is a few tens of
    // kilobytes -- so hitting one means something is wrong, not that the game grew.
    static constexpr size_t MAX_FRAME_BYTES = 4u * 1024u * 1024u;
    static constexpr size_t MAX_SEND_BACKLOG = 8u * 1024u * 1024u;

private:
    void Pump();  // non-blocking send from outBuf_ and receive into inBuf_

    unsigned long long sock_;
    std::string        inBuf_;   // received bytes (accumulating frames)
    std::string        outBuf_;  // bytes to send (if the socket is busy)
    bool               alive_ = true;
};

// Listening socket: accepts incoming connections non-blockingly.
class TcpListener
{
public:
    ~TcpListener();

    bool Listen(unsigned short port);
    // Accept a connection; nullptr if none is queued.
    std::unique_ptr<TcpConnection> Accept();

private:
    // All ones is the invalid socket on both platforms once cast back: winsock's
    // INVALID_SOCKET is (SOCKET)~0, and (int)~0ull is -1, which is what a failed POSIX
    // socket() returns. That coincidence is why this header can stay platform-free.
    unsigned long long sock_ = ~0ull;
};

// Client connection to host:port (host is an IPv4 literal, e.g. "127.0.0.1").
// nullptr on error.
std::unique_ptr<TcpConnection> Dial(const std::string& host, unsigned short port);

}  // namespace Net
