#pragma once

#include <cstdint>
#include <string>

// A socket that does not know the game's framing rules.
//
// Tests that exercise what happens when a peer sends nonsense need to send nonsense, and
// TcpConnection::Send always frames correctly. The winsock headers clash with raylib's
// (Rectangle, CloseWindow, ShowCursor), so the implementation lives in its own
// translation unit and only these three functions cross back.
namespace RawSocket
{

// Connects to 127.0.0.1:port. Returns 0 on failure.
unsigned long long Connect(unsigned short port);
// Sends the bytes exactly as given -- no length prefix, no validation.
bool Send(unsigned long long sock, const std::string& bytes);
void Close(unsigned long long sock);

}  // namespace RawSocket
