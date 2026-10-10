#include "render/Perf.h"

#include "raylib.h"
#include "rlgl.h"
#include <chrono>
#include <cstdio>

// raylib's window is GLFW's, and GLFW hands out GL entry points. The timer-query calls are
// core in OpenGL 3.3 -- the version raylib asks for -- but raylib does not wrap them.
extern "C" void* glfwGetProcAddress(const char* name);

namespace Render
{
namespace Perf
{
namespace
{
#if defined(_WIN32) && !defined(_WIN64)
#define PERF_GLAPI __stdcall
#else
#define PERF_GLAPI
#endif
using GenQueries = void(PERF_GLAPI*)(int, unsigned*);
using QueryCounter = void(PERF_GLAPI*)(unsigned, unsigned);
using GetQueryObjectui64v = void(PERF_GLAPI*)(unsigned, unsigned, unsigned long long*);
constexpr unsigned GL_TIMESTAMP_ = 0x8E28;
constexpr unsigned GL_QUERY_RESULT_ = 0x8866;

// A frame's GPU marks. The results are read back a few frames later, when the GPU has long
// since passed them, so reading never waits.
constexpr int FRAMES_IN_FLIGHT = 4;
constexpr int MAX_MARKS = 32;
struct GpuFrame
{
    unsigned queries[MAX_MARKS] = {};
    int      phase[MAX_MARKS] = {};  // which phase the gap *ending* at this mark belongs to
    int      used = 0;
    bool     pending = false;
};

bool g_enabled = false;

double g_frameStart = 0.0;
double g_lastMark = 0.0;

bool                g_gpuTried = false;
bool                g_gpu = false;
GenQueries          g_genQueries = nullptr;
QueryCounter        g_queryCounter = nullptr;
GetQueryObjectui64v g_getQuery = nullptr;
GpuFrame            g_gpuFrames[FRAMES_IN_FLIGHT];
int                 g_gpuSlot = 0;

// The window being collected, and the last one closed.
Summary g_sum;
Summary g_last;

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void LoadGpu()
{
    if (g_gpuTried)
        return;
    g_gpuTried = true;
    if (!IsWindowReady())
        return;
    g_genQueries = (GenQueries)glfwGetProcAddress("glGenQueries");
    g_queryCounter = (QueryCounter)glfwGetProcAddress("glQueryCounter");
    g_getQuery = (GetQueryObjectui64v)glfwGetProcAddress("glGetQueryObjectui64v");
    g_gpu = g_genQueries != nullptr && g_queryCounter != nullptr && g_getQuery != nullptr;
    if (!g_gpu)
        return;
    for (GpuFrame& f : g_gpuFrames)
        g_genQueries(MAX_MARKS, f.queries);
}

// Stamps where the GPU is in the stream of work, once raylib has sent it what it batched.
void GpuMark(int phase)
{
    if (!g_gpu)
        return;
    GpuFrame& f = g_gpuFrames[g_gpuSlot];
    if (f.used >= MAX_MARKS)
        return;
    rlDrawRenderBatchActive();
    g_queryCounter(f.queries[f.used], GL_TIMESTAMP_);
    f.phase[f.used] = phase;
    f.used++;
}

// The frame that used this slot last time round: its marks are four frames old.
void GpuCollect(GpuFrame& f)
{
    if (!f.pending)
        return;
    f.pending = false;
    unsigned long long prev = 0;
    for (int i = 0; i < f.used; i++)
    {
        unsigned long long t = 0;
        g_getQuery(f.queries[i], GL_QUERY_RESULT_, &t);
        if (i > 0 && f.phase[i] >= 0 && t >= prev)
            g_sum.gpuMs[f.phase[i]] += (double)(t - prev) / 1.0e6;
        prev = t;
    }
    g_sum.gpuFrames++;
}
}  // namespace

const char* PhaseName(Phase p)
{
    switch (p)
    {
        case Phase::Update: return "update";
        case Phase::Sky: return "sky";
        case Phase::World: return "world";
        case Phase::Overlay: return "overlay";
        case Phase::Treatment: return "treatment";
        case Phase::Hud: return "hud";
        case Phase::Present: return "present";
        case Phase::Count: break;
    }
    return "?";
}

const char* SectionName(Section s)
{
    switch (s)
    {
        case Section::Compose: return "compose";
        case Section::Soft: return "soft";
        case Section::Surface: return "surface";
        case Section::Count: break;
    }
    return "?";
}

const char* CounterName(Counter c)
{
    switch (c)
    {
        case Counter::Items: return "items";
        case Counter::Culled: return "culled";
        case Counter::Pieces: return "pieces";
        case Counter::Skipped: return "skipped";
        case Counter::Binds: return "binds";
        case Counter::Soft: return "soft";
        case Counter::Surface: return "surface";
        case Counter::Passes: return "passes";
        case Counter::CacheMisses: return "expanded";
        case Counter::Count: break;
    }
    return "?";
}

void Enable(bool on)
{
    g_enabled = on;
}

bool Enabled()
{
    return g_enabled;
}

void BeginFrame()
{
    if (!g_enabled)
        return;
    LoadGpu();
    if (g_gpu)
    {
        g_gpuSlot = (g_gpuSlot + 1) % FRAMES_IN_FLIGHT;
        GpuFrame& f = g_gpuFrames[g_gpuSlot];
        GpuCollect(f);
        f.used = 0;
        GpuMark(-1);  // the start: no gap ends here
    }
    g_frameStart = g_lastMark = NowMs();
}

void Mark(Phase p)
{
    if (!g_enabled)
        return;
    GpuMark((int)p);
    const double now = NowMs();
    g_sum.phaseMs[(int)p] += now - g_lastMark;
    g_lastMark = now;
}

void EndFrame()
{
    if (!g_enabled)
        return;
    g_sum.frameMs += NowMs() - g_frameStart;
    g_sum.frames++;
    if (g_gpu)
        g_gpuFrames[g_gpuSlot].pending = true;
}

void Count(Counter c, int n)
{
    if (g_enabled)
        g_sum.counts[(int)c] += n;
}

Scope::Scope(Section s) : s_(s), start_(g_enabled ? NowMs() : 0.0)
{
}

Scope::~Scope()
{
    if (g_enabled)
        g_sum.sectionMs[(int)s_] += NowMs() - start_;
}

const Summary& Last()
{
    return g_last;
}

void Roll()
{
    Summary   avg;
    const int n = g_sum.frames;
    avg.frames = n;
    avg.gpu = g_gpu;
    if (n > 0)
    {
        avg.frameMs = g_sum.frameMs / n;
        for (int i = 0; i < (int)Phase::Count; i++)
            avg.phaseMs[i] = g_sum.phaseMs[i] / n;
        for (int i = 0; i < (int)Section::Count; i++)
            avg.sectionMs[i] = g_sum.sectionMs[i] / n;
        for (int i = 0; i < (int)Counter::Count; i++)
            avg.counts[i] = g_sum.counts[i] / n;
    }
    if (g_sum.gpuFrames > 0)
        for (int i = 0; i < (int)Phase::Count; i++)
            avg.gpuMs[i] = g_sum.gpuMs[i] / g_sum.gpuFrames;
    g_last = avg;
    g_sum = Summary{};
}

std::string Report(const Summary& s)
{
    char        buf[160];
    std::string out;
    std::snprintf(buf, sizeof buf, "frame %.2f ms (%.0f fps) over %d frames\n", s.frameMs,
                  s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0, s.frames);
    out += buf;
    out += s.gpu ? "  phase         cpu ms   gpu ms\n" : "  phase         cpu ms\n";
    for (int i = 0; i < (int)Phase::Count; i++)
    {
        if (s.gpu)
            std::snprintf(buf, sizeof buf, "  %-10s %9.2f %8.2f\n", PhaseName((Phase)i),
                          s.phaseMs[i], s.gpuMs[i]);
        else
            std::snprintf(buf, sizeof buf, "  %-10s %9.2f\n", PhaseName((Phase)i), s.phaseMs[i]);
        out += buf;
    }
    for (int i = 0; i < (int)Section::Count; i++)
    {
        std::snprintf(buf, sizeof buf, "    %-8s %7.2f  (cpu, inside world)\n",
                      SectionName((Section)i), s.sectionMs[i]);
        out += buf;
    }
    out += " ";
    for (int i = 0; i < (int)Counter::Count; i++)
    {
        std::snprintf(buf, sizeof buf, " %s %.0f", CounterName((Counter)i), s.counts[i]);
        out += buf;
    }
    out += "\n";
    return out;
}

}  // namespace Perf
}  // namespace Render
