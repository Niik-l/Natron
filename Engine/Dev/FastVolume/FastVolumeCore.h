// FastVolumeCore — real-time compressed-VDB volume renderer core.
//
// Natron-independent: depends only on OpenVDB and wgpu-native.
// The Natron node (FastVolumeRender) is a thin wrapper around this class;
// it can also be driven from a CLI (see test_core.cpp).
//
// Pipeline per render() call:
//   1. compress grids to 8^3 bricks (leaf-aligned, 8-bit per-brick quantized)
//   2. upload to GPU storage buffers
//   3. pass A: 1/N-res sun-transmittance light grid (compute)
//   4. pass B: ray march in volume index space; outputs beauty + AOVs
//   5. read back float buffers
//
// Output is LINEAR PREMULTIPLIED. No tonemap, no background.

#ifndef FASTVOLUME_CORE_H
#define FASTVOLUME_CORE_H

#include <memory>
#include <string>
#include <vector>

#include <openvdb/openvdb.h>

namespace FastVolume {

struct CameraParams {
    // Maya/Blender convention: extrinsic XYZ rotation in degrees,
    // camera looks down local -Z. Focal & apertures in mm.
    double tx = 0, ty = 0, tz = 0;
    double rx = 0, ry = 0, rz = 0;
    double focal = 50.0;
    double hAperture = 24.576;
    double vAperture = 18.672;
    bool valid = false;   // false -> auto-frame the volume bbox
};

// One illuminating light. Each casts self-shadows via its own downsampled
// transmittance grid.
//   AREA = a parallel rectangular BEAM along the light's normal — its
//   Width/Height define the beam cross-section (use a long thin rectangle for a
//   laser / light shaft). Only smoke inside the rectangle is lit.
enum LightKind { LIGHT_POINT = 0, LIGHT_DISTANT = 1, LIGHT_SPOT = 2, LIGHT_AREA = 3 };
static const int MAX_LIGHTS = 8;

struct LightDesc {
    int type = LIGHT_DISTANT;
    float pos[3] = {0, 0, 0};        // world-space position (point / spot / area)
    float dir[3] = {0, 0, 1};        // world: DISTANT/AREA = direction TO the light
                                     //        SPOT          = shine axis (toward scene)
    float color[3] = {1, 1, 1};      // color * intensity * 2^exposure (pre-scaled)
    float cosInner = 0.96f;          // spot cone: full intensity inside this cos(half-angle)
    float cosOuter = 0.86f;          // spot cone: zero outside this cos(half-angle)
    // AREA beam cross-section (world space): rectangle axes + half extents.
    float uax[3] = {1, 0, 0};        // beam width axis (light local X)
    float vax[3] = {0, 1, 0};        // beam height axis (light local Y)
    float halfU = 1.0f;              // half Width  (world units)
    float halfV = 1.0f;              // half Height (world units)
};

struct LookParams {
    float sigma = 0.55f;          // extinction scale
    float albedo = 0.72f;
    // Lights. numLights < 0 means "use the single sun* fields below as one
    // distant light" (CLI / test_core default). numLights == 0 means genuinely
    // no lights (volume renders unlit — just emission/ambient).
    int numLights = -1;
    LightDesc lights[MAX_LIGHTS];
    float sunDir[3] = {0.5588f, 0.7265f, 0.3912f};   // TO the sun, world space
    float sunColor[3] = {1.0f, 0.96f, 0.9f};
    float sunIntensity = 18.0f;
    float ambient[3] = {0.16f, 0.2f, 0.28f};
    float hgG = 0.5f;             // Henyey-Greenstein anisotropy
    float fireK = 2.2f;           // emission strength (flames glow to camera)
    float fireMax = 3.0f;         // flames value at ramp top
    float fireLight = 1.0f;       // how much the fire ILLUMINATES nearby smoke (0 = off)
    int steps = 1024;             // march steps across volume bbox
    int lightFactor = 4;          // light grid downsample (voxels per cell)
};

// Per-pixel layers, each width*height*4 floats, row 0 = TOP of image.
struct RenderOutput {
    int width = 0, height = 0;
    std::vector<float> beauty;      // RGB radiance, A coverage (premult)
    std::vector<float> emission;    // RGB fire emission, A = depth (world units)
    std::vector<float> sunScatter;  // RGB direct sun scatter, A = mean flames value
    std::vector<float> ambScatter;  // RGB ambient scatter, A = mean sun visibility
};

class Renderer {
public:
    Renderer();                 // initializes wgpu instance/adapter/device once
    ~Renderer();

    bool valid() const;
    const std::string& lastError() const;
    std::string adapterName() const;

    // density required; flames may be null.
    // volWorldMatrix: optional column-major 4x4 placing the volume node in
    // world space (Natron ReadVDB TRS). Pass nullptr for identity. The
    // grid's own index->world transform is composed automatically.
    bool render(const openvdb::FloatGrid::ConstPtr& density,
                const openvdb::FloatGrid::ConstPtr& flames,
                const CameraParams& cam,
                const LookParams& look,
                const float* volWorldMatrix,
                int width, int height,
                RenderOutput& out);

    // --- split pipeline for the per-frame compression cache ------------------
    // upload() does the expensive, look-independent work once: compress the
    // grids to bricks and create the resident GPU storage buffers, caching
    // them on the Renderer. draw() does the cheap per-frame work (light grid +
    // ray march + readback) against the last uploaded volume, so knob-only
    // re-renders skip recompression entirely.
    //
    // Caller contract: call upload() whenever the grids or volume transform
    // change (new frame, new file, moved volume); call draw() for every render
    // (including knob-only changes). render() above is exactly upload()+draw().
    bool upload(const openvdb::FloatGrid::ConstPtr& density,
                const openvdb::FloatGrid::ConstPtr& flames,
                const float* volWorldMatrix);
    bool draw(const CameraParams& cam,
              const LookParams& look,
              int width, int height,
              RenderOutput& out);
    bool hasVolume() const;   // true once a successful upload() is resident

private:
    struct Impl;
    std::unique_ptr<Impl> _p;
};

}  // namespace FastVolume

#endif
