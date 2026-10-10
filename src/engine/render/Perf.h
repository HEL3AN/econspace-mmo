#pragma once

#include <string>

// What a frame costs, and where (#296).
//
// A frame went from fine to ten a second on a laptop without anyone being able to say why,
// because nothing measured it. This does: the frame is cut into phases at points the caller
// marks, the backend counts what it drew, and the result is a line on stderr (`--perf`) or a
// panel on screen (F9).
//
// Three kinds of time, and they answer different questions:
//
// - A **phase** runs from one Mark to the next, on the CPU: how long the game spent
//   building and handing over that part of the frame.
// - The same phase on the **GPU**: a timestamp query at every Mark, read back a few frames
//   later, and the gap between two of them. It is the only way a treatment pass -- nothing
//   on the CPU, possibly everything on the card -- shows up at all. When the GPU is idle,
//   waiting for the CPU, the gap includes the wait, so a phase that is cheap on both reads
//   as its CPU time. (Waiting for the GPU at every mark instead, with glFinish, was tried
//   first: on an Intel UHD 620 every wait cost ~30 ms of its own.)
// - A **section** (composition, soft glows, surface cuts) is CPU time inside a phase,
//   summed over every object.
//
// Off, every call is a test of one bool. Nothing here changes what is drawn.
namespace Render
{
namespace Perf
{
enum class Phase
{
    Update,     // input, prediction, the snapshot: everything before drawing starts
    Sky,        // the starfield
    World,      // every object: composing it and handing it to the GPU
    Overlay,    // what is drawn over the world in world space: rings, beams, markers
    Treatment,  // the screen treatment's passes, and the final copy to the screen
    Hud,        // windows, overview, radar
    Present,    // EndDrawing: the swap, and whatever the GPU still had to do
    Count
};

enum class Section
{
    Compose,  // Compose(): sections, kits and modules expanded, pieces placed
    Soft,     // glows: gradient discs and soft surface fans
    Surface,  // marks on a sphere, cut to the body's disc
    Count
};

enum class Counter
{
    Items,        // handed to the backend
    Culled,       // ...of which wholly off screen, and not drawn
    Pieces,       // composed pieces drawn
    Skipped,      // composed pieces too small to see, and not drawn
    Binds,        // a material bound: each one is a draw call of its own
    Soft,         // soft glows drawn
    Surface,      // surface pieces cut to their body
    Passes,       // treatment passes run (each one a full-screen fill)
    CacheMisses,  // shapes expanded from scratch rather than reused
    Count
};

const char* PhaseName(Phase p);
const char* SectionName(Section s);
const char* CounterName(Counter c);

// Measuring at all. Off by default; F9 and `--perf` turn it on.
void Enable(bool on);
bool Enabled();

void BeginFrame();
// The time since the last Mark (or BeginFrame) belongs to `p`. Phases may be marked more
// than once a frame; they add up.
void Mark(Phase p);
void EndFrame();

void Count(Counter c, int n = 1);

// CPU time inside a phase, summed.
class Scope
{
public:
    explicit Scope(Section s);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Section s_;
    double  start_;
};

// The averages over the last completed window of frames (a second's worth, or whatever
// `--perf` printed over), in milliseconds and per frame.
struct Summary
{
    int    frames = 0;
    double frameMs = 0.0;  // BeginFrame to EndFrame
    double phaseMs[(int)Phase::Count] = {};
    double gpuMs[(int)Phase::Count] = {};
    int    gpuFrames = 0;  // frames whose GPU marks were read back
    bool   gpu = false;    // whether this driver gave timer queries at all
    double sectionMs[(int)Section::Count] = {};
    double counts[(int)Counter::Count] = {};
};
const Summary& Last();

// Closes the current window: what Last() returns from now on is the average since the
// previous call. Called by whoever reports, once a second or once at the end of a run.
void Roll();

// One line per phase and per counter, for stderr or a panel.
std::string Report(const Summary& s);

}  // namespace Perf
}  // namespace Render
