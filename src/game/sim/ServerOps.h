#pragma once

#include <string>
#include <vector>

// Running a server as a service (#400): copies of its world taken while it runs, a way to give
// it console commands when nobody is at its console, and a clean stop on a signal.
namespace Host
{

// --- Backups ---------------------------------------------------------------------------------

// What a world directory holds that a backup copies: the saves and the server's settings, not
// its logs, its backups or the admin channel -- and not whatever else shares the folder, which
// for a world beside the executable is the executable and data/.
extern const char* const WORLD_ENTRIES[];

// A backup folder's name for the time `isoStamp` (Log::Now()'s form): "2026-10-17T09-41-07Z",
// the colons gone because Windows does not allow them in a name.
std::string BackupName(const std::string& isoStamp);

// Copies the world in `worldDir` to backups/<name>/ inside it, then keeps only the newest
// `keep` backups (0 keeps all). The caller has saved first and does not save during the copy,
// which is what makes it consistent. `made` is the folder written. A write that never
// finished (.tmp) is not copied.
bool BackupWorld(const std::string& worldDir, const std::string& name, int keep, std::string& made,
                 std::string& error);

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

// --- Stopping ------------------------------------------------------------------------------

// Ctrl+C, SIGTERM (systemd's stop), SIGHUP (the terminal closed) and, on Windows, Ctrl+Break
// and closing the console window ask the server to stop: it saves, says goodbye to everyone
// and exits 0. A second Ctrl+C while it does that ends it at once.
void InstallStopHandlers();
bool StopRequested();
void RequestStop();
// Clears a request, for a server started again in the same process (the self-test).
void ClearStop();
// Called once everything is saved. Windows gives a closing console a few seconds and then
// ends the process: the handler waits for this so that the save is not cut short.
void MarkStopped();

}  // namespace Host
