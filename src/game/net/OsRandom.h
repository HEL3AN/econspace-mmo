#pragma once

#include <cstddef>

// Bytes from the operating system's random generator (#351): BCryptGenRandom on Windows,
// getrandom (or /dev/urandom) on Linux, arc4random_buf on macOS and the BSDs.
//
// This is what a nonce and a salt are made from. std::random_device was asked before, mixed
// with the clock, and fed to mt19937_64: on older MinGW the device is a fixed sequence, the
// clock is guessable to the millisecond, and mt19937's output gives its state away after a
// few hundred values -- so a nonce could be predicted, and a predicted nonce makes a proof
// replayable. The platform headers stay in the .cpp, like the sockets' (#12).
namespace Net
{

// Fills `out` with `size` random bytes. False only if the system could not give them; the
// caller must not then fall back to something weaker.
bool OsRandom(void* out, size_t size);

}  // namespace Net
