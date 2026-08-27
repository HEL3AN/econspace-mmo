#include "raw_socket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// No raylib here, deliberately: on Windows, including both sets of headers in one
// translation unit makes Rectangle and CloseWindow mean two different things.
namespace RawSocket
{

#ifdef _WIN32
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
using socket_t = int;
static constexpr socket_t INVALID_SOCK = -1;
#endif

unsigned long long Connect(unsigned short port)
{
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK)
        return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return 0;
    }
    return (unsigned long long)s;
}

bool Send(unsigned long long sock, const std::string& bytes)
{
    return (int)send((socket_t)sock, bytes.data(), (int)bytes.size(), 0) == (int)bytes.size();
}

void Close(unsigned long long sock)
{
#ifdef _WIN32
    closesocket((socket_t)sock);
#else
    close((socket_t)sock);
#endif
}

}  // namespace RawSocket
