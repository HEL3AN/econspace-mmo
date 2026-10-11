#pragma once

#include <string>

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

}  // namespace Save
