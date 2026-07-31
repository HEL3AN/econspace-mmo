#include "net/Tcp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstring>

namespace Net
{

bool Startup()
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void Shutdown()
{
    WSACleanup();
}

namespace
{
void SetNonBlocking(SOCKET s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
}  // namespace

// --- TcpConnection ---------------------------------------------------------

TcpConnection::TcpConnection(unsigned long long sock) : sock_(sock)
{
    SOCKET s = (SOCKET)sock_;
    SetNonBlocking(s);
    int yes = 1;  // disable Nagle — low latency for realtime
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
}

TcpConnection::~TcpConnection()
{
    if ((SOCKET)sock_ != INVALID_SOCKET)
        closesocket((SOCKET)sock_);
}

void TcpConnection::Send(const std::string& msg)
{
    uint32_t len = htonl((uint32_t)msg.size());
    outBuf_.append((const char*)&len, 4);
    outBuf_.append(msg);
    Pump();
}

void TcpConnection::Pump()
{
    if (!alive_)
        return;
    SOCKET s = (SOCKET)sock_;

    // Send what's buffered: push as much as the socket accepts; the rest waits.
    while (!outBuf_.empty())
    {
        int n = send(s, outBuf_.data(), (int)outBuf_.size(), 0);
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
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                break;  // socket buffer full — send the rest later
            alive_ = false;
            break;
        }
    }

    // Receive everything available into inBuf_.
    char buf[4096];
    for (;;)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n > 0)
        {
            inBuf_.append(buf, (size_t)n);
        }
        else if (n == 0)
        {
            alive_ = false;
            break;
        }
        else
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
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
    if (inBuf_.size() < 4 + (size_t)len)
        return false;  // frame hasn't fully arrived yet
    out.assign(inBuf_.data() + 4, (size_t)len);
    inBuf_.erase(0, 4 + (size_t)len);
    return true;
}

// --- TcpListener -----------------------------------------------------------

TcpListener::~TcpListener()
{
    if ((SOCKET)sock_ != INVALID_SOCKET)
        closesocket((SOCKET)sock_);
}

bool TcpListener::Listen(unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
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
        closesocket(s);
        return false;
    }
    SetNonBlocking(s);
    sock_ = (unsigned long long)s;
    return true;
}

std::unique_ptr<TcpConnection> TcpListener::Accept()
{
    if ((SOCKET)sock_ == INVALID_SOCKET)
        return nullptr;
    SOCKET c = accept((SOCKET)sock_, nullptr, nullptr);
    if (c == INVALID_SOCKET)
        return nullptr;
    return std::make_unique<TcpConnection>((unsigned long long)c);
}

// --- Dial ------------------------------------------------------------------

std::unique_ptr<TcpConnection> Dial(const std::string& host, unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return nullptr;

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    // Blocking connect (localhost is fast); the socket becomes non-blocking in
    // the TcpConnection constructor.
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        closesocket(s);
        return nullptr;
    }
    return std::make_unique<TcpConnection>((unsigned long long)s);
}

}  // namespace Net
