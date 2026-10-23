// Fuzz target 1 (#518): the server's frame reader, fed an arbitrary byte stream in chunks.
//
// A real TcpConnection over a socketpair, so the code under test is the code the server
// runs -- the length prefix, the cap (#14), the buffering across reads -- and not a copy of
// it. The input is cut into chunks (the first byte of each says how long), each is written
// to one end, and the other end is polled after every write, the way the host loop polls.
// Whatever comes out must respect the server's frame cap, and a connection that has failed
// must say why.
#include "net/Tcp.h"

#include <cstdint>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return 0;
    // TcpConnection makes its own end non-blocking; ours must not block either.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    {
        Net::TcpConnection conn((unsigned long long)fds[1]);
        conn.SetLimits(Net::TcpConnection::SERVER_MAX_FRAME_BYTES,
                       Net::TcpConnection::SERVER_MAX_SEND_BACKLOG);

        size_t      at = 0;
        std::string msg;
        while (at < size)
        {
            // One byte of chunk length (1..256), then that many bytes of stream.
            const size_t want = (size_t)data[at++] + 1;
            const size_t n = want < size - at ? want : size - at;
            size_t       sent = 0;
            while (sent < n)
            {
                const ssize_t w = write(fds[0], data + at + sent, n - sent);
                if (w <= 0)
                    break;  // the kernel buffer is full, or the reader has hung up
                sent += (size_t)w;
            }
            at += n;
            while (conn.Poll(msg))
                if (msg.size() > Net::TcpConnection::SERVER_MAX_FRAME_BYTES)
                    abort();  // a frame past the cap was handed to the server
            if (!conn.Alive() && conn.CloseReason().empty())
                abort();  // failed without saying why
        }
        while (conn.Poll(msg))
            if (msg.size() > Net::TcpConnection::SERVER_MAX_FRAME_BYTES)
                abort();
    }  // the connection closes fds[1]

    close(fds[0]);
    return 0;
}
