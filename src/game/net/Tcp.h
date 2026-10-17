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
    // Why it stopped being alive, in words, for the log: "peer closed", "recv failed
    // (10054)", "send backlog over the cap". Empty while alive. A dropped connection that
    // says nothing is exactly what made a hung CI run impossible to diagnose.
    const std::string& CloseReason() const { return closeReason_; }
    // Bytes framed and not yet taken by the socket: what the peer has not read, past what
    // the kernel buffers hold. The server stops adding snapshots above a share of the cap
    // rather than reaching it (#411).
    size_t Queued() const { return outBuf_.size(); }

    // Limits, enforced here rather than left to the peer's good manners (#14). A frame
    // header is four bytes the sender chose; believing it is how a single packet turns
    // into an out-of-memory kill. The send cap is the other direction: a client that
    // stops reading must not make the server buffer snapshots for it forever.
    //
    // The values are generous next to real traffic -- a full GalaxyState is a few tens of
    // kilobytes -- so hitting one means something is wrong, not that the game grew.
    static constexpr size_t MAX_FRAME_BYTES = 4u * 1024u * 1024u;
    static constexpr size_t MAX_SEND_BACKLOG = 8u * 1024u * 1024u;

    // The server's side is held tighter (#341): everything a client sends is a command or a
    // login, a few hundred bytes, so a frame of 64 KB is already not one of them; and a
    // client that has let a megabyte of snapshots pile up is a minute behind and no longer
    // playing. The client keeps the defaults above -- it is sent whole galaxies.
    static constexpr size_t SERVER_MAX_FRAME_BYTES = 64u * 1024u;
    static constexpr size_t SERVER_MAX_SEND_BACKLOG = 1024u * 1024u;
    void                    SetLimits(size_t maxFrame, size_t maxSendBacklog)
    {
        maxFrame_ = maxFrame;
        maxBacklog_ = maxSendBacklog;
    }

    // Who is on the other end, as an IPv4 literal ("127.0.0.1"); empty if unknown. The
    // server's per-address limits are keyed by it.
    const std::string& PeerAddress() const { return peer_; }
    void               SetPeerAddress(const std::string& a) { peer_ = a; }

private:
    void Pump();  // non-blocking send from outBuf_ and receive into inBuf_
    void Fail(const std::string& why);

    unsigned long long sock_;
    std::string        inBuf_;   // received bytes (accumulating frames)
    std::string        outBuf_;  // bytes to send (if the socket is busy)
    bool               alive_ = true;
    std::string        closeReason_;
    std::string        peer_;
    size_t             maxFrame_ = MAX_FRAME_BYTES;
    size_t             maxBacklog_ = MAX_SEND_BACKLOG;
};

// Listening socket: accepts incoming connections non-blockingly.
class TcpListener
{
public:
    ~TcpListener();

    // Who may connect (#187). Loopback unless asked: a listener on a public interface is
    // what Windows' firewall stops and asks about, once per executable path, and an
    // unanswered prompt stalls an unattended server. It is also the safer default for a
    // protocol with no transport encryption.
    enum class Exposure
    {
        Loopback,  // this machine only
        Public     // every interface
    };
    // Port 0 lets the operating system choose a free one; Port() then says which (#306).
    // A fixed port in the dynamic range (49152-65535) can be handed to any outbound
    // connection on the machine as its source port, so a test that hard-codes one fails
    // whenever a browser happens to hold it.
    bool Listen(unsigned short port, Exposure exposure = Exposure::Loopback);
    // The port actually listened on; 0 before a successful Listen.
    unsigned short Port() const { return port_; }
    // Accept a connection; nullptr if none is queued.
    std::unique_ptr<TcpConnection> Accept();

private:
    // All ones is the invalid socket on both platforms once cast back: winsock's
    // INVALID_SOCKET is (SOCKET)~0, and (int)~0ull is -1, which is what a failed POSIX
    // socket() returns. That coincidence is why this header can stay platform-free.
    unsigned long long sock_ = ~0ull;
    unsigned short     port_ = 0;
};

// Client connection to host:port (host is an IPv4 literal, e.g. "127.0.0.1").
// nullptr on error, or when nothing answers within `timeoutSeconds`. The connect itself is
// bounded rather than left to the operating system, whose idea of how long to wait for a
// SYN that is never answered is anything from two seconds to minutes. On failure `why`, if
// given, says what happened in words a player can act on: refused, unreachable, no answer.
std::unique_ptr<TcpConnection> Dial(const std::string& host, unsigned short port,
                                    double timeoutSeconds = 10.0, std::string* why = nullptr);

}  // namespace Net
