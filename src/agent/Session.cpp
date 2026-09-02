#include "agent/Session.h"

#include "agent/Jsonrpc.h"

#include <chrono>
#include <thread>

namespace Agent
{

bool Session::Connect(const std::string& host, unsigned short port, const std::string& account)
{
    if (!Net::Startup())
        return false;
    conn_ = Net::Dial(host, port);
    if (!conn_)
        return false;
    // An agent is a player like any other, so it introduces itself the same way: the
    // server has no player behind the socket until this arrives (#3, #42).
    Proto::Hello hello;
    hello.account = account;
    conn_->Send(Proto::EncodeHello(hello));
    return true;
}

void Session::Pump()
{
    if (!conn_)
        return;

    std::string msg;
    while (conn_->Poll(msg))
    {
        // Version first. Without this check a mismatch would decode into defaults and look
        // like a world where nothing ever happens -- see PROTO_VERSION.
        int ver = Proto::MessageVersion(msg);
        if (ver != Proto::PROTO_VERSION)
        {
            if (protocolError_.empty())
            {
                protocolError_ = "server speaks protocol v" + std::to_string(ver) +
                                 ", this build speaks v" + std::to_string(Proto::PROTO_VERSION);
                Rpc::Log("econagent: " + protocolError_);
            }
            continue;
        }

        const std::string type = Proto::MessageType(msg);
        if (type == "bye")
        {
            // The server is ending this on purpose. Keep the reason: "the connection is
            // closed" is true of a crash too, and an agent that cannot tell the two apart
            // will retry a login that is going to be displaced again.
            Proto::Bye b;
            if (Proto::DecodeBye(msg, b))
            {
                byeReason_ = b.reason;
                Rpc::Log("econagent: server ended the session: " + b.reason);
            }
            continue;
        }
        if (type == "snap")
        {
            Proto::Snapshot s;
            if (!Proto::DecodeSnapshot(msg, s))
                continue;
            // Names, sizes and ore live in the layout, not in every tick (#16).
            Proto::CompleteFromLayout(s, layout_);
            // Journal entries accumulate here rather than being replaced: two snapshots may
            // arrive between two tool calls, and dropping the first would lose exactly the
            // event the agent is waiting on.
            for (const Ev::Event& e : s.events)
                if (e.seq > lastEventSeq_)
                {
                    journal_.push_back(e);
                    lastEventSeq_ = e.seq;
                }
            if (journal_.size() > 512)
                journal_.erase(journal_.begin(), journal_.begin() + (journal_.size() - 512));
            snapshot_ = std::move(s);
            haveSnapshot_ = true;
        }
        else if (type == "layout")
        {
            Proto::SystemLayout lay;
            if (!Proto::DecodeLayout(msg, lay))
                continue;
            layout_.clear();
            for (const Proto::EntityLayout& el : lay.entities)
                layout_[el.id] = el;
        }
        else if (type == "galaxy")
        {
            Proto::DecodeGalaxy(msg, galaxy_);
        }
    }
}

bool Session::WaitUntil(const std::function<bool()>& done, double timeoutSeconds)
{
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::duration<double>(timeoutSeconds);

    for (;;)
    {
        Pump();
        if (done())
            return true;
        if (!Alive() || clock::now() >= deadline)
            return done();
        // Short enough that a completed order is noticed promptly, long enough that waiting
        // out a two-minute flight does not spin a core.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Session::Send(const Proto::Command& c)
{
    if (conn_)
        conn_->Send(Proto::EncodeCommand(c));
}

std::vector<Ev::Event> Session::EventsSince(int seq) const
{
    std::vector<Ev::Event> out;
    for (const Ev::Event& e : journal_)
        if (e.seq > seq)
            out.push_back(e);
    return out;
}

std::string Session::Describe(Obs::Detail detail, const WorldLoader::Universe* universe) const
{
    Obs::View v;
    v.snapshot = haveSnapshot_ ? &snapshot_ : nullptr;
    v.layout = &layout_;
    v.universe = universe;
    v.galaxy = &galaxy_;
    return Obs::Describe(v, detail);
}

}  // namespace Agent
