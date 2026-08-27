#include "raw_socket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

// No raylib here, deliberately: including both sets of headers in one translation unit
// makes Rectangle and CloseWindow mean two different things.
namespace RawSocket
{

unsigned long long Connect(unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        closesocket(s);
        return 0;
    }
    return (unsigned long long)s;
}

bool Send(unsigned long long sock, const std::string& bytes)
{
    return send((SOCKET)sock, bytes.data(), (int)bytes.size(), 0) == (int)bytes.size();
}

void Close(unsigned long long sock)
{
    closesocket((SOCKET)sock);
}

}  // namespace RawSocket
