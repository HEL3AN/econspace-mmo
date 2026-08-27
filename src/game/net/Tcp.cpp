#include "net/Tcp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstring>

namespace Net
{

// The differences between winsock and Berkeley sockets, in one place (#12). Everything
// below this block is written once and compiles on both.
namespace
{
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

int LastError()
{
    return WSAGetLastError();
}
bool WouldBlock(int e)
{
    return e == WSAEWOULDBLOCK;
}
void CloseSocket(socket_t s)
{
    closesocket(s);
}
void SetNonBlocking(socket_t s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
// Windows never raises a signal for writing to a dead socket; send returns an error.
constexpr int SEND_FLAGS = 0;
#else
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;

int LastError()
{
    return errno;
}
bool WouldBlock(int e)
{
    // Two names for one condition, and on some platforms two different values.
    return e == EAGAIN || e == EWOULDBLOCK;
}
void CloseSocket(socket_t s)
{
    close(s);
}
void SetNonBlocking(socket_t s)
{
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
// Writing to a socket the peer has closed raises SIGPIPE on POSIX, and the default
// disposition kills the process. A server that dies because one client hung up is not a
// server, so the signal is suppressed per call instead.
#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif
#endif
}  // namespace

bool Startup()
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;  // nothing to initialize: sockets are just file descriptors
#endif
}

void Shutdown()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

// --- TcpConnection ---------------------------------------------------------

TcpConnection::TcpConnection(unsigned long long sock) : sock_(sock)
{
    socket_t s = (socket_t)sock_;
    SetNonBlocking(s);
    int yes = 1;  // disable Nagle — low latency for realtime
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
#if !defined(_WIN32) && !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&yes, sizeof(yes));  // macOS
#endif
}

TcpConnection::~TcpConnection()
{
    if ((socket_t)sock_ != INVALID_SOCK)
        CloseSocket((socket_t)sock_);
}

void TcpConnection::Send(const std::string& msg)
{
    if (!alive_)
        return;
    // A peer that has stopped reading is a peer we cannot serve. Dropping it is kinder to
    // everyone else than growing a send buffer until the process dies (#14).
    if (outBuf_.size() + msg.size() + 4 > MAX_SEND_BACKLOG)
    {
        alive_ = false;
        outBuf_.clear();
        return;
    }
    uint32_t len = htonl((uint32_t)msg.size());
    outBuf_.append((const char*)&len, 4);
    outBuf_.append(msg);
    Pump();
}

void TcpConnection::Pump()
{
    if (!alive_)
        return;
    socket_t s = (socket_t)sock_;

    // Send what's buffered: push as much as the socket accepts; the rest waits.
    while (!outBuf_.empty())
    {
        int n = (int)send(s, outBuf_.data(), (int)outBuf_.size(), SEND_FLAGS);
        if (n > 0)
        {
            outBuf_.erase(0, (size_t)n);
        }
        else if (n == 0)
        {
            alive_ = false;
            break;
        }
        else
        {
            if (WouldBlock(LastError()))
                break;  // socket buffer full — send the rest later
            alive_ = false;
            break;
        }
    }

    // Receive everything available into inBuf_.
    char buf[4096];
    for (;;)
    {
        int n = (int)recv(s, buf, sizeof(buf), 0);
        if (n > 0)
        {
            inBuf_.append(buf, (size_t)n);
            // A frame that never completes is still a frame being buffered. Without this
            // a peer can stream bytes under one oversized header until memory runs out.
            if (inBuf_.size() > MAX_FRAME_BYTES + 4)
            {
                alive_ = false;
                inBuf_.clear();
                break;
            }
        }
        else if (n == 0)
        {
            alive_ = false;
            break;
        }
        else
        {
            if (WouldBlock(LastError()))
                break;  // no data yet
            alive_ = false;
            break;
        }
    }
}

bool TcpConnection::Poll(std::string& out)
{
    Pump();
    if (inBuf_.size() < 4)
        return false;
    uint32_t len;
    std::memcpy(&len, inBuf_.data(), 4);
    len = ntohl(len);
    // The length is four bytes the peer chose. Refusing an absurd one is the difference
    // between a dropped connection and a dead server (#14).
    if ((size_t)len > MAX_FRAME_BYTES)
    {
        alive_ = false;
        inBuf_.clear();
        return false;
    }
    if (inBuf_.size() < 4 + (size_t)len)
        return false;  // frame hasn't fully arrived yet
    out.assign(inBuf_.data() + 4, (size_t)len);
    inBuf_.erase(0, 4 + (size_t)len);
    return true;
}

// --- TcpListener -----------------------------------------------------------

TcpListener::~TcpListener()
{
    if ((socket_t)sock_ != INVALID_SOCK)
        CloseSocket((socket_t)sock_);
}

bool TcpListener::Listen(unsigned short port)
{
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK)
        return false;

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 4) != 0)
    {
        CloseSocket(s);
        return false;
    }
    SetNonBlocking(s);
    sock_ = (unsigned long long)s;
    return true;
}

std::unique_ptr<TcpConnection> TcpListener::Accept()
{
    if ((socket_t)sock_ == INVALID_SOCK)
        return nullptr;
    socket_t c = accept((socket_t)sock_, nullptr, nullptr);
    if (c == INVALID_SOCK)
        return nullptr;
    return std::make_unique<TcpConnection>((unsigned long long)c);
}

// --- Dial ------------------------------------------------------------------

std::unique_ptr<TcpConnection> Dial(const std::string& host, unsigned short port)
{
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK)
        return nullptr;

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        CloseSocket(s);
        return nullptr;
    }

    // Blocking connect (localhost is fast); the socket becomes non-blocking in
    // the TcpConnection constructor.
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        CloseSocket(s);
        return nullptr;
    }
    return std::make_unique<TcpConnection>((unsigned long long)s);
}

}  // namespace Net
