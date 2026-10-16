#include "agent/Session.h"

#include "agent/Jsonrpc.h"

#include "sim/Auth.h"

#include "core/Archetype.h"

#include <chrono>
#include <thread>

namespace Agent
{

bool Session::Connect(const std::string& host, unsigned short port, const std::string& account,
                      const std::string& secret)
{
    account_ = account;
    if (!Net::Startup())
        return false;
    conn_ = Net::Dial(host, port, 10.0);
    if (!conn_)
    {
        Rpc::Log("econagent: nothing accepted the connection within 10 s");
        return false;
    }
    // An agent is a player like any other, so it logs in the same way and through the
    // same code (#3, #42, #106).
    std::string why;
    if (!Auth::ClientHandshake(*conn_, account, secret, 10.0, why))
    {
        byeReason_ = why;
        Rpc::Log("econagent: could not log in: " + why +
                 (conn_->Alive() ? std::string() : " (" + conn_->CloseReason() + ")"));
        return false;
    }
    return true;
}

void Session::Pump()
{
    if (!conn_)
        return;

    std::string msg;
    bool        gotSnap = false;
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
        if (type == "starts")
        {
            Proto::Starts s;
            if (Proto::DecodeStarts(msg, s))
            {
                starts_ = std::move(s);
                awaitingStart_ = true;
            }
            continue;
        }
        if (type == "snap")
        {
            Proto::Snapshot s;
            if (!snapIn_.Decode(msg, s))  // a delta, rebuilt onto what it names (#349)
                continue;
            gotSnap = true;
            // Names, sizes and ore live in the layout, not in every tick (#16).
            Proto::CompleteFromLayout(s, layout_.byId);
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
            layout_.Reset(lay);
        }
        else if (type == "ldelta")
        {
            // The system changed while the agent was in it (#38): a structure built or
            // gone, a wreck searched. Without this the agent would be told to dock at a
            // station that no longer exists.
            Proto::LayoutDelta d;
            if (Proto::DecodeLayoutDelta(msg, d))
                layout_.Apply(d);
        }
        else if (type == "galaxy")
        {
            Proto::DecodeGalaxy(msg, galaxy_);
        }
        else if (type == "design")
        {
            // A design a player made (#279 fitting): known from here on, so the hangar can
            // say what a refitted ship does.
            Proto::DesignDef def;
            std::string      why;
            if (Proto::DecodeDesignDef(msg, def) && !Archetypes::AddDesign(def.design, why))
                Rpc::Log("econagent: design " + def.design.id + " from the server: " + why);
        }
        else if (type == "factions")
        {
            // The server's faction registry (#342 S5): a native faction is reported by its
            // name, not as unaffiliated.
            Proto::FactionList f;
            if (Proto::DecodeFactions(msg, f))
                Proto::ApplyFactions(f);
        }
        else if (type == "chat")
        {
            // Kept between tool calls like the journal: a line said while the agent was
            // flying is still there when it next looks (#347).
            Proto::ChatLines c;
            if (Proto::DecodeChatLines(msg, c))
                for (const Proto::ChatLine& l : c.lines)
                    chat_.push_back({ ++chatSeq_, l });
            if (chat_.size() > 256)
                chat_.erase(chat_.begin(), chat_.begin() + (long)(chat_.size() - 256));
        }
        else if (type == "members")
        {
            Proto::ChatMembers m;
            if (Proto::DecodeChatMembers(msg, m))
                members_ = std::move(m);
        }
        else if (type == "universe")
        {
            // Replaced whole: it is sent whole, at login and whenever it changes (#206).
            WorldLoader::Universe u;
            if (Proto::DecodeUniverse(msg, u))
                universe_ = std::move(u);
        }
    }
    // What the next delta is based on (#349). An agent between tool calls acknowledges
    // nothing, and is sent whole snapshots until it pumps again.
    if (gotSnap)
        conn_->Send(Proto::EncodeAck({ snapIn_.Latest() }));
}

void Session::StartKeepalive()
{
    if (keepalive_.joinable())
        return;
    stopKeepalive_ = false;
    keepalive_ = std::thread(
        [this]
        {
            while (!stopKeepalive_)
            {
                {
                    std::lock_guard<std::mutex> hold(lock_);
                    if (Alive())
                        Pump();
                }
                // Ten times a second: far inside the server's two seconds of patience, and
                // nothing next to the cost of decoding what arrived.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
}

void Session::StopKeepalive()
{
    if (!keepalive_.joinable())
        return;
    stopKeepalive_ = true;
    keepalive_.join();
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

void Session::ChooseStart(const std::string& system)
{
    if (!conn_ || !awaitingStart_)
        return;
    Proto::ChooseStart c;
    c.system = system;
    conn_->Send(Proto::EncodeChooseStart(c));
    awaitingStart_ = false;
}

void Session::Say(const Proto::ChatSay& say)
{
    if (conn_)
        conn_->Send(Proto::EncodeChatSay(say));
}

int Session::Send(const Proto::Command& c)
{
    Proto::Command numbered = c;
    numbered.seq = ++commandSeq_;
    if (conn_)
        conn_->Send(Proto::EncodeCommand(numbered));
    return numbered.seq;
}

bool Session::SendAndConfirm(const Proto::Command& c, double timeoutSeconds)
{
    const int seq = Send(c);
    return WaitUntil([&] { return haveSnapshot_ && snapshot_.player.lastInput >= seq; },
                     timeoutSeconds);
}

std::vector<Ev::Event> Session::EventsSince(int seq) const
{
    std::vector<Ev::Event> out;
    for (const Ev::Event& e : journal_)
        if (e.seq > seq)
            out.push_back(e);
    return out;
}

std::string Session::Describe(Obs::Detail detail) const
{
    Obs::View v;
    v.snapshot = haveSnapshot_ ? &snapshot_ : nullptr;
    v.layout = &layout_.byId;
    v.universe = &universe_;
    v.galaxy = &galaxy_;
    return Obs::Describe(v, detail);
}

}  // namespace Agent
