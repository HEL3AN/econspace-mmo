#pragma once

#include "net/Tcp.h"
#include "sim/Observation.h"
#include "sim/Protocol.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The game half of econagent: an ordinary TCP client to econserver.
//
// Nothing here is privileged. It speaks the same Command/Snapshot protocol a human client
// speaks, which is the property that makes "an agent is just another player" true in the
// code and not only in the design.
namespace Agent
{

class Session
{
public:
    Session() = default;
    ~Session() { StopKeepalive(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Dials the host. False means no connection; the caller reports it and exits, because
    // there is no offline mode to fall back to.
    bool Connect(const std::string& host, unsigned short port, const std::string& account,
                 const std::string& secret);
    bool Alive() const { return conn_ && conn_->Alive(); }

    // Drains whatever has arrived. Cheap; call it often.
    void Pump();

    // Pumps from a thread of its own while nobody else is (#411). A model thinking for a
    // minute between tool calls is normal, and a connection nobody reads is one the server
    // stops sending to and eventually drops; pumped, it stays drained and acknowledged, and
    // the journal and chat keep accumulating. Everything else that touches the session holds
    // Lock() -- the stdio loop takes it for the whole of each request, so a tool sees one
    // consistent state and its own WaitUntil pumps as before.
    void        StartKeepalive();
    void        StopKeepalive();
    std::mutex& Lock() { return lock_; }

    // Pumps until `done` is true or the deadline passes. Returns what `done` last said.
    //
    // This is the blocking wait an agent needs so it can sleep through a two-minute flight
    // instead of polling and paying a model for every look. It blocks the BRIDGE, never
    // the game server -- the server keeps running regardless, and the connection is
    // serviced throughout, which is why this cannot be a plain sleep.
    bool WaitUntil(const std::function<bool()>& done, double timeoutSeconds);

    // Numbers every command, so the snapshot's `lastInput` says which ones the server has
    // applied. A purchase or a hand-in is not an order with a status (#109): the number is
    // the receipt, and the reason for a refusal is a journal Notice in the same snapshot
    // (#219). Returns the number given.
    int Send(const Proto::Command& c);
    // Sends and waits until a snapshot acknowledges it -- by then the snapshot also carries
    // its effect, because the server applies a command before it builds the next one. False
    // if no acknowledgement came in time (the server may be throttling this client).
    bool SendAndConfirm(const Proto::Command& c, double timeoutSeconds);

    const Proto::Snapshot&                    Snapshot() const { return snapshot_; }
    const std::map<int, Proto::EntityLayout>& Layout() const { return layout_.byId; }
    const Proto::GalaxyState&                 Galaxy() const { return galaxy_; }
    // The galaxy index as the server sent it (#206): names, map positions and links. Not
    // read from data/universe.json -- a generated region exists only on the server. Empty
    // until the login completes; the server sends it before the first layout.
    const WorldLoader::Universe& Universe() const { return universe_; }
    bool                         HasSnapshot() const { return haveSnapshot_; }
    // Where this pilot may begin, when the server asked (#342 S2): a new account, or one
    // whose system is gone. Until one is chosen there is no ship and no snapshot.
    bool                 AwaitingStart() const { return awaitingStart_; }
    const Proto::Starts& Starts() const { return starts_; }
    // Answers the server's question: a system from Starts(), or empty for any of them.
    void ChooseStart(const std::string& system);
    // The account this bridge logged in as: what it builds is owned by this name (#39).
    const std::string& Account() const { return account_; }

    // Journal entries newer than `seq`, accumulated across snapshots so nothing is lost
    // between two calls.
    std::vector<Ev::Event> EventsSince(int seq) const;
    int                    LastEventSeq() const { return lastEventSeq_; }

    // Chat (#347): every line heard, numbered in the order it arrived (the newest 256 kept),
    // and who is in the local channel of the system the ship is in.
    struct HeardLine
    {
        int             seq = 0;
        Proto::ChatLine line;
    };
    const std::vector<HeardLine>& ChatHeard() const { return chat_; }
    int                           LastChatSeq() const { return chatSeq_; }
    const Proto::ChatMembers&     ChatMembers() const { return members_; }
    void                          Say(const Proto::ChatSay& say);

    // A situation report from the current state; see sim/Observation.h.
    std::string Describe(Obs::Detail detail) const;

    const std::string& ProtocolError() const { return protocolError_; }
    // Why the server ended this session, if it said (#105). Empty otherwise -- a socket
    // that simply died says nothing, and pretending to know why would be worse.
    const std::string& ByeReason() const { return byeReason_; }
    // What the transport saw when the connection died ("peer closed the connection",
    // "recv failed (error 10054)"). Empty while it is alive.
    std::string CloseReason() const { return conn_ ? conn_->CloseReason() : std::string(); }

private:
    std::unique_ptr<Net::TcpConnection> conn_;
    Proto::Snapshot                     snapshot_;
    Proto::SnapshotReceiver             snapIn_;  // the states deltas are based on (#349)
    Proto::LayoutMirror                 layout_;  // kept current by deltas (#38)
    Proto::GalaxyState                  galaxy_;
    WorldLoader::Universe               universe_;
    std::vector<Ev::Event>              journal_;
    std::vector<HeardLine>              chat_;
    int                                 chatSeq_ = 0;
    Proto::ChatMembers                  members_;
    int                                 lastEventSeq_ = 0;
    int                                 commandSeq_ = 0;
    bool                                haveSnapshot_ = false;
    bool                                awaitingStart_ = false;
    Proto::Starts                       starts_;
    std::string                         protocolError_;
    std::string                         byeReason_;
    std::string                         account_;
    std::mutex                          lock_;
    std::thread                         keepalive_;
    std::atomic<bool>                   stopKeepalive_{ false };
};

}  // namespace Agent
