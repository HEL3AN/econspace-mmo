// Test entry point. The tests live in *_tests.cpp.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

// Declared by hand rather than by including raylib.h.
//
// doctest's implementation pulls in <windows.h>, and raylib collides with it in both
// directions: raylib declares a Rectangle type where the Win32 headers declare a
// Rectangle function, plus CloseWindow and ShowCursor. Whichever header goes first, the
// other fails to compile. One extern "C" declaration sidesteps the whole argument.
extern "C" void      SetTraceLogLevel(int logLevel);
static constexpr int RAYLIB_LOG_NONE = 7;  // TraceLogLevel::LOG_NONE

int main(int argc, char** argv)
{
    // Silence raylib. Loading the galaxy logs a line per fixture, which buries a failure
    // in a wall of "systems in galaxy: 3" and makes a passing run look busy.
    SetTraceLogLevel(RAYLIB_LOG_NONE);
    return doctest::Context(argc, argv).run();
}
