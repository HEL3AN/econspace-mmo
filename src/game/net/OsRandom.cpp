#include "net/OsRandom.h"

#if defined(_WIN32)
#include <windows.h>
// After windows.h, which it needs.
#include <bcrypt.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <sys/random.h>
#include <sys/types.h>
#else
#include <cstdlib>  // arc4random_buf: macOS and the BSDs
#endif

namespace Net
{

bool OsRandom(void* out, size_t size)
{
    if (size == 0)
        return true;
#if defined(_WIN32)
    // The system-preferred generator needs no provider handle to be opened and closed.
    unsigned char* p = static_cast<unsigned char*>(out);
    while (size > 0)
    {
        const ULONG chunk = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (BCryptGenRandom(nullptr, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return false;
        p += chunk;
        size -= chunk;
    }
    return true;
#elif defined(__linux__)
    // getrandom blocks only until the pool is first seeded at boot, and returns short reads
    // past 256 bytes or when a signal arrives, so it is asked until it has given everything.
    unsigned char* p = static_cast<unsigned char*>(out);
    size_t         left = size;
    while (left > 0)
    {
        const ssize_t n = getrandom(p, left, 0);
        if (n > 0)
        {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == ENOSYS)
            break;  // a kernel older than 3.17: the device below
        return false;
    }
    if (left == 0)
        return true;
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr)
        return false;
    const size_t got = std::fread(p, 1, left, f);
    std::fclose(f);
    return got == left;
#else
    arc4random_buf(out, size);
    return true;
#endif
}

}  // namespace Net
