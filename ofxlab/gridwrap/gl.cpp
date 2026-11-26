#include "gl.h"

#include <dlfcn.h>

namespace gw {
namespace gl {
namespace {

// The names tried, in order. `libGL.so.1` is the soname a host has already loaded (an ICD-linked
// OpenGL 1.x-compatible library on every Linux distro that ships a GL host); `libGL.so` is the linker
// name, which is a symlink and is what a distro with only the development package exposes. Nothing
// else is tried: a host that has no GL at all is a host that does not implement
// `kOfxPropSupportsOverlays` with a GL context, and a second probe of a software fallback would be a
// slower way of reaching the same refusal.
#ifdef _WIN32
const char kLibraryNames[][13] = { "opengl32.dll", "opengl32.dll" };
#else
const char kLibraryNames[][13] = { "libGL.so.1", "libGL.so" };
#endif
const int kNumLibraries = 2;

// The entry points, and the slot each one lands in. Spelled as raw function-pointer types rather than
// pulled from `<GL/gl.h>` — that header is the thing rule 2 forbids here.
typedef void (*FnLineWidth)(float);
typedef void (*FnColor4f)(float, float, float, float);
typedef void (*FnBegin)(unsigned int);
typedef void (*FnEnd)();
typedef void (*FnVertex2d)(double, double);

const char kEntryNames[][12] = { "glLineWidth", "glColor4f", "glBegin", "glEnd", "glVertex2d" };
const int kNumEntries = 5;

Loader loadGL()
{
    Loader out;
    for (int li = 0; li < kNumLibraries; ++li) {
        const char* name = kLibraryNames[li];
        void* h = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
        if (!h)
            continue;
        out.library = name;

        // Resolve everything before declaring success: half a subset is worse than none, because a
        // missing glEnd leaves the host's GL in the middle of a primitive list.
        void* syms[kNumEntries];
        bool complete = true;
        for (int i = 0; i < kNumEntries; ++i) {
            syms[i] = dlsym(h, kEntryNames[i]);
            if (!syms[i]) {
                out.missing = kEntryNames[i];
                complete = false;
                break;
            }
        }
        if (!complete) {
            // Keep probing: the next soname may be the one that carries all five.
            dlclose(h);
            out.library.clear();
            continue;
        }

        out.lineWidth = reinterpret_cast<FnLineWidth>(syms[0]);
        out.color4f = reinterpret_cast<FnColor4f>(syms[1]);
        out.begin = reinterpret_cast<FnBegin>(syms[2]);
        out.end = reinterpret_cast<FnEnd>(syms[3]);
        out.vertex2d = reinterpret_cast<FnVertex2d>(syms[4]);
        out.ok = true;
        out.missing.clear();
        // The handle is deliberately leaked: the functions stay valid as long as the host's own GL is
        // loaded, and unloading an ICD a host is still using is not this plugin's business (the same
        // choice gpu.cpp makes for a device context it does not own).
        return out;
    }

    if (out.library.empty())
        out.library = kLibraryNames[kNumLibraries - 1];
    out.missing = out.missing.empty() ? "no loadable GL library" : out.missing;
    return out;
}

} // namespace

const Loader& loader()
{
    // One resolution per process. A C++11 function-local static is initialised exactly once even if
    // two threads race to it, which matters because a host may describe the overlay on one thread and
    // draw on another.
    static const Loader loaded = loadGL();
    return loaded;
}

bool available()
{
    return loader().ok;
}

std::string refusalReason()
{
    const Loader& l = loader();
    if (l.ok)
        return std::string();
    return std::string("GL overlay: could not load ") + l.library + " (" + l.missing +
           "), the lattice is edited through its knobs";
}

void begin(PrimType mode)
{
    const Loader* l = &loader();
    if (l->ok)
        l->begin(static_cast<unsigned int>(mode));
}

void end()
{
    const Loader* l = &loader();
    if (l->ok)
        l->end();
}

void vertex(const Vec& p)
{
    const Loader* l = &loader();
    if (l->ok)
        l->vertex2d(p.x, p.y);
}

void color(double r, double g, double b, double a)
{
    const Loader* l = &loader();
    if (l->ok)
        l->color4f(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b),
                   static_cast<float>(a));
}

void lineWidth(double w)
{
    const Loader* l = &loader();
    if (l->ok && w > 0.0)
        l->lineWidth(static_cast<float>(w));
}

} // namespace gl
} // namespace gw
