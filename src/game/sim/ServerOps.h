#pragma once

#include "core/AdminChannel.h"  // the admin channel (#400), in the engine for the editor

#include <string>
#include <vector>

// Running a server as a service (#400): copies of its world taken while it runs, a way to give
// it console commands when nobody is at its console (core/AdminChannel.h), and a clean stop on
// a signal.
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
