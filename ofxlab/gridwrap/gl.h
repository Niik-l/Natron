#ifndef GW_GL_H
#define GW_GL_H

// ---------------------------------------------------------------------------
// The overlay's GL draw calls, reached the same way ZDefocusZ reaches an OpenCL device: `dlopen` +
// `dlsym` against a hand-declared subset of the client GL API, so this plugin links no GL library and
// a host that draws its overlays with something else still loads it (D-78, and the precedent is
// D-51's gpu.cpp).
//
// Why the loader and not a `-lGL` link line: rule 2 keeps the only *linked* dependency the vendored
// SDK, and an OFX overlay is drawn inside whatever GL context the host already has, so the plugin needs
// the *functions*, not a library it initialises. Natron, Nuke and Resolve all ship a legacy immediate-
// mode GL context for overlays (Natron's is `Gui/ViewerGL.cpp`'s paint, which calls
// `OfxInteract::draw` inside a `GL_protectAttribs` block), and every one of them has `libGL.so.1`
// loaded in the process already — `dlopen` of a name already present is a refcount bump, not a load.
//
// What is declared here is only the five entry points `draw()` issues, with the ABI of the SGI/GLX
// headers they come from: `GLenum` is `unsigned int`, `GLdouble` is `double`, `GLfloat` is `float`, and
// the mode constants carry their published values, because a wrong *constant* in a hand-declared ABI is
// the failure mode this pattern carries — it compiles, it links, and it draws the wrong primitive. A
// wrong *prototype* would be worse, which is why the signatures are spelled out in full rather than
// taken from a `typedef`. Unlike ZDefocusZ's OpenCL ABI (D-51), which `tests/gpu_probe.sh` re-checks
// against a vendor header, this one has no such gate: there is no host-free way to call a GL function,
// so the published values are transcribed and the arbiter is the Viewer picture (D-78).
//
// No GL state is saved or restored, and none is touched beyond those five: `glGetError` and `glEnable`
// are deliberately absent, because the host's `GL_protectAttribs` is what restores what the overlay
// changed. That is also what bounds what the overlay may draw: Natron protects `GL_COLOR_BUFFER_BIT |
// GL_LINE_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT` (`Gui/ViewerGL.cpp:733-734`), and a mask without
// `GL_POINT_BIT` means a `glPointSize` the overlay set would survive into the host's own drawing, so
// that entry point is not loaded at all and every marker the lattice draws is a stroke of lines (D-78).
// The values the loader reports are the whole truth about whether the overlay can draw at all;
// if the load fails, `draw()` returns false and the lattice is edited through its knobs (D-53), which is
// what the plugin did before this file existed.
// ---------------------------------------------------------------------------

#include <string>

#include "grid.h"

namespace gw {
namespace gl {

// The primitive modes `begin`/`end` accept, with the published GL enum values (Khronos gl.h: GL_LINES
// 0x0001, GL_LINE_LOOP 0x0002, GL_LINE_STRIP 0x0003). `kLineLoop` is the basis rect — a closed stroke —
// and `kLineStrip` a grid line, which is open at both ends. GL_POINTS (0x0000) is not in this list
// because the only mode that needs `glPointSize`, which the host does not restore for an overlay; every
// marker the lattice draws is a cross of `kLines` instead (see the note above, D-78).
enum PrimType : unsigned int {
    kLines     = 0x0001u,
    kLineLoop  = 0x0002u,
    kLineStrip = 0x0003u
};

// The loaded subset. Every slot is non-null when `ok` is true; `missing` says which name could not be
// found when it is false, because the reason a host has no overlay drawing is worth printing once.
struct Loader {
    bool ok;
    std::string library;    // the soname that worked, or the one that refused to load
    std::string missing;    // the entry point that was absent

    void (*lineWidth)(float);
    void (*color4f)(float, float, float, float);
    void (*begin)(unsigned int);
    void (*end)();
    void (*vertex2d)(double, double);

    Loader()
        : ok(false), library(), missing(), lineWidth(0), color4f(0),
          begin(0), end(0), vertex2d(0) {}
};

// The loader, resolved on first use and cached: a probe that ran `dlopen` per draw call would be a
// syscall per stroke.
const Loader& loader();

// True when every entry point is present, i.e. `draw()` below would actually reach the driver.
bool available();

// One sentence for the plugin's log: which library was tried and what it lacked. Empty when available.
std::string refusalReason();

// The guarded calls. Each is a no-op when the loader failed, so a shell can issue a whole frame
// without checking anything: an overlay that cannot draw draws nothing, and `draw()` reports the refusal
// once through `refusalReason`.
void begin(PrimType mode);
void end();
void vertex(const Vec& p);
void color(double r, double g, double b, double a);
void lineWidth(double w);

} // namespace gl
} // namespace gw

#endif // GW_GL_H
