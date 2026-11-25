#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// Writing a save so that a crash can never leave half of one behind (#341).
//
// A save written in place is truncated the moment the file is opened, and a server that dies
// during the write leaves a file that ends mid-object. Before this, the next boot read such a
// world as "no world", started a new galaxy and wrote it over the old one a minute later; and
// a truncated account looked like a free name to whoever logged in under it first.
//
// So a save is written beside the real file, flushed, and only then renamed over it: the
// rename is atomic on both platforms, and the real file is at any moment either the old save
// or the new one, never a mixture. The file it replaces is kept as `.bak1`, and older copies
// shift down to `.bakN` -- a save that is whole but wrong (a bug, not a crash) can still be
// undone by hand.
namespace Save
{

// Writes `contents` to `path` atomically, keeping `backups` older copies (0 keeps none).
// False if anything failed; the file at `path` is then exactly what it was before.
bool WriteFileAtomic(const std::string& path, const std::string& contents, int backups);

// The name of the n-th backup of `path` (n >= 1): `path.bak1`, `path.bak2`, ...
std::string BackupPath(const std::string& path, int n);

// Writes saves on a thread of its own (#350), so a checkpoint costs the simulation's tick the
// building of what is saved and not the text and the disk.
//
// A job is a path and a function that makes its text: the function owns a copy of what is
// saved (the caller built it on its own thread and moved it in), so nothing the simulation
// goes on to change is read here. Rules:
//   - one write at a time, in submission order of the paths;
//   - a job submitted for a path that already has one waiting *replaces* it: the newer save is
//     the whole state, and the older was never going to be the file;
//   - a job already running is never interrupted, and one submitted meanwhile waits behind it;
//   - Wait() returns once nothing is running or waiting, and the destructor does the same, so a
//     stopping server's last saves reach the disk;
//   - every write is WriteFileAtomic, so a failure leaves the old file as it was.
class BackgroundWriter
{
public:
    BackgroundWriter();
    ~BackgroundWriter();
    BackgroundWriter(const BackgroundWriter&) = delete;
    BackgroundWriter& operator=(const BackgroundWriter&) = delete;

    void Submit(const std::string& path, std::function<std::string()> text, int backups);
    // Blocks until everything submitted so far is on disk or has failed.
    void Wait();
    // Writes that failed since the last call, with the path of the latest. For the caller to
    // say so in its log: this thread writes nothing there itself.
    int TakeFailures(std::string* lastPath = nullptr);
    // Jobs that were replaced before they ran, and writes done, since construction (a test's
    // and the server's metrics' to read).
    int Coalesced() const;
    int Written() const;

private:
    struct Job
    {
        std::function<std::string()> text;
        int                          backups = 0;
    };
    void Run();

    mutable std::mutex         m_;
    std::condition_variable    wake_, idle_;
    std::map<std::string, Job> waiting_;
    bool                       busy_ = false;
    bool                       stop_ = false;
    int                        failures_ = 0, coalesced_ = 0, written_ = 0;
    std::string                failedPath_;
    std::thread                worker_;  // last: started once the rest exists
};

}  // namespace Save
