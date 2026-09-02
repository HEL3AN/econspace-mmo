#pragma once

#include <deque>
#include <string>
#include <utility>

// Client<->server message transport (track M, M4d). An abstraction over the
// delivery of string messages (JSON commands/snapshots) so the command/snapshot
// logic doesn't depend on what's underneath — an in-process loopback
// in-process or over the network. Implementations:
//   LocalTransport — in-process loopback channel, used by the server's own smoke tests;
//   TcpTransport   — network (winsock on Windows, Berkeley sockets elsewhere).
// Swapping TCP for UDP/ENet is a new ITransport implementation, no logic changes.
struct ITransport
{
    virtual ~ITransport() = default;
    virtual void Send(const std::string& msg) = 0;  // send one message
    virtual bool Poll(std::string& out) = 0;        // take an incoming one (false if empty)
};

// In-process loopback transport: two ends (client and server) with crossed
// queues. Send on one end is delivered to Poll on the other — no network and no
// stream copying, for a client and a server living in one process -- which is a test
// arrangement now, not a way to play (#23).
class LocalTransport
{
public:
    LocalTransport() : clientEnd_(c2s_, s2c_), serverEnd_(s2c_, c2s_) {}

    LocalTransport(const LocalTransport&) = delete;
    LocalTransport& operator=(const LocalTransport&) = delete;

    ITransport& Client() { return clientEnd_; }  // the client end
    ITransport& Server() { return serverEnd_; }  // the server end

private:
    // A channel end: writes to the out-queue, reads from the in-queue.
    class Endpoint : public ITransport
    {
    public:
        Endpoint(std::deque<std::string>& out, std::deque<std::string>& in) : out_(out), in_(in) {}

        void Send(const std::string& msg) override { out_.push_back(msg); }

        bool Poll(std::string& out) override
        {
            if (in_.empty())
                return false;
            out = std::move(in_.front());
            in_.pop_front();
            return true;
        }

    private:
        std::deque<std::string>& out_;
        std::deque<std::string>& in_;
    };

    // Queues are declared before the ends — they construct first, and the ends
    // hold references to them.
    std::deque<std::string> c2s_;  // client -> server
    std::deque<std::string> s2c_;  // server -> client
    Endpoint                clientEnd_;
    Endpoint                serverEnd_;
};
