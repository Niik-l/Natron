// SplatCore: 3D Gaussian splat renderer core (wgpu).
//
// Natron-independent: depends only on wgpu-native. The Natron node (SplatRender)
// is a thin wrapper around this class; it can also be driven from a small test
// program (complex_tools/gsplats/tests/splat_core_test.cpp).
//
// How a frame is made:
//   1. upload(): once per scene. Each splat's 3D covariance is built from its
//      scale and rotation, and positions, covariances and colour coefficients
//      go to GPU storage buffers.
//   2. draw(): per frame. Splats are sorted far to near on the CPU (two-pass
//      radix sort on quantised view depth) and the order goes up as one index
//      buffer. One instanced draw then renders a screen-space quad per splat:
//      the vertex shader projects the covariance to a 2D ellipse and evaluates
//      the spherical-harmonic colour for the view direction, the fragment
//      shader applies the Gaussian falloff, and the GPU blends "over" in order.
//      A second target accumulates depth the same way.
//   3. The two float targets are read back.
//
// The picture follows the reference 3DGS rasteriser: 3 sigma footprint, 0.3 px
// low-pass on the projected covariance, alpha clamped to 0.99, contributions
// below 1/255 dropped. Blending is back to front, which gives the same sum as
// the reference's front-to-back accumulation.
//
// Output is PREMULTIPLIED, in the colourspace of the splat colours (the
// training photographs, normally sRGB). No background.

#ifndef SPLAT_CORE_H
#define SPLAT_CORE_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace SplatCore {

// A splat scene as plain arrays (see Engine/Dev/Scene3D/SplatData.h for the
// conventions: linear scales, normalised quaternion w x y z, opacity 0..1, SH
// band 0 in dc and bands 1..shDegree in shRest as R.. G.. B.. per splat).
struct SceneDesc {
    std::size_t count = 0;
    const float* pos = nullptr;       // 3 per splat
    const float* scale = nullptr;     // 3 per splat
    const float* rot = nullptr;       // 4 per splat
    const float* opacity = nullptr;   // 1 per splat
    const float* dc = nullptr;        // 3 per splat
    const float* shRest = nullptr;    // 3 * ((shDegree + 1)^2 - 1) per splat, may be null when shDegree == 0
    int shDegree = 0;                 // 0..3
};

struct DrawParams {
    float view[16];             // world -> camera, column-major; the camera looks down -Z, +Y up
    float model[16];            // splat file space -> world, column-major (rotation, uniform scale, mirror)
    float projX = 1.f;          // projection [0][0] = focal / (horizontal aperture / 2)
    float projY = 1.f;          // projection [1][1] = focal / (vertical aperture / 2)
    float nearClip = 0.1f;      // splats whose centre is nearer than this are not drawn
    float splatScale = 1.f;     // multiplies every splat's size (1 = as trained)
    float opacityScale = 1.f;   // multiplies every splat's opacity
    int shDegree = 3;           // highest SH band to use; limited to what was uploaded
    bool antialias = true;      // the 0.3 px low-pass of the reference renderer
};

// Per-pixel layers, row 0 = TOP of the image.
struct RenderOutput {
    int width = 0, height = 0;
    std::vector<float> rgba;    // width*height*4: premultiplied colour + alpha
    std::vector<float> depth;   // width*height:   mean distance along the view axis of what is visible
                                //                 (world units), 0 where alpha is 0
    std::size_t drawn = 0;      // splats in front of the near plane, sorted and drawn
    double sortMs = 0., gpuMs = 0.;
};

class Renderer {
public:
    Renderer();                 // initializes wgpu instance/adapter/device once
    ~Renderer();

    bool valid() const;
    const std::string& lastError() const;
    std::string adapterName() const;
    bool float32Targets() const;   // true: 32-bit float render targets; false: 16-bit (GPU cannot blend 32-bit)

    // Bytes of GPU buffers a scene needs (to tell the user before trying).
    static std::size_t gpuBytesFor(std::size_t count, int shDegree);

    // Build and upload the scene. Returns false (see lastError) when the GPU
    // cannot hold it. A successful upload replaces the previous scene.
    bool upload(const SceneDesc& scene);
    bool hasScene() const;
    std::size_t sceneCount() const;

    // Render the uploaded scene. `pos` must be the same positions that were
    // uploaded (3 per splat): the depth sort runs on the CPU and the core keeps
    // no copy of them.
    bool draw(const float* pos, const DrawParams& params, int width, int height, RenderOutput& out);

private:
    struct Impl;
    std::unique_ptr<Impl> _p;
};

}  // namespace SplatCore

#endif
