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
    bool Connect(const std::string& host, unsigned short port);
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

    void Send(const Proto::Command& c);

    const Proto::Snapshot&                    Snapshot() const { return snapshot_; }
    const std::map<int, Proto::EntityLayout>& Layout() const { return layout_; }
    const Proto::GalaxyState&                 Galaxy() const { return galaxy_; }
    bool                                      HasSnapshot() const { return haveSnapshot_; }

    // Journal entries newer than `seq`, accumulated across snapshots so nothing is lost
    // between two calls.
    std::vector<Ev::Event> EventsSince(int seq) const;
    int                    LastEventSeq() const { return lastEventSeq_; }

    // A situation report from the current state; see sim/Observation.h.
    std::string Describe(Obs::Detail detail, const WorldLoader::Universe* universe) const;

    const std::string& ProtocolError() const { return protocolError_; }

private:
    std::unique_ptr<Net::TcpConnection> conn_;
    Proto::Snapshot                     snapshot_;
    std::map<int, Proto::EntityLayout>  layout_;
    Proto::GalaxyState                  galaxy_;
    std::vector<Ev::Event>              journal_;
    int                                 lastEventSeq_ = 0;
    bool                                haveSnapshot_ = false;
    std::string                         protocolError_;
};

}  // namespace Agent
