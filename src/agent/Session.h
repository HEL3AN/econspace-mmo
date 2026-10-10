#pragma once

#include "net/Tcp.h"
#include "sim/Observation.h"
#include "sim/Protocol.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
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
    // Dials the host. False means no connection; the caller reports it and exits, because
    // there is no offline mode to fall back to.
    bool Connect(const std::string& host, unsigned short port, const std::string& account,
                 const std::string& secret);
    bool Alive() const { return conn_ && conn_->Alive(); }

    // Drains whatever has arrived. Cheap; call it often.
    void Pump();

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

    // Journal entries newer than `seq`, accumulated across snapshots so nothing is lost
    // between two calls.
    std::vector<Ev::Event> EventsSince(int seq) const;
    int                    LastEventSeq() const { return lastEventSeq_; }

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
    Proto::LayoutMirror                 layout_;  // kept current by deltas (#38)
    Proto::GalaxyState                  galaxy_;
    WorldLoader::Universe               universe_;
    std::vector<Ev::Event>              journal_;
    int                                 lastEventSeq_ = 0;
    int                                 commandSeq_ = 0;
    bool                                haveSnapshot_ = false;
    std::string                         protocolError_;
    std::string                         byeReason_;
};

}  // namespace Agent
