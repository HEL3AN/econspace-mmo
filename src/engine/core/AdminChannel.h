#pragma once

#include <string>
#include <vector>

// In the engine, not the server (#431 slice 4): the world editor gives a running server its
// moderation commands (rename) through the same channel, and the editor links only the engine.
namespace Host
{

// --- The admin channel -----------------------------------------------------------------------
//
// A server run by systemd has no console. `econserver admin <dir> <command>` puts the command
// in <dir>/admin/ as a file; the running server picks it up within a second, runs it as if it
// had been typed at its console and writes what it said back beside it, which `admin` prints.
// It is local by construction: only someone who may write to the world's folder -- the
// account the server runs as, or root -- can give a command, and nothing listens on a port.

class AdminChannel
{
public:
    struct Request
    {
        std::string id;
        std::string command;
    };

    // The server's side. Makes <worldDir>/admin/ (readable by its owner only, where the system
    // has such a thing) and removes what is left in it: a command from before this start was
    // given to a server that is gone, and running a stale `shutdown` now would stop this one.
    bool               Open(const std::string& worldDir, std::string& error);
    bool               IsOpen() const { return !dir_.empty(); }
    const std::string& Dir() const { return dir_; }
    // The commands waiting, oldest first; each is taken (its file removed) as it is returned.
    std::vector<Request> Take();
    // What the command `id` said, for `admin` to print.
    void Answer(const std::string& id, const std::string& text);

private:
    std::string dir_;
};

// The other side. Gives `command` to the server running the world in `worldDir` and waits for
// its answer. False -- with `answer` saying why -- if no server took the command within
// `pickupSeconds` (it is then withdrawn) or did not answer within `answerSeconds`.
bool AdminRequest(const std::string& worldDir, const std::string& command, std::string& answer,
                  double pickupSeconds = 5.0, double answerSeconds = 120.0);

}  // namespace Host
