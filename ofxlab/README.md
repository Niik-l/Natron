# OFX Lab plugins

Five OpenFX plugins by **Draephus**, included in this fork's builds with the author's permission.
They appear in Natron under the **OFX Lab** group and are **experimental**.

| Plugin | ID | What it does |
|---|---|---|
| GridWrap | `net.ofxlab.gridwrap` | Bézier grid warp and morph with source and destination grids, edited in the viewer |
| UVGen | `net.ofxlab.uvgen` | UV ramp generator (red = x/width, green = y/height) |
| SharpenZ | `net.ofxlab.sharpenz` | Edge-aware sharpen (port of the `nt_sharpen` gizmo) |
| TimeWarp | `net.ofxlab.timewarp` | Retiming with frame blending or optical-flow motion interpolation |
| ZDefocusZ | `net.ofxlab.zdefocusz` | Depth-based layered defocus with bokeh shapes |

## Building

    ofxlab/build.sh <openfx checkout> <output dir>

The plugins are written against the upstream ASWF OpenFX Support library, not the copy under
`libs/OpenFX`. The release pipelines clone it at `OFXLAB_OPENFX_REF` from
`tools/win-build/config.sh` and install the results next to the other OFX bundles.

## Changes made in this fork

The sources are the author's, with these changes:

- **Render scale**: every plugin fails the region-of-definition and render actions at a render
  scale other than 1, so the host renders at full size and scales the result. Without this the
  picture was wrong in proxy mode and in a zoomed-out viewer.
- **Windows**: `gridwrap/gl.cpp` and `zdefocusz/gpu.cpp` pick the OpenGL / OpenCL library name
  per platform, and `common/win/dlfcn.h` maps the three `dl*` calls onto `LoadLibrary`.
- **TimeWarp**: fixed an out-of-bounds write in the pass-through copy.
- **ZDefocusZ**: Depth Channel defaults to `r` (it read alpha, so the node did nothing at its
  defaults); a render can be cancelled between bands.
- **GridWrap**: the overlay ignores clicks when OpenGL could not be loaded.
- **UVGen**: asks the host for none of its input's pixels.
- **SharpenZ**: non-finite pixels no longer disable the pre-blur over whole bands.
- `common/ofxlab_channels.h` and `common/ofxlab_maskmix.h` are **stand-ins written for this
  build**: the author's own shared headers were not part of what was sent. The Channels knob may
  behave slightly differently from the author's builds.

## Known limits

- Pixel aspect ratio is ignored by all five.
- TimeWarp keeps its motion estimate after an upstream change until the node's cache is cleared,
  and its Motion mode can differ between a zoomed-in view and a full render. The built-in
  **TimeFlow** node is the tested retimer.
- ZDefocusZ renders on one thread, and pixels with alpha 0 come out black once anything is
  blurred. Connect a depth pass with Z in red to the Depth input and set Focus Distance to a
  depth value from the scene.
- GridWrap stores its grid as 640 hidden position knobs.
