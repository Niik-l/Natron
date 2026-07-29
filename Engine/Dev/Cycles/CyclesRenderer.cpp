/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "CyclesRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

// Cycles headers — must be included BEFORE CCL_NAMESPACE_BEGIN block below
#include "device/device.h"
#include "scene/camera.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/pointcloud.h"
#include "scene/hair.h"
#include "scene/volume.h"
#include "scene/image.h"
#include "scene/image_vdb.h"
#include "scene/scene.h"
#include "scene/attribute.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "scene/background.h"
#include "scene/integrator.h"
#include "scene/film.h"
#include "scene/pass.h"
#include "session/buffers.h"
#include "session/deep_output_driver.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/image.h"
#include "util/path.h"
#include "util/progress.h"
#include "util/string.h"
#include "util/unique_ptr.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/color.h>

// Natron headers
#include "Engine/OCIOColorSpaceUtils.h"
#include "Engine/Dev/Scene3D/CameraMath.h"
#include "Engine/Dev/Scene3D/RotationConventions.h"
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/Dev/Scene3D/MaterialProvider.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
#include "Engine/Dev/Scene3D/ReadAlembicArchive.h"
#include "Engine/Dev/Scene3D/CyclesRenderPass.h"
#include "Engine/Dev/Scene3D/ReadVDB.h"
#include "Engine/Dev/Scene3D/Volume3D.h"
#include "Engine/Dev/Particles/ParticleProvider.h"
#include "Engine/Dev/Particles/ParticleMaterial.h"
#include <unordered_map>
#include "Engine/Dev/DotUtils.h"
#include "Engine/Dev/Particles/ParticleInstance.h"
#include "Engine/Dev/Scene3D/Cube3D.h"
#include "Engine/Dev/Scene3D/Sphere3D.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
// Camera3D.h removed — old Camera3D-based methods (syncScene, renderToFile,
// renderToBuffer) are no longer used. CyclesRender node uses renderToBufferWithCamera.

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// NatronOutputDriver — in ccl namespace (inherits ccl::OutputDriver)
// Must be OUTSIDE NATRON_NAMESPACE
// ============================================================================

CCL_NAMESPACE_BEGIN

// Output driver that captures pixels to a memory buffer
class NatronBufferOutputDriver : public OutputDriver {
public:
    NatronBufferOutputDriver(std::vector<float>* buffer, int width, int height)
        : buffer_(buffer), width_(width), height_(height) {}

    void write_render_tile(const Tile& tile) override
    {
        if (!(tile.size == tile.full_size)) return;

        const int w = tile.size.x;
        const int h = tile.size.y;
        buffer_->resize(w * h * 4);

        // Natron images are bottom-up (like OpenGL), same as Cycles internal.
        // No flip needed for buffer output — only flip for file output (top-down formats).
        if (!tile.get_pass_pixels("Combined", 4, buffer_->data())) {
            buffer_->clear();
        }
    }

private:
    std::vector<float>* buffer_;
    int width_, height_;
};

// Output driver that captures multiple AOV passes to memory buffers
class NatronMultiPassOutputDriver : public OutputDriver {
public:
    NatronMultiPassOutputDriver(std::map<std::string, std::vector<float>>* passBuffers,
                                const std::vector<std::string>& passNames,
                                int width, int height)
        : passBuffers_(passBuffers), passNames_(passNames), width_(width), height_(height) {}

    void write_render_tile(const Tile& tile) override
    {
        if (!(tile.size == tile.full_size)) return;

        const int w = tile.size.x;
        const int h = tile.size.y;

        printf("[Cycles AOV] write_render_tile %dx%d, requesting %d passes\n", w, h, (int)passNames_.size());
        for (const auto& passName : passNames_) {
            // Depth and AO are 1-channel passes; request native channels then expand to 4
            bool is1ch = (passName == "Depth" || passName == "AO");
            int nch = is1ch ? 1 : 4;

            std::vector<float> rawBuf(w * h * nch, 0.0f);
            bool ok = tile.get_pass_pixels(passName, nch, rawBuf.data());
            printf("[Cycles AOV]   pass '%s' (%dch): get_pass_pixels=%s\n", passName.c_str(), nch, ok ? "OK" : "FAILED");
            if (ok) {
                // Expand to 4-channel buffer for uniform handling
                std::vector<float> buf(w * h * 4, 0.0f);
                if (is1ch) {
                    for (int p = 0; p < w * h; ++p) {
                        buf[p * 4 + 0] = rawBuf[p];
                        buf[p * 4 + 1] = rawBuf[p];
                        buf[p * 4 + 2] = rawBuf[p];
                        buf[p * 4 + 3] = 1.0f;
                    }
                } else {
                    buf = std::move(rawBuf);
                }

                // Debug: print channel 0 stats
                float ch0Min = 1e30f, ch0Max = -1e30f;
                int nonZeroCount = 0;
                for (int p = 0; p < w * h; ++p) {
                    float v = buf[p * 4];
                    if (v != 0.0f) nonZeroCount++;
                    if (v > 0.001f && v < 1e9f) {
                        if (v < ch0Min) ch0Min = v;
                        if (v > ch0Max) ch0Max = v;
                    }
                }
                printf("[Cycles AOV]   pass '%s': ch0 range=[%.4f, %.4f] nonZero=%d/%d\n",
                       passName.c_str(), ch0Min, ch0Max, nonZeroCount, w * h);

                (*passBuffers_)[passName] = std::move(buf);
            }
        }
    }

private:
    std::map<std::string, std::vector<float>>* passBuffers_;
    std::vector<std::string> passNames_;
    int width_, height_;
};

// Output driver that writes to a file via OIIO
class NatronOutputDriver : public OutputDriver {
public:
    NatronOutputDriver(const std::string& filepath)
        : filepath_(filepath) {}

    void write_render_tile(const Tile& tile) override
    {
        if (!(tile.size == tile.full_size)) {
            return;
        }

        const int width = tile.size.x;
        const int height = tile.size.y;

        std::vector<float> pixels(width * height * 4);
        bool gotPixels = tile.get_pass_pixels("Combined", 4, pixels.data());

        if (!gotPixels) {
            return;
        }

        // Flip vertically (Cycles is bottom-up, files are top-down)
        std::vector<float> flipped(width * height * 4);
        for (int y = 0; y < height; ++y) {
            memcpy(&flipped[y * width * 4],
                   &pixels[(height - 1 - y) * width * 4],
                   width * 4 * sizeof(float));
        }

        // Apply gamma for PNG/JPG (sRGB)
        if (filepath_.find(".png") != std::string::npos ||
            filepath_.find(".PNG") != std::string::npos ||
            filepath_.find(".jpg") != std::string::npos) {
            for (size_t i = 0; i < flipped.size(); i += 4) {
                flipped[i + 0] = powf(std::max(0.0f, flipped[i + 0]), 1.0f / 2.2f);
                flipped[i + 1] = powf(std::max(0.0f, flipped[i + 1]), 1.0f / 2.2f);
                flipped[i + 2] = powf(std::max(0.0f, flipped[i + 2]), 1.0f / 2.2f);
            }
        }

        OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);
        auto image_output = OIIO::ImageOutput::create(filepath_);
        if (!image_output) {
            return;
        }
        if (!image_output->open(filepath_, spec)) {
            return;
        }

        image_output->write_image(OIIO::TypeDesc::FLOAT, flipped.data());
        image_output->close();
    }

private:
    std::string filepath_;
};

// Dense volume loader — subclasses VDBImageLoader to use grid_from_dense_voxels(),
// which converts raw floats → OpenVDB grid → NanoVDB binary (required by Cycles kernel).
class DenseVolumeLoader : public VDBImageLoader {
public:
    DenseVolumeLoader(const std::vector<float>& data, int res,
                      Transform transform_3d = transform_identity())
        : VDBImageLoader("density")
        , data_(data), resolution_(res), transform_3d_(transform_3d)
        , loaded_(false) {}

    string name() const override { return "dense_volume"; }
    bool equals(const ImageLoader& other) const override { return this == &other; }

protected:
    void load_grid() override
    {
        if (!loaded_ && !data_.empty() && resolution_ > 0) {
            grid_from_dense_voxels(
                (size_t)resolution_, (size_t)resolution_, (size_t)resolution_,
                1, data_.data(), transform_3d_);
            loaded_ = true;
        }
    }

private:
    std::vector<float> data_;
    int resolution_;
    Transform transform_3d_;
    bool loaded_;
};

CCL_NAMESPACE_END

NATRON_NAMESPACE_ENTER

// Shadow-catcher diagnostics gate. Set env NATRON_DEBUG_SC=1 to enable the
// verbose [Cycles SC] render logging + the extra raw-catcher pass reads. Off
// by default so production renders stay quiet.
static bool
scDebugEnabled()
{
    static const bool on = (std::getenv("NATRON_DEBUG_SC") != NULL);
    return on;
}

// Cycles session lifecycle diagnostics. Set env NATRON_DEBUG_CYCLES_SESSIONS=1 to
// log every CyclesRenderer + ccl::Session create/destroy with running live counts.
// One live session per actively-previewing node (CyclesRenderPass / CyclesRender) is
// expected; a count that climbs as you scrub / edit is a leak. Off by default.
static bool
cyclesSessionDebugEnabled()
{
    static const bool on = (std::getenv("NATRON_DEBUG_CYCLES_SESSIONS") != NULL);
    return on;
}
static std::atomic<int> g_liveCyclesRenderers{0};
static std::atomic<int> g_liveCyclesSessions{0};

// ============================================================================
// Helper: map Light3D::LightType to ccl::LightType
// ============================================================================

static ccl::LightType mapLightType(Light3D::LightType lt)
{
    switch (lt) {
        case Light3D::eLightPoint:   return ccl::LIGHT_POINT;
        case Light3D::eLightDistant: return ccl::LIGHT_DISTANT;
        case Light3D::eLightSpot:    return ccl::LIGHT_SPOT;
        case Light3D::eLightArea:    return ccl::LIGHT_AREA;
        case Light3D::eLightDome:    return ccl::LIGHT_BACKGROUND;
        default:                     return ccl::LIGHT_POINT;
    }
}

// ============================================================================
// Impl
// ============================================================================

struct CyclesRenderer::Impl
{
    ccl::unique_ptr<ccl::Session> session;
    int width = 0;
    int height = 0;
    int samples = 64;
    bool denoise = false;  // set per-render via setDenoise(); default off
    bool initialized = false;

    // Pixel buffer for readback
    std::vector<float> pixelBuffer;  // RGBA float, width*height*4

    // Per-light ray-visibility overrides for the current render (light name -> flags).
    std::map<std::string, LightRayVis> lightRayVis;

    // When true, objects flagged reflectionMatte are rendered as pure white
    // emitters instead of their normal material. Used by the second (reflection
    // matte) render pass — emission carries through glossy bounces, so the matte
    // appears directly AND in reflections (shader AOVs can't, they're camera-only).
    bool emissiveMatteMode = false;

    Impl() {}
    ~Impl()
    {
        if (session) {
            session->cancel();
            session.reset();
            if (cyclesSessionDebugEnabled()) {
                int s = --g_liveCyclesSessions;
                fprintf(stderr, "[Cycles Session] ~Impl freed session -> live sessions=%d\n", s);
            }
        }
    }
};

// ============================================================================
// Helper: generate sphere mesh vertices/triangles
// ============================================================================

static void generateSphereMesh(std::vector<ccl::float3>& verts,
                                std::vector<int>& triVerts,
                                int rings = 16, int segments = 32)
{
    verts.clear();
    triVerts.clear();

    // Top pole
    verts.push_back(ccl::make_float3(0, 1, 0));

    for (int r = 1; r < rings; ++r) {
        float phi = (float)M_PI * r / rings;
        float y = cosf(phi);
        float sinPhi = sinf(phi);
        for (int s = 0; s < segments; ++s) {
            float theta = 2.0f * (float)M_PI * s / segments;
            verts.push_back(ccl::make_float3(sinPhi * cosf(theta), y, sinPhi * sinf(theta)));
        }
    }

    // Bottom pole
    verts.push_back(ccl::make_float3(0, -1, 0));

    int topPole = 0;
    int bottomPole = (int)verts.size() - 1;

    // Top cap
    for (int s = 0; s < segments; ++s) {
        int next = (s + 1) % segments;
        triVerts.push_back(topPole);
        triVerts.push_back(1 + s);
        triVerts.push_back(1 + next);
    }

    // Middle rings
    for (int r = 0; r < rings - 2; ++r) {
        for (int s = 0; s < segments; ++s) {
            int cur = 1 + r * segments + s;
            int next = 1 + r * segments + (s + 1) % segments;
            int curBelow = 1 + (r + 1) * segments + s;
            int nextBelow = 1 + (r + 1) * segments + (s + 1) % segments;
            triVerts.push_back(cur);
            triVerts.push_back(curBelow);
            triVerts.push_back(next);
            triVerts.push_back(next);
            triVerts.push_back(curBelow);
            triVerts.push_back(nextBelow);
        }
    }

    // Bottom cap
    int lastRingStart = 1 + (rings - 2) * segments;
    for (int s = 0; s < segments; ++s) {
        int next = (s + 1) % segments;
        triVerts.push_back(lastRingStart + s);
        triVerts.push_back(bottomPole);
        triVerts.push_back(lastRingStart + next);
    }
}

// ============================================================================
// Helper: generate box mesh
// ============================================================================

static void generateBoxMesh(std::vector<ccl::float3>& verts,
                             std::vector<int>& triVerts)
{
    verts = {
        ccl::make_float3(-0.5f, -0.5f, -0.5f),  // 0
        ccl::make_float3( 0.5f, -0.5f, -0.5f),  // 1
        ccl::make_float3( 0.5f,  0.5f, -0.5f),  // 2
        ccl::make_float3(-0.5f,  0.5f, -0.5f),  // 3
        ccl::make_float3(-0.5f, -0.5f,  0.5f),  // 4
        ccl::make_float3( 0.5f, -0.5f,  0.5f),  // 5
        ccl::make_float3( 0.5f,  0.5f,  0.5f),  // 6
        ccl::make_float3(-0.5f,  0.5f,  0.5f),  // 7
    };
    triVerts = {
        0,1,2, 0,2,3,   // front
        5,4,7, 5,7,6,   // back
        4,0,3, 4,3,7,   // left
        1,5,6, 1,6,2,   // right
        3,2,6, 3,6,7,   // top
        4,5,1, 4,1,0,   // bottom
    };
}

// ============================================================================
// Helper: generate quad mesh (for Card3D)
// ============================================================================

static void generateQuadMesh(std::vector<ccl::float3>& verts,
                              std::vector<int>& triVerts)
{
    verts = {
        ccl::make_float3(-0.5f, -0.5f, 0.0f),
        ccl::make_float3( 0.5f, -0.5f, 0.0f),
        ccl::make_float3( 0.5f,  0.5f, 0.0f),
        ccl::make_float3(-0.5f,  0.5f, 0.0f),
    };
    triVerts = { 0,1,2, 0,2,3 };
}

// ============================================================================
// Helper: convert Natron column-major 4x4 to Cycles Transform
// ============================================================================

// ImGuizmo-compatible rotation matrix builder (same math as the demo)
static void CyclesRotationAxis(float* m16, float axX, float axY, float axZ, float angleRad)
{
    float c = cosf(angleRad), s = sinf(angleRad), k = 1.0f - c;
    m16[0]=axX*axX*k+c;     m16[1]=axX*axY*k+axZ*s;  m16[2]=axX*axZ*k-axY*s;  m16[3]=0;
    m16[4]=axX*axY*k-axZ*s; m16[5]=axY*axY*k+c;      m16[6]=axY*axZ*k+axX*s;  m16[7]=0;
    m16[8]=axX*axZ*k+axY*s; m16[9]=axY*axZ*k-axX*s;  m16[10]=axZ*axZ*k+c;     m16[11]=0;
    m16[12]=0;              m16[13]=0;                m16[14]=0;                m16[15]=1;
}

static void CyclesFPU_MatMul(const float* a, const float* b, float* r)
{
    r[0]  = a[0]*b[0]  + a[1]*b[4]  + a[2]*b[8]   + a[3]*b[12];
    r[1]  = a[0]*b[1]  + a[1]*b[5]  + a[2]*b[9]   + a[3]*b[13];
    r[2]  = a[0]*b[2]  + a[1]*b[6]  + a[2]*b[10]  + a[3]*b[14];
    r[3]  = a[0]*b[3]  + a[1]*b[7]  + a[2]*b[11]  + a[3]*b[15];
    r[4]  = a[4]*b[0]  + a[5]*b[4]  + a[6]*b[8]   + a[7]*b[12];
    r[5]  = a[4]*b[1]  + a[5]*b[5]  + a[6]*b[9]   + a[7]*b[13];
    r[6]  = a[4]*b[2]  + a[5]*b[6]  + a[6]*b[10]  + a[7]*b[14];
    r[7]  = a[4]*b[3]  + a[5]*b[7]  + a[6]*b[11]  + a[7]*b[15];
    r[8]  = a[8]*b[0]  + a[9]*b[4]  + a[10]*b[8]  + a[11]*b[12];
    r[9]  = a[8]*b[1]  + a[9]*b[5]  + a[10]*b[9]  + a[11]*b[13];
    r[10] = a[8]*b[2]  + a[9]*b[6]  + a[10]*b[10] + a[11]*b[14];
    r[11] = a[8]*b[3]  + a[9]*b[7]  + a[10]*b[11] + a[11]*b[15];
    r[12] = a[12]*b[0] + a[13]*b[4] + a[14]*b[8]  + a[15]*b[12];
    r[13] = a[12]*b[1] + a[13]*b[5] + a[14]*b[9]  + a[15]*b[13];
    r[14] = a[12]*b[2] + a[13]*b[6] + a[14]*b[10] + a[15]*b[14];
    r[15] = a[12]*b[3] + a[13]*b[7] + a[14]*b[11] + a[15]*b[15];
}

static ccl::Transform buildLightTransform(float tx, float ty, float tz,
                                           float rxDeg, float ryDeg, float rzDeg)
{
    // Build the SAME matrix as ImGuizmo::RecomposeMatrixFromComponents
    // (Rx * Ry * Rz), then convert to Cycles Transform.
    // This guarantees Cycles sees the same orientation as the 3D viewport.
    float rotX[16], rotY[16], rotZ[16], tmp[16], m[16];
    CyclesRotationAxis(rotX, 1, 0, 0, rxDeg * (float)M_PI / 180.0f);
    CyclesRotationAxis(rotY, 0, 1, 0, ryDeg * (float)M_PI / 180.0f);
    CyclesRotationAxis(rotZ, 0, 0, 1, rzDeg * (float)M_PI / 180.0f);
    CyclesFPU_MatMul(rotX, rotY, tmp);
    CyclesFPU_MatMul(tmp, rotZ, m);

    // Set position at m[12]/m[13]/m[14] — same layout as SceneGraph::buildTRS
    // so natronMatrixToCyclesTransform reads it correctly
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1.0f;
    // Same conversion as geometry objects
    ccl::Transform tfm;
    tfm.x = ccl::make_float4(m[0], m[4], m[8],  m[12]);
    tfm.y = ccl::make_float4(m[1], m[5], m[9],  m[13]);
    tfm.z = ccl::make_float4(m[2], m[6], m[10], m[14]);
    return tfm;
}

static ccl::Transform natronMatrixToCyclesTransform(const float m[16])
{
    // Natron stores column-major (OpenGL style): m[col*4 + row]
    // Cycles Transform is row-major 4x3 (no bottom row needed).
    ccl::Transform tfm;
    tfm.x = ccl::make_float4(m[0], m[4], m[8],  m[12]);
    tfm.y = ccl::make_float4(m[1], m[5], m[9],  m[13]);
    tfm.z = ccl::make_float4(m[2], m[6], m[10], m[14]);
    return tfm;
}

// ============================================================================
// Material shader creation helper
// ============================================================================

// Resolve #### or %04d frame patterns in texture paths
static std::string resolveTextureFrame(const std::string& path, int frame)
{
    if (path.empty()) return path;

    std::string result = path;

    // Replace #### with zero-padded frame number
    size_t hashStart = result.find('#');
    if (hashStart != std::string::npos) {
        size_t hashEnd = hashStart;
        while (hashEnd < result.size() && result[hashEnd] == '#') ++hashEnd;
        int padding = (int)(hashEnd - hashStart);
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(padding) << frame;
        result.replace(hashStart, hashEnd - hashStart, ss.str());
        return result;
    }

    // Replace %04d style patterns
    char buf[1024];
    snprintf(buf, sizeof(buf), result.c_str(), frame);
    if (std::string(buf) != result) return std::string(buf);

    // Also try: find last digit group before extension and substitute
    size_t dotPos = result.rfind('.');
    if (dotPos != std::string::npos && dotPos > 0) {
        size_t numEnd = dotPos;
        size_t numStart = numEnd;
        while (numStart > 0 && result[numStart - 1] >= '0' && result[numStart - 1] <= '9') --numStart;
        if (numStart < numEnd) {
            int padding = (int)(numEnd - numStart);
            std::ostringstream ss;
            ss << std::setfill('0') << std::setw(padding) << frame;
            result.replace(numStart, numEnd - numStart, ss.str());
            return result;
        }
    }

    return result;
}

// Map a Material3D colorspace choice ("sRGB" / "Linear" / "ACEScg" / "Raw") to a
// Cycles colorspace string that resolves under ANY active OCIO config.
//
// Cycles converts textures via OIIO/OpenColorIO using the config pointed to by
// the OCIO env var — which Natron sets to its selected config (Settings.cpp),
// so the render space follows Natron's config (Linear Rec.709 under the default
// 'blender' config, ACEScg under an ACES config). To stay config-agnostic:
//   - "Raw"    -> Cycles' built-in raw (identity; no transform), works in any config.
//   - "Linear" -> built-in raw too (the texture is already in the rendering
//                 scene-linear space, so no transform).
//   - color spaces ("sRGB", "ACEScg", or any custom name) pass through as OCIO
//     names so OCIO converts them into the config's rendering space (correct
//     primaries for both Rec.709 and ACES, unlike Cycles' built-in sRGB which
//     assumes Rec.709). The chosen name must exist in the active config.
// Data maps (normal/roughness/metallic/transmission) always use built-in raw
// directly — they're never color-managed.
static ccl::ustring
materialColorspaceToCycles(const std::string& choice)
{
    if (choice == "Raw" || choice == "Linear") {
        return ccl::ustring("__builtin_raw");
    }
    return ccl::ustring(choice);
}

// Pure white emitter, used for reflection-matte objects in the second render
// pass. A flat constant emission reads as a clean matte both directly and in
// reflections, with no dependence on scene lighting.
static ccl::Shader*
createEmissiveMatteShader(ccl::Scene* scene)
{
    ccl::Shader* shader = scene->create_node<ccl::Shader>();
    auto graph = ccl::make_unique<ccl::ShaderGraph>();
    ccl::EmissionNode* em = graph->create_node<ccl::EmissionNode>();
    em->set_color(ccl::make_float3(1.0f, 1.0f, 1.0f));
    em->set_strength(1.0f);
    graph->connect(em->output("Emission"), graph->output()->input("Surface"));
    shader->set_graph(std::move(graph));
    shader->tag_update(scene);
    return shader;
}

static ccl::Shader*
createMaterialShader(ccl::Scene* scene, MaterialProvider* matProvider, double time,
                     bool particleColorTint = false,
                     float particleEmissionStrength = -1.0f)
{
    // Resolve: if a Material3D is connected, use it instead of inline knobs
    MaterialProvider* mat = matProvider;
    if (mat->hasMaterialInput()) {
        MaterialProvider* connected = mat->getConnectedMaterial();
        if (connected) mat = connected;
    }

    ccl::Shader* shader = scene->create_node<ccl::Shader>();
    auto graph = ccl::make_unique<ccl::ShaderGraph>();

    ccl::PrincipledBsdfNode* principled = graph->create_node<ccl::PrincipledBsdfNode>();

    double r, g, b;
    mat->getMaterialBaseColor(time, r, g, b);
    principled->set_base_color(ccl::make_float3((float)r, (float)g, (float)b));
    principled->set_roughness((float)mat->getMaterialRoughness(time));
    principled->set_metallic((float)mat->getMaterialMetallic(time));
    principled->set_specular_ior_level((float)mat->getMaterialSpecular(time));
    principled->set_transmission_weight((float)mat->getMaterialTransmission(time));
    principled->set_ior((float)mat->getMaterialIOR(time));

    double er, eg, eb, estrength;
    mat->getMaterialEmission(time, er, eg, eb, estrength);
    principled->set_emission_color(ccl::make_float3((float)er, (float)eg, (float)eb));
    principled->set_emission_strength((float)estrength);

    // Shared texture coordinate node (reused for all texture maps)
    ccl::TextureCoordinateNode* texCoord = graph->create_node<ccl::TextureCoordinateNode>();
    int frame = (int)time;

    // Base color texture
    ccl::ImageTextureNode* baseTex = nullptr;
    std::string texFile = resolveTextureFrame(mat->getMaterialTextureFile(), frame);
    if (!texFile.empty()) {
        baseTex = graph->create_node<ccl::ImageTextureNode>();
        baseTex->set_filename(ccl::ustring(texFile));
        baseTex->set_colorspace(materialColorspaceToCycles(mat->getMaterialDiffuseColorspace()));
        graph->connect(texCoord->output("UV"), baseTex->input("Vector"));
        if (!particleColorTint) {
            graph->connect(baseTex->output("Color"), principled->input("Base Color"));
        }
    }

    // Particle tint: multiply the per-particle vertex color into the base
    // color (texture output when present, else the flat material color), and
    // drive alpha from the particle age fade — so emitter colors and
    // ParticleAttribute ramps show through the material.
    ccl::AttributeNode* particleVcol = nullptr;
    if (particleColorTint) {
        particleVcol = graph->create_node<ccl::AttributeNode>();
        // "particle_color", NOT "vertex_color" — the latter is a reserved
        // Cycles standard-attribute name and never matches our named attr.
        particleVcol->set_attribute(ccl::ustring("particle_color"));
        ccl::VectorMathNode* mul = graph->create_node<ccl::VectorMathNode>();
        mul->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
        if (baseTex) {
            graph->connect(baseTex->output("Color"), mul->input("Vector1"));
        } else {
            mul->set_vector1(ccl::make_float3((float)r, (float)g, (float)b));
        }
        graph->connect(particleVcol->output("Color"), mul->input("Vector2"));
        graph->connect(mul->output("Vector"), principled->input("Base Color"));
        graph->connect(particleVcol->output("Alpha"), principled->input("Alpha"));
    }

    // ParticleMaterial's Emission Strength (>0) overrides the material's
    // emission: glow in the per-particle color (tint), else the base color.
    const bool particleEmissionOverride = (particleEmissionStrength > 0.0f);
    if (particleEmissionOverride) {
        // Strength = knob × the per-particle emission attribute (the
        // ParticleAttribute Emission section's output; 1.0 when unused).
        ccl::AttributeNode* emisAttr = graph->create_node<ccl::AttributeNode>();
        emisAttr->set_attribute(ccl::ustring("particle_emission"));
        ccl::MathNode* emisMul = graph->create_node<ccl::MathNode>();
        emisMul->set_math_type(ccl::NODE_MATH_MULTIPLY);
        emisMul->set_value2(particleEmissionStrength);
        graph->connect(emisAttr->output("Fac"), emisMul->input("Value1"));
        graph->connect(emisMul->output("Value"), principled->input("Emission Strength"));
        if (particleVcol) {
            graph->connect(particleVcol->output("Color"), principled->input("Emission Color"));
        } else if (baseTex) {
            graph->connect(baseTex->output("Color"), principled->input("Emission Color"));
        } else {
            principled->set_emission_color(ccl::make_float3((float)r, (float)g, (float)b));
        }
    }

    // Normal map
    std::string normalFile = resolveTextureFrame(mat->getMaterialNormalMapFile(), frame);
    if (!normalFile.empty()) {
        ccl::ImageTextureNode* normalTex = graph->create_node<ccl::ImageTextureNode>();
        normalTex->set_filename(ccl::ustring(normalFile));
        normalTex->set_colorspace(ccl::ustring("__builtin_raw"));
        ccl::NormalMapNode* normalMap = graph->create_node<ccl::NormalMapNode>();
        normalMap->set_strength((float)mat->getMaterialNormalStrength(time));
        graph->connect(texCoord->output("UV"), normalTex->input("Vector"));
        graph->connect(normalTex->output("Color"), normalMap->input("Color"));
        graph->connect(normalMap->output("Normal"), principled->input("Normal"));
    }

    // Roughness map
    std::string roughFile = resolveTextureFrame(mat->getMaterialRoughnessMapFile(), frame);
    if (!roughFile.empty()) {
        ccl::ImageTextureNode* roughTex = graph->create_node<ccl::ImageTextureNode>();
        roughTex->set_filename(ccl::ustring(roughFile));
        roughTex->set_colorspace(ccl::ustring("__builtin_raw"));
        graph->connect(texCoord->output("UV"), roughTex->input("Vector"));
        graph->connect(roughTex->output("Color"), principled->input("Roughness"));
    }

    // Metallic map
    std::string metalFile = resolveTextureFrame(mat->getMaterialMetallicMapFile(), frame);
    if (!metalFile.empty()) {
        ccl::ImageTextureNode* metalTex = graph->create_node<ccl::ImageTextureNode>();
        metalTex->set_filename(ccl::ustring(metalFile));
        metalTex->set_colorspace(ccl::ustring("__builtin_raw"));
        graph->connect(texCoord->output("UV"), metalTex->input("Vector"));
        graph->connect(metalTex->output("Color"), principled->input("Metallic"));
    }

    // Emission map
    std::string emissionFile = resolveTextureFrame(mat->getMaterialEmissionMapFile(), frame);
    if (!emissionFile.empty()) {
        ccl::ImageTextureNode* emissionTex = graph->create_node<ccl::ImageTextureNode>();
        emissionTex->set_filename(ccl::ustring(emissionFile));
        emissionTex->set_colorspace(materialColorspaceToCycles(mat->getMaterialEmissionColorspace()));
        graph->connect(texCoord->output("UV"), emissionTex->input("Vector"));
        if (!particleEmissionOverride) { // particle glow override wins over the emission map
            graph->connect(emissionTex->output("Color"), principled->input("Emission Color"));
        }
    }

    // Transmission map (mask) — drives the Principled "Transmission Weight"
    // input per-pixel: white = glass/transparent, black = opaque. Overrides the
    // scalar Transmission slider where present.
    std::string transFile = resolveTextureFrame(mat->getMaterialTransmissionMapFile(), frame);
    if (!transFile.empty()) {
        ccl::ImageTextureNode* transTex = graph->create_node<ccl::ImageTextureNode>();
        transTex->set_filename(ccl::ustring(transFile));
        transTex->set_colorspace(ccl::ustring("__builtin_raw"));
        graph->connect(texCoord->output("UV"), transTex->input("Vector"));
        graph->connect(transTex->output("Color"), principled->input("Transmission Weight"));
    }

    graph->connect(principled->output("BSDF"), graph->output()->input("Surface"));
    shader->set_graph(std::move(graph));
    shader->tag_update(scene);
    return shader;
}

// ============================================================================
// CyclesRenderer
// ============================================================================

CyclesRenderer::CyclesRenderer()
    : _impl(new Impl())
{
    if (cyclesSessionDebugEnabled()) {
        int n = ++g_liveCyclesRenderers;
        fprintf(stderr, "[Cycles Session] CyclesRenderer ctor this=%p -> live renderers=%d (sessions=%d)\n",
                (void*)this, n, g_liveCyclesSessions.load());
    }
}

CyclesRenderer::~CyclesRenderer()
{
    // _impl (and the ccl::Session it owns) is freed after this body via ~Impl,
    // which emits its own "[Cycles Session] ~Impl freed session" line.
    if (cyclesSessionDebugEnabled()) {
        int n = --g_liveCyclesRenderers;
        fprintf(stderr, "[Cycles Session] CyclesRenderer dtor this=%p -> live renderers=%d (sessions=%d)\n",
                (void*)this, n, g_liveCyclesSessions.load());
    }
}

void
CyclesRenderer::setLightRayVisibility(const std::map<std::string, LightRayVis>* overrides)
{
    if (overrides) _impl->lightRayVis = *overrides;
    else           _impl->lightRayVis.clear();
}

bool
CyclesRenderer::initialize(int width, int height, int samples)
{
    _impl->width = width;
    _impl->height = height;
    _impl->samples = samples;

    // Init Cycles path (needed for kernel loading)
    ccl::path_init();

    _impl->initialized = true;
    return true;
}

// syncScene(Camera3D) removed — use syncSceneWithCamera() instead
// renderToFile(Camera3D) removed — no longer needed
// renderToBuffer(Camera3D) removed — use renderToBufferWithCamera() instead

void
CyclesRenderer::startRender()
{
    if (!_impl->session) return;

    ccl::BufferParams bufferParams;
    bufferParams.width = _impl->width;
    bufferParams.height = _impl->height;
    bufferParams.full_width = _impl->width;
    bufferParams.full_height = _impl->height;

    ccl::SessionParams sp = _impl->session->params;
    sp.samples = _impl->samples;
    _impl->session->reset(sp, bufferParams);
    _impl->session->start();
}

void
CyclesRenderer::cancelRender()
{
    if (_impl->session) {
        _impl->session->cancel();
    }
}

void
CyclesRenderer::waitForRender()
{
    if (_impl->session) {
        _impl->session->wait();
    }
}

bool
CyclesRenderer::isRendering() const
{
    if (!_impl->session) return false;
    return _impl->session->progress.get_progress() < 1.0f;
}

float
CyclesRenderer::getProgress() const
{
    if (!_impl->session) return 0.0f;
    return _impl->session->progress.get_progress();
}

int
CyclesRenderer::getCurrentSample() const
{
    if (!_impl->session) return 0;
    return _impl->session->progress.get_current_sample();
}

bool
CyclesRenderer::getPixels(float* rgba, int width, int height) const
{
    if (!_impl->session) return false;
    // TODO: implement pixel readback from Cycles session buffers
    return false;
}

void
CyclesRenderer::syncSceneWithCamera(const SceneGraph& sg,
                                     double camTX, double camTY, double camTZ,
                                     double camRX, double camRY, double camRZ,
                                     double focalLength, double hAperture, double vAperture,
                                     double time,
                                     const std::vector<std::string>& requestedPasses,
                                     const std::map<std::string, ObjectVisibility>* visibilityMap,
                                     const std::set<std::string>* activeLights,
                                     const DOFParams* dof,
                                     const MotionBlurParams* motionBlur,
                                     const IntegratorParams* integrator,
                                     MaterialProvider* materialOverride,
                                     const std::set<std::string>* holdoutObjects)
{
    if (!_impl->initialized) return;

    // Cancel any in-progress render
    if (_impl->session) {
        _impl->session->cancel();
        _impl->session.reset();
        if (cyclesSessionDebugEnabled()) {
            int s = --g_liveCyclesSessions;
            fprintf(stderr, "[Cycles Session] reset prior session (this=%p) -> live sessions=%d\n",
                    (void*)this, s);
        }
    }

    ccl::SessionParams sessionParams;
    sessionParams.background = true;
    sessionParams.samples = _impl->samples;
    sessionParams.threads = 0;

    // Explicitly select CPU device (required for NanoVDB volume support).
    // Without this, volumes render as empty/black.
    {
        std::vector<ccl::DeviceInfo> devices = ccl::Device::available_devices(
            (uint)ccl::DEVICE_MASK_CPU);
        if (!devices.empty()) {
            sessionParams.device = devices.front();
        }
    }

    ccl::SceneParams sceneParams;
    _impl->session = ccl::make_unique<ccl::Session>(sessionParams, sceneParams);
    if (cyclesSessionDebugEnabled()) {
        int s = ++g_liveCyclesSessions;
        fprintf(stderr, "[Cycles Session] created session (this=%p) -> live sessions=%d (renderers=%d)\n",
                (void*)this, s, g_liveCyclesRenderers.load());
    }
    ccl::Scene* scene = _impl->session->scene.get();

    // --- Default material ---
    {
        ccl::Shader* defaultShader = scene->default_surface;
        auto graph = ccl::make_unique<ccl::ShaderGraph>();
        ccl::PrincipledBsdfNode* principled = graph->create_node<ccl::PrincipledBsdfNode>();
        principled->set_base_color(ccl::make_float3(0.8f, 0.2f, 0.2f));
        principled->set_roughness(0.3f);
        principled->set_metallic(0.0f);
        graph->connect(principled->output("BSDF"), graph->output()->input("Surface"));
        defaultShader->set_graph(std::move(graph));
        defaultShader->tag_update(scene);
    }

    // --- Background + Lights ---
    // First scan for dome lights to set the background shader.
    // Then process other light types.
    bool hasDome = false;
    bool hasDomeCameraVisible = false;
    unsigned int domeBgVisibility = ccl::PATH_RAY_ALL_VISIBILITY;  // dome ray-visibility (overridable)
    {
        // === First pass: find dome lights and set background ===
        const std::vector<SceneNode>& lightScan = sg.nodes();
        for (size_t i = 0; i < lightScan.size(); ++i) {
            const SceneNode& sn = lightScan[i];
            if (sn.type != eSceneNodeLight || !sn.visible) continue;
            NodePtr node = sn.sourceNode.lock();
            if (!node) continue;
            Light3D* light3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
            if (!light3d || light3d->getLightType() != Light3D::eLightDome) continue;
            // Respect the Active Lights selection — a dome that isn't active must NOT
            // light the scene (matches non-dome lights; "no lights selected" = dark).
            // Without this the dome lit the scene regardless of its Active checkbox.
            if (activeLights && !activeLights->empty() &&
                activeLights->find(sn.name) == activeLights->end()) {
                continue;
            }
            // "Renderable" controls camera visibility only (like Arnold's skydome Camera flag).
            bool domeVisibleInCamera = light3d->isRenderable();

            // Per-light ray-visibility override (from the CyclesRenderPass Active Lights rows):
            // untick Refl/Diff/Trans to drop the dome ENVIRONMENT from that ray type
            // (it still lights the scene); Cam combines with Renderable.
            {
                auto it = _impl->lightRayVis.find(node->getScriptName_mt_safe());
                if (it != _impl->lightRayVis.end()) {
                    if (!it->second.camera)   domeVisibleInCamera = false;
                    if (!it->second.glossy)   domeBgVisibility &= ~ccl::PATH_RAY_GLOSSY;
                    if (!it->second.diffuse)  domeBgVisibility &= ~ccl::PATH_RAY_DIFFUSE;
                    if (!it->second.transmit) domeBgVisibility &= ~ccl::PATH_RAY_TRANSMIT;
                }
            }

            double ltx, lty, ltz, lr, lg, lb, lint, lexp;
            light3d->getLightParams(time, ltx, lty, ltz, lr, lg, lb, lint, lexp);
            lint *= pow(2.0, lexp);
            float strengthScale = (float)lint * 10.0f;
            std::string envMap = light3d->getEnvironmentMap();

            ccl::Shader* bgShader = scene->default_background;
            auto bgGraph = ccl::make_unique<ccl::ShaderGraph>();

            if (!envMap.empty()) {
                // HDRI environment map
                ccl::EnvironmentTextureNode* envTex = bgGraph->create_node<ccl::EnvironmentTextureNode>();
                envTex->set_filename(ccl::ustring(envMap));
                envTex->set_colorspace(ccl::ustring("__builtin_raw")); // HDRIs are linear

                // Rotation via Mapping node (read light's rotateX/Y/Z)
                float lrx = 0, lry = 0, lrz = 0;
                {
                    EffectInstancePtr eff = node->getEffectInstance();
                    if (eff) {
                        KnobIPtr k;
                        k = eff->getKnobByName("rotateX"); if (k) lrx = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                        k = eff->getKnobByName("rotateY"); if (k) lry = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                        k = eff->getKnobByName("rotateZ"); if (k) lrz = (float)dynamic_cast<KnobDouble*>(k.get())->getValueAtTime(time);
                    }
                }
                // Cycles env texture uses Z-up, our scene is Y-up.
                // Swap Y↔Z via SeparateXYZ/CombineXYZ so rotations work in Y-up space.
                ccl::TextureCoordinateNode* texCoord = bgGraph->create_node<ccl::TextureCoordinateNode>();
                ccl::SeparateXYZNode* sepXYZ = bgGraph->create_node<ccl::SeparateXYZNode>();
                ccl::CombineXYZNode* combXYZ = bgGraph->create_node<ccl::CombineXYZNode>();

                bgGraph->connect(texCoord->output("Generated"), sepXYZ->input("Vector"));
                // Remap: new_X = old_X, new_Y = old_Z, new_Z = old_Y (Y↔Z swap)
                bgGraph->connect(sepXYZ->output("X"), combXYZ->input("X"));
                bgGraph->connect(sepXYZ->output("Z"), combXYZ->input("Y"));
                bgGraph->connect(sepXYZ->output("Y"), combXYZ->input("Z"));

                // Apply user rotation — axes are swapped in this space (Y↔Z),
                // so user's rotateY (horizontal spin) maps to MappingNode Z,
                // and user's rotateZ maps to MappingNode Y.
                ccl::MappingNode* mapping = bgGraph->create_node<ccl::MappingNode>();
                mapping->set_mapping_type(ccl::NODE_MAPPING_TYPE_POINT);
                mapping->set_rotation(ccl::make_float3(
                    lrx * (float)M_PI / 180.0f,
                    lrz * (float)M_PI / 180.0f,
                    lry * (float)M_PI / 180.0f));
                bgGraph->connect(combXYZ->output("Vector"), mapping->input("Vector"));
                bgGraph->connect(mapping->output("Vector"), envTex->input("Vector"));

                ccl::BackgroundNode* bgNode = bgGraph->create_node<ccl::BackgroundNode>();
                bgNode->set_strength(strengthScale);

                // Tint HDRI by Light Color (if not white)
                bool isWhite = (std::abs(lr - 1.0) < 0.001 && std::abs(lg - 1.0) < 0.001 && std::abs(lb - 1.0) < 0.001);
                if (!isWhite) {
                    ccl::MixNode* tint = bgGraph->create_node<ccl::MixNode>();
                    tint->set_mix_type(ccl::NODE_MIX_MUL);
                    tint->set_fac(1.0f);
                    tint->set_color2(ccl::make_float3((float)lr, (float)lg, (float)lb));
                    bgGraph->connect(envTex->output("Color"), tint->input("Color1"));
                    bgGraph->connect(tint->output("Color"), bgNode->input("Color"));
                } else {
                    bgGraph->connect(envTex->output("Color"), bgNode->input("Color"));
                }
                bgGraph->connect(bgNode->output("Background"), bgGraph->output()->input("Surface"));
            } else {
                // Solid color dome
                ccl::BackgroundNode* bgNode = bgGraph->create_node<ccl::BackgroundNode>();
                bgNode->set_color(ccl::make_float3((float)lr, (float)lg, (float)lb));
                bgNode->set_strength(strengthScale);
                bgGraph->connect(bgNode->output("Background"), bgGraph->output()->input("Surface"));
            }

            bgShader->set_graph(std::move(bgGraph));
            bgShader->tag_update(scene);

            // Create a background Light object for HDRI importance sampling (MIS).
            // Without this, Cycles samples the HDRI uniformly → excessive noise.
            ccl::Light* bgLight = scene->create_node<ccl::Light>();
            bgLight->set_light_type(ccl::LIGHT_BACKGROUND);
            bgLight->set_use_mis(true);
            bgLight->set_map_resolution(2048);
            bgLight->set_strength(ccl::make_float3(1.0f, 1.0f, 1.0f));
            bgLight->set_is_enabled(true);
            // Assign the background shader so Cycles can importance-sample the HDRI
            ccl::array<ccl::Node*> bgLightShaders;
            bgLightShaders.push_back_slow(bgShader);
            bgLight->set_used_shaders(bgLightShaders);
            bgLight->tag_update(scene);

            hasDome = true;
            hasDomeCameraVisible = domeVisibleInCamera;
            break; // only one dome light
        }

        // Default background: black (no dome light = only point/area lights illuminate)
        if (!hasDome) {
            ccl::Shader* bgShader = scene->default_background;
            auto bgGraph = ccl::make_unique<ccl::ShaderGraph>();
            ccl::BackgroundNode* bgNode = bgGraph->create_node<ccl::BackgroundNode>();
            bgNode->set_color(ccl::make_float3(0.0f, 0.0f, 0.0f));
            bgNode->set_strength(0.0f);
            bgGraph->connect(bgNode->output("Background"), bgGraph->output()->input("Surface"));
            bgShader->set_graph(std::move(bgGraph));
            bgShader->tag_update(scene);
        }

        // === Second pass: process non-dome lights ===
        for (size_t i = 0; i < lightScan.size(); ++i) {
            const SceneNode& sn = lightScan[i];

            if (sn.type != eSceneNodeLight || !sn.visible) continue;

            NodePtr node = sn.sourceNode.lock();
            if (!node) continue;

            Light3D* light3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
            if (!light3d) continue;
            if (!light3d->isRenderable()) continue;

            Light3D::LightType ltype = light3d->getLightType();
            // Skip dome lights — already handled as background
            if (ltype == Light3D::eLightDome) continue;

            double ltx, lty, ltz, lr, lg, lb, lint, lexp;
            light3d->getLightParams(time, ltx, lty, ltz, lr, lg, lb, lint, lexp);
            // Apply exposure: multiply intensity by 2^exposure
            lint *= pow(2.0, lexp);

            ccl::Light* light = scene->create_node<ccl::Light>();
            light->set_light_type(mapLightType(ltype));
            float strengthScale = (float)lint * 10.0f;
            light->set_strength(ccl::make_float3((float)lr * strengthScale,
                                                  (float)lg * strengthScale,
                                                  (float)lb * strengthScale));
            light->set_size(ltype == Light3D::eLightArea ? 1.0f : 0.1f);
            light->set_use_mis(true);
            light->set_cast_shadow(true);
            if (ltype == Light3D::eLightSpot) {
                light->set_spot_angle((float)light3d->getSpotAngle(time) * (float)M_PI / 180.0f);
                light->set_spot_smooth((float)light3d->getSpotSmooth(time));
            }
            if (ltype == Light3D::eLightArea) {
                light->set_sizeu((float)light3d->getAreaSizeU(time));
                light->set_sizev((float)light3d->getAreaSizeV(time));
                light->set_spread((float)light3d->getSpread(time) * (float)M_PI / 180.0f);
            }

            ccl::Shader* lShader = scene->create_node<ccl::Shader>();
            auto lGraph = ccl::make_unique<ccl::ShaderGraph>();

            // For dome lights with HDRI, use environment texture
            std::string envMap = light3d->getEnvironmentMap();
            if (ltype == Light3D::eLightDome && !envMap.empty()) {
                ccl::EnvironmentTextureNode* envTex = lGraph->create_node<ccl::EnvironmentTextureNode>();
                envTex->set_filename(ccl::ustring(envMap));
                envTex->set_colorspace(ccl::ustring("__builtin_raw")); // HDRIs are linear
                ccl::BackgroundNode* bgNode = lGraph->create_node<ccl::BackgroundNode>();
                bgNode->set_strength(strengthScale);
                lGraph->connect(envTex->output("Color"), bgNode->input("Color"));
                lGraph->connect(bgNode->output("Background"), lGraph->output()->input("Surface"));
            } else if (ltype == Light3D::eLightDome) {
                // Dome light with solid color (no HDRI)
                ccl::BackgroundNode* bgNode = lGraph->create_node<ccl::BackgroundNode>();
                bgNode->set_color(ccl::make_float3((float)lr, (float)lg, (float)lb));
                bgNode->set_strength(strengthScale);
                lGraph->connect(bgNode->output("Background"), lGraph->output()->input("Surface"));
            } else {
                // Regular light: emission shader
                ccl::EmissionNode* lEmission = lGraph->create_node<ccl::EmissionNode>();
                lEmission->set_color(ccl::make_float3((float)lr, (float)lg, (float)lb));
                lEmission->set_strength(strengthScale);
                lGraph->connect(lEmission->output("Emission"), lGraph->output()->input("Surface"));
            }

            lShader->set_graph(std::move(lGraph));
            lShader->tag_update(scene);

            ccl::array<ccl::Node*> shaders;
            shaders.push_back_slow(lShader);
            light->set_used_shaders(shaders);

            // Apply active lights filter if provided
            if (activeLights && !activeLights->empty()) {
                if (activeLights->find(sn.name) == activeLights->end()) {
                    // Light not in active set — skip it entirely
                    continue;
                }
            }

            ccl::Object* obj = scene->create_node<ccl::Object>();
            obj->set_geometry(light);
            // Natural default: visible to all rays except camera (you don't see the
            // lamp directly). Per-light override (CyclesRenderPass Active Lights rows) is
            // subtractive: unticking Refl/Diff/Trans removes the lamp's visible SHAPE
            // from that ray type (e.g. an area light not appearing in reflections —
            // its illumination/highlight still lands; that's handled by light sampling).
            unsigned int lightVis = ccl::PATH_RAY_ALL_VISIBILITY & ~ccl::PATH_RAY_CAMERA;
            {
                auto it = _impl->lightRayVis.find(sn.name);
                if (it != _impl->lightRayVis.end()) {
                    if (!it->second.camera)   lightVis &= ~ccl::PATH_RAY_CAMERA;
                    if (!it->second.glossy)   lightVis &= ~ccl::PATH_RAY_GLOSSY;
                    if (!it->second.diffuse)  lightVis &= ~ccl::PATH_RAY_DIFFUSE;
                    if (!it->second.transmit) lightVis &= ~ccl::PATH_RAY_TRANSMIT;
                }
            }
            obj->set_visibility(lightVis);
            // Mark the light's wrapper object as a shadow catcher so Cycles does
            // NOT stamp SHADER_EXCLUDE_SHADOW_CATCHER on the lamp (light.cpp:331).
            // Without this the lamp is skipped during NEE in the shadow-catcher
            // object pass (light.h:117), the catcher's unshadowed reference is
            // unlit (color_catcher=0), and the shadow ratio collapses to a flat
            // 1.0 (no shadow). This matches Blender, where lamps illuminate the
            // catcher pass by default. Harmless when no catcher is present.
            obj->set_is_shadow_catcher(true);
            // Use the SceneGraph world matrix — position + rotation + any parent
            // Group3D transform — exactly like geometry. This makes grouped /
            // nested lights follow the group and match the viewport. (Was
            // buildLightTransform() from the light's own knobs, which ignored
            // the group transform, so grouped lights rendered at their local
            // position.) For an ungrouped light this is byte-identical to the
            // old path (buildTRS == buildLightTransform).
            obj->set_tfm(natronMatrixToCyclesTransform(sn.worldMatrix));

            // Set light group on the object (Cycles uses Object::lightgroup)
            std::string lgName = light3d->getLightGroup();
            if (!lgName.empty()) {
                obj->set_lightgroup(ccl::ustring(lgName));
            }

            obj->tag_update(scene);
        }
    }

    // --- Camera from Euler angles ---
    // CameraProvider gives: world position (tx,ty,tz) + euler rotation (rx,ry,rz) in degrees
    //
    // Natron camera convention: at zero rotation, camera looks down -Z (like OpenGL).
    // Cycles camera convention: camera looks down +Z in local space.
    // So we need to flip the Z axis: negate the forward column of the rotation matrix.
    {
        // Camera-to-world rotation in Natron's standard extrinsic XYZ convention
        // (M = Rz*Ry*Rx column-vector, matches SceneGraph::buildTRS, ImGuizmo,
        // and Maya/Blender/Houdini default).
        double mCam[3][3];
        RotationConventions::compose(camRX, camRY, camRZ, mCam);
        const float r00 = (float)mCam[0][0]; // right.x   = M[0][0]
        const float r01 = (float)mCam[1][0]; // right.y   = M[1][0]
        const float r02 = (float)mCam[2][0]; // right.z   = M[2][0]
        const float r10 = (float)mCam[0][1]; // up.x      = M[0][1]
        const float r11 = (float)mCam[1][1]; // up.y      = M[1][1]
        const float r12 = (float)mCam[2][1]; // up.z      = M[2][1]
        const float r20 = (float)mCam[0][2]; // forward.x = M[0][2] (Natron -Z; negated below)
        const float r21 = (float)mCam[1][2]; // forward.y = M[1][2]
        const float r22 = (float)mCam[2][2]; // forward.z = M[2][2]

        // Cycles camera-to-world transform (row-major 3x4):
        // Column 0 = right axis (X), Column 1 = up axis (Y),
        // Column 2 = forward axis (Cycles +Z = Natron -Z, so negate),
        // Column 3 = position
        ccl::Transform cameraTfm;
        cameraTfm.x = ccl::make_float4( r00,  r10, -r20, (float)camTX);
        cameraTfm.y = ccl::make_float4( r01,  r11, -r21, (float)camTY);
        cameraTfm.z = ccl::make_float4( r02,  r12, -r22, (float)camTZ);

        scene->camera->set_matrix(cameraTfm);
        scene->camera->set_camera_type(ccl::CAMERA_PERSPECTIVE);

        // Independent fovH/fovV from both apertures via CameraMath. Cycles' default
        // landscape viewplane is (aspect, 1.0) which makes set_fov() be interpreted
        // as VERTICAL FOV with horizontal angle derived from render aspect — same
        // class of bug that bit ScanlineRender. Override the viewplane explicitly
        // so X extent reflects the camera's actual H aperture, not render aspect.
        double fovH = 0, fovV = 0;
        CameraMath::computeFovs(focalLength, hAperture, vAperture, fovH, fovV);
        float fovHf = (float)fovH;
        float fovVf = (float)fovV;
        if (fovVf < 0.01f || fovVf > 3.0f) { fovVf = 45.0f * (float)M_PI / 180.0f; fovHf = fovVf; }

        scene->camera->set_fov(fovVf); // Cycles uses this as the short-axis FOV.

        // Override viewplane so horizontal extent at z=1 = tan(fovH/2) instead of
        // aspect * tan(fovV/2). Vertical stays at ±1 (= tan(fovV/2) after fov scale).
        const float halfRatio = tanf(fovHf * 0.5f) / tanf(fovVf * 0.5f);
        scene->camera->viewplane.left   = -halfRatio;
        scene->camera->viewplane.right  =  halfRatio;
        scene->camera->viewplane.bottom = -1.0f;
        scene->camera->viewplane.top    =  1.0f;

        // Sensor metadata (in METERS — Cycles defaults are 0.036 / 0.024 for FF35).
        // Not used in projection math; exposed to OSL/shader Camera Data nodes.
        scene->camera->set_sensorwidth((float)hAperture * 0.001f);
        scene->camera->set_sensorheight((float)vAperture * 0.001f);

        scene->camera->set_full_width(_impl->width);
        scene->camera->set_full_height(_impl->height);
        scene->camera->set_nearclip(0.1f);
        scene->camera->set_farclip(10000.0f);

        // Depth of Field
        if (dof && dof->enabled) {
            scene->camera->set_aperturesize(dof->apertureSize);
            scene->camera->set_focaldistance(dof->focusDistance);
            scene->camera->set_blades(dof->blades);
            scene->camera->set_bladesrotation(dof->bladeRotation);
        }

        // Motion blur camera shutter
        if (motionBlur && motionBlur->enabled) {
            scene->camera->set_shuttertime(motionBlur->shutterTime);
            scene->camera->set_motion_position(
                (ccl::MotionPosition)motionBlur->shutterPosition);
        }

        scene->camera->update(scene);
    }

    // --- Render passes ---
    {
        // Standard AOV pass definitions
        struct PassDef { const char* name; ccl::PassType type; };
        static const PassDef standardPasses[] = {
            {"Combined",  ccl::PASS_COMBINED},
            {"DiffDir",   ccl::PASS_DIFFUSE_DIRECT},
            {"DiffInd",   ccl::PASS_DIFFUSE_INDIRECT},
            {"DiffCol",   ccl::PASS_DIFFUSE_COLOR},
            {"GlossDir",  ccl::PASS_GLOSSY_DIRECT},
            {"GlossInd",  ccl::PASS_GLOSSY_INDIRECT},
            {"GlossCol",  ccl::PASS_GLOSSY_COLOR},
            {"TransDir",  ccl::PASS_TRANSMISSION_DIRECT},
            {"TransInd",  ccl::PASS_TRANSMISSION_INDIRECT},
            {"TransCol",  ccl::PASS_TRANSMISSION_COLOR},
            {"Emit",      ccl::PASS_EMISSION},
            {"Env",       ccl::PASS_BACKGROUND},
            {"AO",        ccl::PASS_AO},
            {"Normal",    ccl::PASS_NORMAL},
            {"UV",        ccl::PASS_UV},
            {"Depth",     ccl::PASS_DEPTH},
            {"Mist",      ccl::PASS_MIST},
            // PASS_SHADOW_CATCHER_MATTE: when read via the pass accessor
            // with use_approximate_shadow_catcher=true, Cycles populates
            // this pass with "non-catcher objects + catcher with shadow
            // baked in" — the single-image equivalent of what Blender's
            // viewer shows when you tick the catcher checkbox and render.
            // We auto-request this internally when catchers exist and
            // copy it over Combined so users get the catcher-with-shadow
            // result in a single pass.
            {"ShadowCatcherMatte", ccl::PASS_SHADOW_CATCHER_MATTE},
            // Raw catcher accumulator (shadow ratio) + sample count — used for
            // diagnostics: lets us read what the catcher actually measured,
            // independent of the matte-with-shadow accessor math.
            {"ShadowCatcher", ccl::PASS_SHADOW_CATCHER},
            {"ShadowCatcherSampleCount", ccl::PASS_SHADOW_CATCHER_SAMPLE_COUNT},
        };

        // Build the set of passes to create. Always include "Combined".
        std::set<std::string> passesToCreate;
        passesToCreate.insert("Combined");
        for (const auto& pn : requestedPasses) {
            passesToCreate.insert(pn);
        }

        // Create requested standard passes
        printf("[Cycles AOV] Creating passes. Requested: %d\n", (int)passesToCreate.size());
        for (const auto& pd : standardPasses) {
            if (passesToCreate.find(pd.name) != passesToCreate.end()) {
                ccl::Pass* pass = scene->create_node<ccl::Pass>();
                pass->set_name(ccl::ustring(pd.name));
                pass->set_type(pd.type);
                printf("[Cycles AOV]   Created pass '%s' type=%d\n", pd.name, (int)pd.type);
            }
        }

        // --- Light group passes ---
        // Collect unique light group names from all lights in the scene
        std::set<std::string> lightGroupNames;
        const std::vector<SceneNode>& lgScan = sg.nodes();
        for (size_t li = 0; li < lgScan.size(); ++li) {
            const SceneNode& lsn = lgScan[li];
            if (lsn.type != eSceneNodeLight || !lsn.visible) continue;
            NodePtr lgNode = lsn.sourceNode.lock();
            if (!lgNode) continue;
            Light3D* lg3d = dynamic_cast<Light3D*>(lgNode->getEffectInstance().get());
            if (!lg3d) continue;
            std::string grp = lg3d->getLightGroup();
            if (!grp.empty()) {
                lightGroupNames.insert(grp);
            }
        }

        // Register light groups with the scene and create per-group Combined passes
        int nextLightGroupId = 0;
        for (const auto& groupName : lightGroupNames) {
            scene->lightgroups[ccl::ustring(groupName)] = nextLightGroupId++;

            // Create a Combined pass for this light group
            std::string lgPassName = "Combined_" + groupName;
            ccl::Pass* lgPass = scene->create_node<ccl::Pass>();
            lgPass->set_name(ccl::ustring(lgPassName));
            lgPass->set_type(ccl::PASS_COMBINED);
            lgPass->set_lightgroup(ccl::ustring(groupName));
            printf("[Cycles AOV]   Created lightgroup pass '%s' group='%s' id=%d\n",
                   lgPassName.c_str(), groupName.c_str(), nextLightGroupId - 1);
        }
    }

    scene->film->set_exposure(1.0f);
    // Transparent bg when no dome light (proper alpha for comp over).
    // When dome light present, show HDRI as visible background.
    // Transparent bg controls whether HDRI is visible in camera.
    // Dome light ALWAYS contributes lighting (scatter, reflections, etc).
    // "Renderable" on dome = camera visibility only (like Arnold's skydome Camera flag).
    scene->background->set_transparent(!hasDomeCameraVisible);
    scene->background->set_visibility(domeBgVisibility);  // dome out of reflections/diffuse/etc. if overridden

    // --- Integrator ---
    IntegratorParams integ;
    if (integrator) integ = *integrator;

    scene->integrator->set_max_bounce(integ.maxBounces);
    scene->integrator->set_max_diffuse_bounce(integ.diffuseBounces);
    scene->integrator->set_max_glossy_bounce(integ.glossyBounces);
    scene->integrator->set_max_transmission_bounce(integ.transmissionBounces);

    if (integ.aoFactor > 0.0f) {
        scene->integrator->set_ao_factor(integ.aoFactor);
        scene->integrator->set_ao_bounces(integ.aoBounces);
        scene->integrator->set_ao_distance(integ.aoDistance);
    }

    // Light sampling — reduces noise especially with HDRI environments
    scene->integrator->set_use_light_tree(true);

    // Motion blur
    if (motionBlur && motionBlur->enabled) {
        scene->integrator->set_motion_blur(true);
    }

    // Denoising (OpenImageDenoise, CPU). When on, Cycles auto-adds the denoising
    // albedo/normal aux passes and writes a DENOISED variant of each denoisable
    // pass; the output driver reads "Combined" which resolves to the denoised
    // result (scene passes default to PassMode::DENOISED, with automatic
    // fall-back to noisy when denoising is off).
    scene->integrator->set_use_denoise(_impl->denoise);
    if (_impl->denoise) {
        scene->integrator->set_denoiser_type(ccl::DENOISER_OPENIMAGEDENOISE);
        scene->integrator->set_denoise_use_gpu(false);  // CPU device
    }

    scene->integrator->tag_update(scene, ccl::Integrator::UPDATE_ALL);

    // Motion blur shutter timing — used by ParticleInstance, mesh, and any
    // other path that needs sub-time samples. Hoisted out of the per-branch
    // duplication so all paths agree.
    const bool  mbEnabled    = motionBlur && motionBlur->enabled;
    float       mbShutterOpen  = 0.0f;
    float       mbShutterClose = 0.0f;
    if (mbEnabled) {
        const float st = motionBlur->shutterTime;
        switch (motionBlur->shutterPosition) {
            case 0: mbShutterOpen = 0;          mbShutterClose = st;        break; // Start
            case 1: mbShutterOpen = -st * 0.5f; mbShutterClose = st * 0.5f; break; // Center
            case 2: mbShutterOpen = -st;        mbShutterClose = 0;         break; // End
        }
    }

    // --- Sync geometry from SceneGraph ---
    const std::vector<SceneNode>& sceneNodes = sg.nodes();
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (!sn.visible) continue;

        // --- Particles: render as PointCloud or instanced geo ---
        if (sn.type == eSceneNodeParticles) {
            NodePtr srcNode = sn.sourceNode.lock();
            if (!srcNode) continue;
            EffectInstancePtr effect = srcNode->getEffectInstance();
            if (!effect) continue;

            // If the source is a ParticleInstance, render the instanced geo via Cycles native instancing
            ParticleInstance* particleInstancer = dynamic_cast<ParticleInstance*>(effect.get());
            if (particleInstancer) {
                std::vector<ParticleInstance::GeoInstance> instances;
                particleInstancer->getInstances(time, instances);
                if (instances.empty()) continue;

                // Build prototype meshes once per geo source (Cycles instancing = shared ccl::Mesh)
                ccl::Mesh* protoMeshes[4] = {nullptr, nullptr, nullptr, nullptr};
                for (int g = 0; g < 4; ++g) {
                    EffectInstancePtr geoInput = particleInstancer->getInput(g + 1);
                    if (!geoInput) continue;

                    std::vector<ccl::float3> verts;
                    std::vector<int> tris;

                    // Cube3D: unit cube
                    if (dynamic_cast<Cube3D*>(geoInput.get())) {
                        float s = 0.5f;
                        verts = {
                            {-s,-s,-s}, {s,-s,-s}, {s,s,-s}, {-s,s,-s},
                            {-s,-s, s}, {s,-s, s}, {s,s, s}, {-s,s, s}
                        };
                        tris = {
                            0,1,2, 0,2,3, // back
                            4,6,5, 4,7,6, // front
                            0,4,5, 0,5,1, // bottom
                            2,6,7, 2,7,3, // top
                            0,3,7, 0,7,4, // left
                            1,5,6, 1,6,2  // right
                        };
                    }
                    // Sphere3D: simple sphere
                    else if (dynamic_cast<Sphere3D*>(geoInput.get())) {
                        const int rings = 16, sectors = 24;
                        float rad = 0.5f;
                        for (int r = 0; r <= rings; ++r) {
                            float phi = (float)M_PI * r / rings;
                            for (int s = 0; s <= sectors; ++s) {
                                float theta = 2.0f * (float)M_PI * s / sectors;
                                verts.push_back(ccl::make_float3(
                                    rad * std::sin(phi) * std::cos(theta),
                                    rad * std::cos(phi),
                                    rad * std::sin(phi) * std::sin(theta)));
                            }
                        }
                        for (int r = 0; r < rings; ++r) {
                            for (int s = 0; s < sectors; ++s) {
                                int i0 = r * (sectors + 1) + s;
                                int i1 = i0 + sectors + 1;
                                tris.push_back(i0); tris.push_back(i1); tris.push_back(i0 + 1);
                                tris.push_back(i0 + 1); tris.push_back(i1); tris.push_back(i1 + 1);
                            }
                        }
                    }
                    else {
                        continue; // unsupported geo type
                    }

                    // Create the prototype mesh
                    ccl::Mesh* mesh = scene->create_node<ccl::Mesh>();
                    mesh->reserve_mesh((int)verts.size(), (int)tris.size() / 3);
                    for (const auto& v : verts) mesh->add_vertex(v);
                    for (size_t t = 0; t < tris.size(); t += 3) {
                        mesh->add_triangle(tris[t], tris[t+1], tris[t+2], 0, true);
                    }

                    // Use the particle color shader (created per-instance below)
                    protoMeshes[g] = mesh;
                }

                // Create a shared particle shader
                ccl::Shader* instShader = scene->create_node<ccl::Shader>();
                auto instGraph = ccl::make_unique<ccl::ShaderGraph>();
                ccl::PrincipledBsdfNode* instBsdf = instGraph->create_node<ccl::PrincipledBsdfNode>();
                instBsdf->set_base_color(ccl::make_float3(0.8f, 0.5f, 0.2f));
                instBsdf->set_roughness(0.5f);
                instGraph->connect(instBsdf->output("BSDF"), instGraph->output()->input("Surface"));
                instShader->set_graph(std::move(instGraph));
                instShader->tag_update(scene);

                ccl::array<ccl::Node*> shaders;
                shaders.push_back_slow(instShader);
                for (int g = 0; g < 4; ++g) {
                    if (protoMeshes[g]) protoMeshes[g]->set_used_shaders(shaders);
                }

                // Motion blur shutter timing — now hoisted outside the loop.
                // Local aliases for readability inside this branch.
                const float shutterOpen  = mbShutterOpen;
                const float shutterClose = mbShutterClose;

                // Enable motion blur on all prototype meshes so Cycles allocates motion attribute space
                if (mbEnabled) {
                    for (int g = 0; g < 4; ++g) {
                        if (protoMeshes[g]) {
                            protoMeshes[g]->set_use_motion_blur(true);
                            protoMeshes[g]->set_motion_steps(3);
                        }
                    }
                }

                // Create one ccl::Object per instance (shared mesh = native Cycles instancing)
                for (const auto& inst : instances) {
                    if (inst.geoSourceIndex < 0 || inst.geoSourceIndex >= 4) continue;
                    ccl::Mesh* proto = protoMeshes[inst.geoSourceIndex];
                    if (!proto) continue;

                    // Helper to build a transform at a time offset (extrapolate position from velocity)
                    auto buildInstTfm = [&](float dt) -> ccl::Transform {
                        ccl::Transform t = ccl::transform_identity();
                        t = t * ccl::transform_translate(
                            inst.px + inst.vx * dt,
                            inst.py + inst.vy * dt,
                            inst.pz + inst.vz * dt);
                        // Extrinsic XYZ: build M = Rz * Ry * Rx (column-vector) so that
                        // applying M to a vector rotates Rx-then-Ry-then-Rz around world
                        // axes. Same convention as SceneGraph::buildTRS.
                        if (inst.rz != 0) t = t * ccl::transform_rotate(inst.rz * (float)M_PI / 180.0f, ccl::make_float3(0, 0, 1));
                        if (inst.ry != 0) t = t * ccl::transform_rotate(inst.ry * (float)M_PI / 180.0f, ccl::make_float3(0, 1, 0));
                        if (inst.rx != 0) t = t * ccl::transform_rotate(inst.rx * (float)M_PI / 180.0f, ccl::make_float3(1, 0, 0));
                        t = t * ccl::transform_scale(inst.sx, inst.sy, inst.sz);
                        return t;
                    };

                    ccl::Object* instObj = scene->create_node<ccl::Object>();
                    instObj->set_geometry(proto);
                    instObj->set_tfm(buildInstTfm(0.0f)); // center (current frame)

                    if (mbEnabled) {
                        // 3 motion steps: open, center, close
                        // (set_use_motion_blur is on the Mesh, already enabled above)
                        ccl::array<ccl::Transform> motionTfms;
                        motionTfms.resize(3);
                        motionTfms[0] = buildInstTfm(shutterOpen);
                        motionTfms[1] = buildInstTfm(0.0f);
                        motionTfms[2] = buildInstTfm(shutterClose);
                        instObj->set_motion(motionTfms);
                    }

                    instObj->set_color(ccl::make_float3(inst.r, inst.g, inst.b));
                    instObj->tag_update(scene);
                }

                continue; // done with this node
            }

            ParticleDataPtr particleData;
            ParticleProvider* provider = dynamic_cast<ParticleProvider*>(effect.get());
            if (provider) particleData = provider->getParticleData(time);

            if (!particleData || particleData->numParticles() == 0) continue;

            int count = particleData->numParticles();

            // Shading override: walk the particle chain (input 0, through
            // Dots) for a ParticleMaterial node. Found BEFORE geometry
            // creation because its Trails toggle selects curve geometry.
            ParticleMaterial* pmat = nullptr;
            {
                EffectInstancePtr cur = effect;
                for (int depth = 0; cur && depth < 64; ++depth) {
                    pmat = dynamic_cast<ParticleMaterial*>(cur.get());
                    if (pmat) break;
                    if (!dynamic_cast<ParticleProvider*>(cur.get())) break; // left the chain
                    cur = skipDots(cur->getInput(0));
                }
            }
            MaterialProvider* particleMatProv = pmat ? pmat->getParticleMaterialProvider() : nullptr;
            const bool wantTrails = pmat && pmat->getTrailsEnabled();

            fprintf(stderr, "[CyclesParticles] shader select: ParticleMaterial=%s material=%s tint=%d emission=%.2f trails=%d\n",
                    pmat ? "FOUND" : "none",
                    particleMatProv ? "connected" : "none",
                    pmat ? (int)pmat->getTintWithParticleColor() : -1,
                    pmat ? pmat->getEmissionStrength(time) : -1.0,
                    (int)wantTrails);
            fflush(stderr);

            ccl::Geometry* particleGeom = nullptr;
            ccl::PointCloud* pc = nullptr;
            ccl::Hair* hair = nullptr;

            // Per-key colors/emission gathered during trail construction.
            std::vector<ccl::float4> keyColors;
            std::vector<float> keyEmis;

            if (wantTrails) {
                // --- Trail mode: one curve per particle through its PAST
                // positions (matched by ID via the provider's frame cache).
                // Cycles renders these as camera-facing ribbons natively.
                const int trailLen = pmat->getTrailLength(time);
                const float headR = (float)pmat->getTrailHeadRadius(time);
                const float tailR = (float)pmat->getTrailTailRadius(time);
                const float tailFade = (float)pmat->getTrailTailFade(time);
                double ttr, ttg, ttb;
                pmat->getTrailTailTint(time, ttr, ttg, ttb);

                std::vector<std::unordered_map<uint32_t, const Particle*> > history;
                std::vector<ParticleDataPtr> historyData; // keeps snapshots alive
                for (int k = 1; k <= trailLen; ++k) {
                    ParticleDataPtr past = provider ? provider->getParticleData(time - k) : ParticleDataPtr();
                    if (!past || past->particles.empty()) break;
                    historyData.push_back(past);
                    history.push_back(std::unordered_map<uint32_t, const Particle*>());
                    std::unordered_map<uint32_t, const Particle*>& m = history.back();
                    m.reserve(past->particles.size());
                    for (size_t q = 0; q < past->particles.size(); ++q) {
                        m[past->particles[q].id] = &past->particles[q];
                    }
                }

                // Chains: head (current) then progressively older.
                std::vector<ccl::float3> keyPos;
                std::vector<float> keyRad;
                std::vector<int> curveFirstKey;
                std::vector<int> curveNumKeys;
                keyPos.reserve((size_t)count * (size_t)(trailLen + 1));
                keyRad.reserve(keyPos.capacity());
                keyColors.reserve(keyPos.capacity());
                keyEmis.reserve(keyPos.capacity());

                for (int pi = 0; pi < count; ++pi) {
                    const Particle& p = particleData->particles[pi];
                    const float baseR = (p.size > 0.001f ? p.size : 0.01f) * 0.5f;
                    const float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
                    const float headAlpha = p.a * (1.0f - ageFrac);

                    const int firstKey = (int)keyPos.size();
                    int nKeys = 0;

                    // head
                    keyPos.push_back(ccl::make_float3(p.px, p.py, p.pz));
                    keyRad.push_back(baseR * headR);
                    keyColors.push_back(ccl::make_float4(p.r, p.g, p.b, headAlpha));
                    keyEmis.push_back(p.emission);
                    ++nKeys;

                    for (size_t k = 0; k < history.size(); ++k) {
                        std::unordered_map<uint32_t, const Particle*>::const_iterator it = history[k].find(p.id);
                        if (it == history[k].end()) break;
                        const Particle& hp = *it->second;
                        const float t = (float)(k + 1) / (float)trailLen;
                        keyPos.push_back(ccl::make_float3(hp.px, hp.py, hp.pz));
                        keyRad.push_back(baseR * (headR + (tailR - headR) * t));
                        keyColors.push_back(ccl::make_float4(
                            p.r * (1.0f + ((float)ttr - 1.0f) * t),
                            p.g * (1.0f + ((float)ttg - 1.0f) * t),
                            p.b * (1.0f + ((float)ttb - 1.0f) * t),
                            headAlpha * (1.0f + (tailFade - 1.0f) * t)));
                        keyEmis.push_back(p.emission * (1.0f + (tailFade - 1.0f) * t));
                        ++nKeys;
                    }
                    if (nKeys < 2) {
                        // Newborn: synthesize a one-frame-back tail from velocity.
                        keyPos.push_back(ccl::make_float3(p.px - p.vx, p.py - p.vy, p.pz - p.vz));
                        keyRad.push_back(baseR * tailR);
                        keyColors.push_back(ccl::make_float4(p.r, p.g, p.b, headAlpha * tailFade));
                        keyEmis.push_back(p.emission * tailFade);
                        ++nKeys;
                    }
                    curveFirstKey.push_back(firstKey);
                    curveNumKeys.push_back(nKeys);
                }

                hair = scene->create_node<ccl::Hair>();
                hair->reserve_curves((int)curveFirstKey.size(), (int)keyPos.size());
                for (size_t ki = 0; ki < keyPos.size(); ++ki) {
                    hair->add_curve_key(keyPos[ki], keyRad[ki]);
                }
                for (size_t ci = 0; ci < curveFirstKey.size(); ++ci) {
                    hair->add_curve(curveFirstKey[ci], 0);
                }
                particleGeom = hair;
            } else {
                // --- Point mode (default): PointCloud geometry ---
                pc = scene->create_node<ccl::PointCloud>();
                pc->reserve(count);

                for (int pi = 0; pi < count; ++pi) {
                    const Particle& p = particleData->particles[pi];
                    pc->add_point(ccl::make_float3(p.px, p.py, p.pz),
                                  p.size > 0.001f ? p.size : 0.01f,
                                  0);
                }
                particleGeom = pc;
            }

            // Motion blur: velocity-extrapolated shutter positions (points
            // only — a trail IS the motion representation).
            if (pc && motionBlur && motionBlur->enabled) {
                // Compute shutter open/close times
                float shutterOpen = 0, shutterClose = 0;
                float st = motionBlur->shutterTime;
                switch (motionBlur->shutterPosition) {
                    case 0: // Start
                        shutterOpen = 0; shutterClose = st; break;
                    case 1: // Center
                        shutterOpen = -st * 0.5f; shutterClose = st * 0.5f; break;
                    case 2: // End
                        shutterOpen = -st; shutterClose = 0; break;
                }

                // Shutter positions from the CENTER frame's per-particle
                // velocity (displacement per frame) — the same approach as
                // ScanlineRender's stretch blur. Re-simulating at shutter
                // times returned index-SHIFTED arrays as particles die/spawn
                // (a streak connected two unrelated particles -> giant
                // criss-cross web), and providers snap to whole frames so the
                // streaks were a full frame long instead of a shutter
                // fraction. Velocity extrapolation is exact per particle.
                {
                    // Use 3 motion steps: open, center (implicit), close
                    pc->set_motion_steps(3);
                    pc->set_use_motion_blur(true);

                    // Motion attribute stores positions for step 0 (open) and step 1 (close)
                    // Center (step at index motion_steps/2) is the main points array
                    ccl::Attribute* motionAttr = pc->attributes.add(
                        ccl::ATTR_STD_MOTION_VERTEX_POSITION);
                    ccl::float4* motionData = motionAttr->data_float4();

                    for (int pi = 0; pi < count; ++pi) {
                        const Particle& p = particleData->particles[pi];
                        float r = p.size > 0.001f ? p.size : 0.01f;
                        motionData[pi] = ccl::make_float4(p.px + p.vx * shutterOpen,
                                                          p.py + p.vy * shutterOpen,
                                                          p.pz + p.vz * shutterOpen,
                                                          r);
                        motionData[count + pi] = ccl::make_float4(p.px + p.vx * shutterClose,
                                                                  p.py + p.vy * shutterClose,
                                                                  p.pz + p.vz * shutterClose,
                                                                  r);
                    }
                }
            }

            ccl::Shader* pShader = nullptr;
            if (particleMatProv) {
                // Full PBR material, optionally tinted by the per-particle
                // vertex color (age fade drives alpha). Emission Strength > 0
                // overrides the material's emission with particle glow.
                pShader = createMaterialShader(scene, particleMatProv, time,
                                               pmat->getTintWithParticleColor(),
                                               (float)pmat->getEmissionStrength(time));
            } else {
                // Default: per-particle color shader. ParticleMaterial's
                // knobs override the historic hardcoded values when present.
                const float emisStrength = pmat ? (float)pmat->getEmissionStrength(time) : 1.0f;
                const float roughVal     = pmat ? (float)pmat->getRoughness(time)        : 0.5f;
                const float metalVal     = pmat ? (float)pmat->getMetallic(time)         : 0.0f;

                pShader = scene->create_node<ccl::Shader>();
                auto pGraph = ccl::make_unique<ccl::ShaderGraph>();
                ccl::PrincipledBsdfNode* pBsdf = pGraph->create_node<ccl::PrincipledBsdfNode>();

                // Per-particle color attribute. NOTE: the name must NOT be
                // "vertex_color" — that is a RESERVED Cycles standard-attribute
                // name (ATTR_STD_VERTEX_COLOR); the AttributeNode converts it
                // into a std lookup which never matches a plain named
                // attribute, so the shader silently reads black.
                ccl::AttributeNode* colorAttr = pGraph->create_node<ccl::AttributeNode>();
                colorAttr->set_attribute(ccl::ustring("particle_color"));
                pGraph->connect(colorAttr->output("Color"), pBsdf->input("Base Color"));
                pGraph->connect(colorAttr->output("Color"), pBsdf->input("Emission Color"));
                if (wantTrails) {
                    // Trails carry a per-key alpha fade — honor it.
                    pGraph->connect(colorAttr->output("Alpha"), pBsdf->input("Alpha"));
                }
                // Per-particle emission (ParticleAttribute Emission section)
                // x the node/hardcoded strength.
                ccl::AttributeNode* emisAttr = pGraph->create_node<ccl::AttributeNode>();
                emisAttr->set_attribute(ccl::ustring("particle_emission"));
                ccl::MathNode* emisMul = pGraph->create_node<ccl::MathNode>();
                emisMul->set_math_type(ccl::NODE_MATH_MULTIPLY);
                emisMul->set_value2(emisStrength);
                pGraph->connect(emisAttr->output("Fac"), emisMul->input("Value1"));
                pGraph->connect(emisMul->output("Value"), pBsdf->input("Emission Strength"));
                pBsdf->set_roughness(roughVal);
                pBsdf->set_metallic(metalVal);

                pGraph->connect(pBsdf->output("BSDF"), pGraph->output()->input("Surface"));
                pShader->set_graph(std::move(pGraph));
            }
            pShader->tag_update(scene);

            ccl::array<ccl::Node*> used_shaders;
            used_shaders.push_back_slow(pShader);
            particleGeom->set_used_shaders(used_shaders);

            // Attributes ("particle_color" / "particle_emission" — see the
            // shader note about the reserved "vertex_color" standard name).
            if (hair) {
                ccl::Attribute* vcol = hair->attributes.add(ccl::ustring("particle_color"),
                                                            ccl::TypeRGBA,
                                                            ccl::ATTR_ELEMENT_CURVE_KEY);
                ccl::float4* colorData = vcol->data_float4();
                for (size_t ki = 0; ki < keyColors.size(); ++ki) colorData[ki] = keyColors[ki];

                ccl::Attribute* vemis = hair->attributes.add(ccl::ustring("particle_emission"),
                                                             ccl::TypeFloat,
                                                             ccl::ATTR_ELEMENT_CURVE_KEY);
                float* emisData = vemis->data_float();
                for (size_t ki = 0; ki < keyEmis.size(); ++ki) emisData[ki] = keyEmis[ki];
            } else {
                ccl::Attribute* vcol = pc->attributes.add(ccl::ustring("particle_color"),
                                                           ccl::TypeRGBA,
                                                           ccl::ATTR_ELEMENT_VERTEX);
                ccl::float4* colorData = vcol->data_float4();
                for (int pi = 0; pi < count; ++pi) {
                    const Particle& p = particleData->particles[pi];
                    float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
                    float alpha = p.a * (1.0f - ageFrac);
                    colorData[pi] = ccl::make_float4(p.r, p.g, p.b, alpha);
                }

                ccl::Attribute* vemis = pc->attributes.add(ccl::ustring("particle_emission"),
                                                            ccl::TypeFloat,
                                                            ccl::ATTR_ELEMENT_VERTEX);
                float* emisData = vemis->data_float();
                for (int pi = 0; pi < count; ++pi) {
                    emisData[pi] = particleData->particles[pi].emission;
                }
            }

            // Create object
            ccl::Object* obj = scene->create_node<ccl::Object>();
            obj->set_geometry(particleGeom);
            obj->set_tfm(ccl::transform_identity());

            // Apply CyclesRenderPass visibility if provided
            if (visibilityMap) {
                auto it = visibilityMap->find(sn.name);
                if (it != visibilityMap->end()) {
                    const ObjectVisibility& vis = it->second;
                    if (vis.isExcluded) {
                        obj->set_visibility(0);
                    } else {
                        obj->set_visibility(vis.rayVisibility);
                    }
                } else {
                    obj->set_visibility(0);
                }
            }

            obj->tag_update(scene);
            continue; // Skip mesh path
        }

        // --- Volumes: render via ccl::Volume ---
        if (sn.type == eSceneNodeVolume) {
            NodePtr srcNode = sn.sourceNode.lock();
            if (!srcNode) continue;
            EffectInstancePtr effect = srcNode->getEffectInstance();
            if (!effect) continue;

            Volume3D* vol3d = dynamic_cast<Volume3D*>(effect.get());
            ReadVDB* readVdb = dynamic_cast<ReadVDB*>(effect.get());

            // === ReadVDB: multi-grid VDB → PrincipledVolumeNode (fire/smoke) ===
            if (readVdb) {
                ReadVDB::VDBDirectData vd;
                if (!readVdb->getVDBDirect(time, vd) || vd.grids.empty()) continue;

                // 1. PrincipledVolume shader — handles density, temperature, blackbody natively
                ccl::Shader* volShader = scene->create_node<ccl::Shader>();
                {
                    auto g = ccl::make_unique<ccl::ShaderGraph>();
                    ccl::PrincipledVolumeNode* pvol = g->create_node<ccl::PrincipledVolumeNode>();

                    pvol->set_density_attribute(ccl::ustring(vd.bindDensity));
                    pvol->set_temperature_attribute(ccl::ustring(vd.bindTemperature));
                    pvol->set_color(ccl::make_float3(vd.colorR, vd.colorG, vd.colorB));
                    pvol->set_absorption_color(ccl::make_float3(
                        vd.absorptionR, vd.absorptionG, vd.absorptionB));
                    pvol->set_anisotropy(vd.anisotropy);
                    pvol->set_blackbody_intensity(vd.blackbodyIntensity);
                    pvol->set_blackbody_tint(ccl::make_float3(
                        vd.blackbodyTintR, vd.blackbodyTintG, vd.blackbodyTintB));
                    pvol->set_temperature(vd.temperatureScale);

                    // Density: ValueNode × remap curve → Density input
                    // CRITICAL: never call pvol->set_density() — that marks the socket
                    // constant and blocks the volume attribute from being read.
                    ccl::ValueNode* densVal = g->create_node<ccl::ValueNode>();
                    densVal->set_value(vd.density > 0.01f ? vd.density : 1.0f);

                    // Density remap curve (FloatCurveNode)
                    bool densRemapIsIdentity = true;
                    if ((int)vd.densityRemap.size() >= ReadVDB::VDBDirectData::REMAP_SAMPLES) {
                        const int N = ReadVDB::VDBDirectData::REMAP_SAMPLES;
                        for (int i = 0; i < N; ++i) {
                            float expected = (float)i / (float)(N - 1);
                            if (std::abs(vd.densityRemap[i] - expected) > 0.001f) {
                                densRemapIsIdentity = false;
                                break;
                            }
                        }
                    }
                    if (!densRemapIsIdentity) {
                        ccl::FloatCurveNode* densityCurve = g->create_node<ccl::FloatCurveNode>();
                        ccl::array<float> curveData;
                        curveData.resize(vd.densityRemap.size());
                        for (size_t i = 0; i < vd.densityRemap.size(); ++i)
                            curveData[i] = vd.densityRemap[i];
                        densityCurve->set_curve(curveData);
                        densityCurve->set_min_x(0.0f);
                        densityCurve->set_max_x(1.0f);
                        // Attribute("density") → FloatCurve → multiply by density knob → Density
                        ccl::AttributeNode* densAttr = g->create_node<ccl::AttributeNode>();
                        densAttr->set_attribute(ccl::ustring(vd.bindDensity));
                        g->connect(densAttr->output("Fac"), densityCurve->input("Value"));
                        ccl::MathNode* mul = g->create_node<ccl::MathNode>();
                        mul->set_math_type(ccl::NODE_MATH_MULTIPLY);
                        g->connect(densityCurve->output("Value"), mul->input("Value1"));
                        g->connect(densVal->output("Value"), mul->input("Value2"));
                        g->connect(mul->output("Value"), pvol->input("Density"));
                        printf("[Cycles Volume] Density remap curve active\n");
                    } else {
                        g->connect(densVal->output("Value"), pvol->input("Density"));
                    }

                    // Temperature remap curve (FloatCurveNode)
                    bool tempRemapIsIdentity = true;
                    if ((int)vd.temperatureRemap.size() >= ReadVDB::VDBDirectData::REMAP_SAMPLES) {
                        const int N = ReadVDB::VDBDirectData::REMAP_SAMPLES;
                        for (int i = 0; i < N; ++i) {
                            float expected = (float)i / (float)(N - 1);
                            if (std::abs(vd.temperatureRemap[i] - expected) > 0.001f) {
                                tempRemapIsIdentity = false;
                                break;
                            }
                        }
                    }
                    if (!tempRemapIsIdentity) {
                        ccl::FloatCurveNode* tempCurve = g->create_node<ccl::FloatCurveNode>();
                        ccl::array<float> curveData;
                        curveData.resize(vd.temperatureRemap.size());
                        for (size_t i = 0; i < vd.temperatureRemap.size(); ++i)
                            curveData[i] = vd.temperatureRemap[i];
                        tempCurve->set_curve(curveData);
                        tempCurve->set_min_x(0.0f);
                        tempCurve->set_max_x(1.0f);
                        ccl::AttributeNode* tempAttr = g->create_node<ccl::AttributeNode>();
                        tempAttr->set_attribute(ccl::ustring(vd.bindTemperature));
                        g->connect(tempAttr->output("Fac"), tempCurve->input("Value"));
                        g->connect(tempCurve->output("Value"), pvol->input("Temperature"));
                        printf("[Cycles Volume] Temperature remap curve active\n");
                    }

                    g->connect(pvol->output("Volume"), g->output()->input("Volume"));
                    volShader->set_graph(std::move(g));
                    volShader->tag_update(scene);
                }

                // 2. Create ccl::Volume
                ccl::Volume* volume = scene->create_node<ccl::Volume>();
                if (vd.stepSize > 0.001f) {
                    volume->set_step_size(vd.stepSize);
                }

                // 3. Assign shader + volume bounces
                ccl::array<ccl::Node*> shaders;
                shaders.push_back_slow(volShader);
                volume->set_used_shaders(shaders);
                scene->integrator->set_max_volume_bounce(vd.volumeBounces);

                // 4. Load ALL VDB grids as Cycles attributes
                ccl::ImageParams imgParams;
                imgParams.frame = 0.0f;

                for (const auto& gi : vd.grids) {
                    // Map grid name to standard Cycles attribute using user bindings
                    ccl::AttributeStandard std = ccl::ATTR_STD_NONE;
                    if (gi.name == vd.bindDensity) std = ccl::ATTR_STD_VOLUME_DENSITY;
                    else if (gi.name == vd.bindTemperature) std = ccl::ATTR_STD_VOLUME_TEMPERATURE;
                    else if (gi.name == vd.bindFlame) std = ccl::ATTR_STD_VOLUME_FLAME;
                    else if (gi.name == vd.bindColor) std = ccl::ATTR_STD_VOLUME_COLOR;

                    auto loader = ccl::make_unique<ccl::VDBImageLoader>(gi.grid, gi.name);
                    ccl::ImageHandle handle = scene->image_manager->add_image(
                        std::move(loader), imgParams, true);

                    if (std != ccl::ATTR_STD_NONE) {
                        ccl::Attribute* attr = volume->attributes.add(std);
                        attr->data_voxel() = handle;
                    } else {
                        ccl::Attribute* attr = volume->attributes.add(
                            ccl::ustring(gi.name), ccl::TypeFloat, ccl::ATTR_ELEMENT_VOXEL);
                        attr->data_voxel() = handle;
                    }

                    printf("[Cycles Volume]   grid '%s' loaded\n", gi.name.c_str());
                }

                // 5. Merge grids (velocity)
                volume->merge_grids(scene);

                // 6. Create object
                ccl::Object* obj = scene->create_node<ccl::Object>();
                obj->set_geometry(volume);
                obj->set_tfm(natronMatrixToCyclesTransform(sn.worldMatrix));

                if (visibilityMap) {
                    auto it = visibilityMap->find(sn.name);
                    if (it != visibilityMap->end())
                        obj->set_visibility(it->second.isExcluded ? 0 : it->second.rayVisibility);
                    else obj->set_visibility(0);
                }
                obj->tag_update(scene);
                printf("[Cycles Volume] ReadVDB '%s': %d grids, density=%.1f bb=%.1f temp=%.0fK\n",
                       sn.name.c_str(), (int)vd.grids.size(), vd.density,
                       vd.blackbodyIntensity, vd.temperatureScale);
                continue;
            }

            // === Volume3D path: ccl::Mesh with procedural shader ===
            if (!vol3d) continue;

            ccl::Mesh* mesh = scene->create_node<ccl::Mesh>();
            ccl::Object* obj = scene->create_node<ccl::Object>();
            obj->set_geometry(mesh);
            obj->set_tfm(natronMatrixToCyclesTransform(sn.worldMatrix));

            // Unit cube vertices [-1,1]^3
            ccl::array<ccl::float3> P;
            P.resize(8);
            P[0] = ccl::make_float3( 1, 1,-1); P[1] = ccl::make_float3( 1,-1,-1);
            P[2] = ccl::make_float3(-1,-1,-1); P[3] = ccl::make_float3(-1, 1,-1);
            P[4] = ccl::make_float3( 1, 1, 1); P[5] = ccl::make_float3( 1,-1, 1);
            P[6] = ccl::make_float3(-1,-1, 1); P[7] = ccl::make_float3(-1, 1, 1);
            mesh->set_verts(P);
            mesh->reserve_mesh(8, 12);
            const bool smooth = true;
            mesh->add_triangle(0,1,2, 0, smooth); mesh->add_triangle(0,2,3, 0, smooth);
            mesh->add_triangle(4,7,6, 0, smooth); mesh->add_triangle(4,6,5, 0, smooth);
            mesh->add_triangle(0,4,5, 0, smooth); mesh->add_triangle(0,5,1, 0, smooth);
            mesh->add_triangle(1,5,6, 0, smooth); mesh->add_triangle(1,6,2, 0, smooth);
            mesh->add_triangle(2,6,7, 0, smooth); mesh->add_triangle(2,7,3, 0, smooth);
            mesh->add_triangle(4,0,3, 0, smooth); mesh->add_triangle(4,3,7, 0, smooth);

            // Get Volume3D params
            Volume3D::VolumeParams vp = vol3d->getVolumeParams(time);
            float volDensity = vp.density;
            float volColorR = vp.colorR, volColorG = vp.colorG, volColorB = vp.colorB;
            int volType = vp.volumeType;
            float noiseScale = vp.noiseScale, noiseDetail = vp.noiseDetail;
            float stepSize = vp.stepSize;
            int volumeBounces = vp.volumeBounces;

            scene->integrator->set_max_volume_bounce(volumeBounces);

            // Procedural volume shader — shapes built from shader nodes (no VDB)
            ccl::Shader* volShader = scene->create_node<ccl::Shader>();
            if (stepSize > 0.001f) {
                volShader->set_volume_step_rate(stepSize);
            }
            auto volGraph = ccl::make_unique<ccl::ShaderGraph>();

            ccl::TextureCoordinateNode* texCoord = volGraph->create_node<ccl::TextureCoordinateNode>();

            ccl::MathNode* mul = volGraph->create_node<ccl::MathNode>();
            mul->set_math_type(ccl::NODE_MATH_MULTIPLY);
            mul->set_value2(volDensity);

            if (volType == 0) {
                // Sphere: max(0, 1 - length(pos))
                ccl::VectorMathNode* len = volGraph->create_node<ccl::VectorMathNode>();
                len->set_math_type(ccl::NODE_VECTOR_MATH_LENGTH);
                volGraph->connect(texCoord->output("Object"), len->input("Vector1"));

                ccl::MathNode* sub = volGraph->create_node<ccl::MathNode>();
                sub->set_math_type(ccl::NODE_MATH_SUBTRACT);
                sub->set_value1(1.0f);
                volGraph->connect(len->output("Value"), sub->input("Value2"));

                ccl::MathNode* clampMin = volGraph->create_node<ccl::MathNode>();
                clampMin->set_math_type(ccl::NODE_MATH_MAXIMUM);
                clampMin->set_value2(0.0f);
                volGraph->connect(sub->output("Value"), clampMin->input("Value1"));

                if (noiseScale > 0.01f) {
                    ccl::NoiseTextureNode* noise = volGraph->create_node<ccl::NoiseTextureNode>();
                    noise->set_dimensions(3);
                    noise->set_scale(noiseScale);
                    noise->set_detail(noiseDetail);
                    volGraph->connect(texCoord->output("Object"), noise->input("Vector"));

                    ccl::MathNode* noiseMul = volGraph->create_node<ccl::MathNode>();
                    noiseMul->set_math_type(ccl::NODE_MATH_MULTIPLY);
                    volGraph->connect(clampMin->output("Value"), noiseMul->input("Value1"));
                    volGraph->connect(noise->output("Fac"), noiseMul->input("Value2"));

                    volGraph->connect(noiseMul->output("Value"), mul->input("Value1"));
                } else {
                    volGraph->connect(clampMin->output("Value"), mul->input("Value1"));
                }
            } else {
                // Box: uniform density, optionally modulated by noise
                if (noiseScale > 0.01f) {
                    ccl::NoiseTextureNode* noise = volGraph->create_node<ccl::NoiseTextureNode>();
                    noise->set_dimensions(3);
                    noise->set_scale(noiseScale);
                    noise->set_detail(noiseDetail);
                    volGraph->connect(texCoord->output("Object"), noise->input("Vector"));
                    volGraph->connect(noise->output("Fac"), mul->input("Value1"));
                } else {
                    mul->set_value1(1.0f);
                }
            }

            ccl::ScatterVolumeNode* scatter = volGraph->create_node<ccl::ScatterVolumeNode>();
            scatter->set_color(ccl::make_float3(volColorR, volColorG, volColorB));
            volGraph->connect(mul->output("Value"), scatter->input("Density"));

            ccl::AbsorptionVolumeNode* absorb = volGraph->create_node<ccl::AbsorptionVolumeNode>();
            absorb->set_color(ccl::make_float3(volColorR, volColorG, volColorB));
            volGraph->connect(mul->output("Value"), absorb->input("Density"));

            ccl::AddClosureNode* addCl = volGraph->create_node<ccl::AddClosureNode>();
            volGraph->connect(scatter->output("Volume"), addCl->input("Closure1"));
            volGraph->connect(absorb->output("Volume"), addCl->input("Closure2"));
            volGraph->connect(addCl->output("Closure"), volGraph->output()->input("Volume"));

            volShader->set_graph(std::move(volGraph));
            volShader->tag_update(scene);

            ccl::array<ccl::Node*> used_shaders;
            used_shaders.push_back_slow(volShader);
            mesh->set_used_shaders(used_shaders);

            const char* typeNames[] = {"sphere", "box"};
            printf("[Cycles Volume] Created '%s' type=%s density=%.1f noise=%.1f\n",
                   sn.name.c_str(), typeNames[std::min(volType, 1)], volDensity, noiseScale);

            if (visibilityMap) {
                auto it = visibilityMap->find(sn.name);
                if (it != visibilityMap->end()) {
                    obj->set_visibility(it->second.isExcluded ? 0 : it->second.rayVisibility);
                } else {
                    obj->set_visibility(0);
                }
            }

            obj->tag_update(scene);
            continue;
        }

        std::vector<ccl::float3> verts;
        std::vector<int> triVerts;
        std::vector<ccl::float2> uvs; // per-vertex UVs

        switch (sn.type) {
            case eSceneNodeSphere:
                generateSphereMesh(verts, triVerts);
                // Generate equirectangular UVs for sphere
                uvs.resize(verts.size());
                for (size_t vi = 0; vi < verts.size(); ++vi) {
                    float x = verts[vi].x, y = verts[vi].y, z = verts[vi].z;
                    float len = sqrtf(x*x + y*y + z*z);
                    if (len > 1e-6f) { x /= len; y /= len; z /= len; }
                    float u = 0.5f + atan2f(x, z) / (2.0f * (float)M_PI);
                    float v = 0.5f - asinf(fminf(fmaxf(y, -1.0f), 1.0f)) / (float)M_PI;
                    uvs[vi] = ccl::make_float2(u, v);
                }
                break;
            case eSceneNodeCube:
                generateBoxMesh(verts, triVerts);
                // Per-face UVs for cube (each face gets 0-1 range)
                uvs.resize(verts.size());
                for (size_t vi = 0; vi < verts.size(); ++vi) {
                    // 4 verts per face, 6 faces = 24 verts
                    int face = (int)vi / 4;
                    int corner = (int)vi % 4;
                    float u = (corner == 0 || corner == 3) ? 0.0f : 1.0f;
                    float v = (corner < 2) ? 0.0f : 1.0f;
                    uvs[vi] = ccl::make_float2(u, v);
                }
                break;
            case eSceneNodeCard:
                generateQuadMesh(verts, triVerts);
                // Simple planar UVs for card
                uvs.resize(verts.size());
                if (verts.size() >= 4) {
                    uvs[0] = ccl::make_float2(0.0f, 0.0f);
                    uvs[1] = ccl::make_float2(1.0f, 0.0f);
                    uvs[2] = ccl::make_float2(1.0f, 1.0f);
                    uvs[3] = ccl::make_float2(0.0f, 1.0f);
                }
                break;
            case eSceneNodeCylinder:
                generateBoxMesh(verts, triVerts);
                break;
            case eSceneNodeMesh: {
                // Prefer mesh data carried directly on the SceneNode (set by
                // ReadGeo and ReadAlembicArchive). Fall back to the source-node
                // dynamic_cast for older code paths that haven't been updated.
                MeshDataPtr meshData = sn.meshData;
                if (!meshData) {
                    NodePtr meshSrcNode = sn.sourceNode.lock();
                    if (!meshSrcNode) continue;
                    ReadGeo* readGeo = dynamic_cast<ReadGeo*>(meshSrcNode->getEffectInstance().get());
                    if (!readGeo) continue;
                    meshData = readGeo->getMeshData(time);
                }
                if (!meshData || meshData->numVertices == 0) continue;

                // Copy vertices
                verts.resize(meshData->numVertices);
                const float* vtx = meshData->vertices.data();
                for (size_t vi = 0; vi < meshData->numVertices; ++vi) {
                    verts[vi] = ccl::make_float3(vtx[vi*3+0], vtx[vi*3+1], vtx[vi*3+2]);
                }

                // Triangulate n-gon faces using fan triangulation
                // Also build per-corner UVs for each output triangle
                triVerts.clear();
                std::vector<ccl::float2> triUVs; // per-corner UVs for triangulated mesh
                bool hasUV = meshData->hasUVs && !meshData->uvs.empty();
                const float* uvData = hasUV ? meshData->uvs.data() : nullptr;

                size_t idxOffset = 0;
                for (size_t f = 0; f < meshData->numFaces; ++f) {
                    int count = meshData->faceCounts[f];
                    for (int t = 1; t < count - 1; ++t) {
                        triVerts.push_back(meshData->faceIndices[idxOffset]);
                        triVerts.push_back(meshData->faceIndices[idxOffset + t]);
                        triVerts.push_back(meshData->faceIndices[idxOffset + t + 1]);

                        if (hasUV) {
                            // UVs are per face-vertex (indexed same as faceIndices)
                            size_t uv0 = (idxOffset) * 2;
                            size_t uv1 = (idxOffset + t) * 2;
                            size_t uv2 = (idxOffset + t + 1) * 2;
                            triUVs.push_back(ccl::make_float2(uvData[uv0], uvData[uv0+1]));
                            triUVs.push_back(ccl::make_float2(uvData[uv1], uvData[uv1+1]));
                            triUVs.push_back(ccl::make_float2(uvData[uv2], uvData[uv2+1]));
                        }
                    }
                    idxOffset += count;
                }

                // Use triUVs as per-corner UVs (overrides the per-vertex uvs path below)
                if (hasUV && !triUVs.empty()) {
                    uvs.clear(); // clear per-vertex uvs so the general path doesn't override
                    // Store triUVs in uvs — but mark as "already per-corner"
                    // We handle this specially below
                }

                // Store per-corner UVs for ReadGeo meshes in the uvs vector
                // We repurpose uvs to carry per-corner data when size matches triVerts
                if (hasUV && !triUVs.empty()) {
                    uvs.resize(triUVs.size());
                    for (size_t ti = 0; ti < triUVs.size(); ++ti) {
                        uvs[ti] = triUVs[ti];
                    }
                }
                break;
            }
            default:
                continue;
        }

        if (verts.empty()) continue;
        int numTris = (int)triVerts.size() / 3;

        // Per-object material shader. When a material override is in
        // effect, use it instead of the per-object MaterialProvider so
        // every renderable receives the same shader — the standard
        // clay-render / shadow-pass pattern. srcNode is hoisted out
        // because the motion-blur block below still consults it.
        NodePtr srcNode = sn.sourceNode.lock();
        NodePtr matOverrideNode = sn.materialNode.lock(); // per-part (GeoMaterialOverride)

        // Visibility / holdout / matte lookup key. The CyclesRenderPass object
        // tables (visibilityMap, holdoutObjects) key on each geo NODE's script
        // name (one row per node, from enumerateSceneGeo -> getScriptName_mt_safe).
        // A multi-emit ReadAlembicArchive emits one SceneNode per sub-mesh whose
        // name is "<archiveNode>/<entry/path>", which never matches the table key,
        // so every archive entry used to fall through to the "not in map" branch
        // and get set_visibility(0) — discovered but invisible. Map archive entries
        // back to their archive node so they inherit that node's row.
        std::string visKey = sn.name;
        if (sn.archiveEntryIdx >= 0 && srcNode) {
            visKey = srcNode->getScriptName_mt_safe();
        }

        // Reflection matte: in the second (matte) render pass, flagged objects
        // become pure white emitters so they read as a matte directly AND in
        // reflections (emission carries through glossy bounces).
        bool isMatteObj = false;
        if (visibilityMap) {
            auto vit = visibilityMap->find(visKey);
            if (vit != visibilityMap->end() && vit->second.reflectionMatte) isMatteObj = true;
        }
        ccl::Shader* objShader = nullptr;
        if (_impl->emissiveMatteMode && isMatteObj) {
            // Matte render: override this object's material with white emission.
            objShader = createEmissiveMatteShader(scene);
        } else if (materialOverride) {
            // Downstream / per-pass override wins (clay / shadow-pass pattern).
            objShader = createMaterialShader(scene, materialOverride, time);
        } else if (matOverrideNode) {
            // Per-part material override for this archive sub-object.
            MaterialProvider* mp = dynamic_cast<MaterialProvider*>(matOverrideNode->getEffectInstance().get());
            if (mp) {
                objShader = createMaterialShader(scene, mp, time);
            }
        } else if (srcNode) {
            MaterialProvider* matProv = dynamic_cast<MaterialProvider*>(srcNode->getEffectInstance().get());
            if (matProv) {
                objShader = createMaterialShader(scene, matProv, time);
            }
        }

        ccl::Mesh* mesh = scene->create_node<ccl::Mesh>();

        // Assign per-object shader
        if (objShader) {
            ccl::array<ccl::Node*> used_shaders;
            used_shaders.push_back_slow(objShader);
            mesh->set_used_shaders(used_shaders);
        }

        mesh->reserve_mesh(verts.size(), numTris);
        for (size_t v = 0; v < verts.size(); ++v) {
            mesh->add_vertex(verts[v]);
        }
        for (int t = 0; t < numTris; ++t) {
            mesh->add_triangle(triVerts[t * 3 + 0], triVerts[t * 3 + 1], triVerts[t * 3 + 2], 0, true);
        }

        // Add UV attribute (per-corner, 3 per triangle)
        if (!uvs.empty()) {
            ccl::Attribute* uvAttr = mesh->attributes.add(ccl::ATTR_STD_UV, ccl::ustring("UVMap"));
            ccl::float2* uvDst = uvAttr->data_float2();
            if ((int)uvs.size() == numTris * 3) {
                // Per-corner UVs (ReadGeo Alembic meshes) — copy directly
                for (int c = 0; c < numTris * 3; ++c) {
                    uvDst[c] = uvs[c];
                }
            } else {
                // Per-vertex UVs (procedural shapes) — index by vertex
                for (int t = 0; t < numTris; ++t) {
                    int i0 = triVerts[t*3+0], i1 = triVerts[t*3+1], i2 = triVerts[t*3+2];
                    uvDst[t*3+0] = (i0 < (int)uvs.size()) ? uvs[i0] : ccl::make_float2(0,0);
                    uvDst[t*3+1] = (i1 < (int)uvs.size()) ? uvs[i1] : ccl::make_float2(0,0);
                    uvDst[t*3+2] = (i2 < (int)uvs.size()) ? uvs[i2] : ccl::make_float2(0,0);
                }
            }
        }

        // Motion blur for animated meshes (vertex deformation only — Cycles
        // reads ATTR_STD_MOTION_VERTEX_POSITION and interpolates between the
        // shutter-open and shutter-close vertex positions). Works for both
        // ReadAlembicArchive entries (via archiveEntryIdx) and ReadGeo
        // single-mesh sources. Xform motion would need to re-evaluate the
        // SceneGraph parent chain at sub-time — deferred.
        if (mbEnabled && sn.type == eSceneNodeMesh && srcNode) {
            EffectInstancePtr effInst = srcNode->getEffectInstance();
            ReadAlembicArchive* abcArch = dynamic_cast<ReadAlembicArchive*>(effInst.get());
            ReadGeo*            readGeo = dynamic_cast<ReadGeo*>(effInst.get());

            // Number of vertices on the Cycles mesh — also the size in float3s
            // we expect from each sub-time query (3 floats per vertex).
            const size_t N = verts.size();

            // Lambda: fetch vertices at sub-time into outVerts. Returns false
            // if the result isn't usable. Note this mutates the shared
            // ReadAlembicArchive/ReadGeo mesh data in place; we copy the
            // vertices out immediately and restore the shared data to the
            // center time at the end of this block.
            auto fetchMeshAt = [&](double t, std::vector<float>& outVerts) -> bool {
                MeshDataPtr md;
                if (abcArch && sn.archiveEntryIdx >= 0) {
                    md = abcArch->getMeshDataAt(sn.archiveEntryIdx, t);
                } else if (readGeo) {
                    md = readGeo->getMeshData(t);
                }
                if (!md || md->numVertices != N) return false;
                outVerts = md->vertices; // deep copy
                return true;
            };

            std::vector<float> vertsOpen, vertsClose;
            const bool gotOpen  = fetchMeshAt(time + mbShutterOpen,  vertsOpen);
            const bool gotClose = fetchMeshAt(time + mbShutterClose, vertsClose);

            // Restore shared mesh data to center time so any subsequent reads
            // of sn.meshData (or the source node's _lastMeshData) get
            // consistent data.
            {
                std::vector<float> tmp;
                (void)fetchMeshAt(time, tmp);
            }

            if (gotOpen && gotClose &&
                vertsOpen.size()  == N * 3 &&
                vertsClose.size() == N * 3) {
                mesh->set_use_motion_blur(true);
                mesh->set_motion_steps(3);
                ccl::Attribute* attrMotion = mesh->attributes.add(ccl::ATTR_STD_MOTION_VERTEX_POSITION);
                ccl::float3* motionData = attrMotion->data_float3();
                // Layout: [step 0: open verts] [step 2: close verts] —
                // motion_steps = 3 means (steps - 1) = 2 motion samples, the
                // center step is the mesh's own verts (already set above).
                for (size_t vi = 0; vi < N; ++vi) {
                    motionData[vi] = ccl::make_float3(
                        vertsOpen[vi * 3 + 0],
                        vertsOpen[vi * 3 + 1],
                        vertsOpen[vi * 3 + 2]);
                }
                for (size_t vi = 0; vi < N; ++vi) {
                    motionData[N + vi] = ccl::make_float3(
                        vertsClose[vi * 3 + 0],
                        vertsClose[vi * 3 + 1],
                        vertsClose[vi * 3 + 2]);
                }
            }
        }

        ccl::Object* obj = scene->create_node<ccl::Object>();
        obj->set_geometry(mesh);
        obj->set_tfm(natronMatrixToCyclesTransform(sn.worldMatrix));

        // Apply CyclesRenderPass visibility if provided
        if (visibilityMap) {
            auto it = visibilityMap->find(visKey);
            if (it != visibilityMap->end()) {
                const ObjectVisibility& vis = it->second;
                if (scDebugEnabled())
                fprintf(stderr, "[Cycles SC] object '%s' in visMap: excluded=%d holdout=%d "
                       "shadowCatcher=%d rayVis=0x%X\n",
                       sn.name.c_str(), (int)vis.isExcluded, (int)vis.isHoldout,
                       (int)vis.isShadowCatcher, vis.rayVisibility);
                if (vis.isExcluded) {
                    obj->set_visibility(0);
                } else {
                    obj->set_visibility(vis.rayVisibility);
                    obj->set_use_holdout(vis.isHoldout);
                    obj->set_is_shadow_catcher(vis.isShadowCatcher);
                    if (vis.isShadowCatcher) {
                        // Approximate mode is required for the
                        // PASS_SHADOW_CATCHER_MATTE accessor to return
                        // the "matte + shadow approximated onto the
                        // catcher" combination — that's how we get a
                        // single-image catcher-with-shadow result.
                        // Without this flag the matte pass returns only
                        // non-catcher objects (the catcher is invisible)
                        // and shadow data lives only in the raw catcher
                        // accumulator which would need manual comp.
                        scene->film->set_use_approximate_shadow_catcher(true);
                        if (scDebugEnabled())
                        fprintf(stderr, "[Cycles SC] -> set_is_shadow_catcher(true) + approximate mode ON for '%s'\n",
                               sn.name.c_str());
                    }
                }
            } else {
                // Object not in any category — excluded
                if (scDebugEnabled())
                fprintf(stderr, "[Cycles SC] WARNING object '%s' NOT in visMap -> EXCLUDED (invisible). "
                       "Name mismatch? visMap has %d entries.\n",
                       sn.name.c_str(), (int)visibilityMap->size());
                obj->set_visibility(0);
            }
        }

        // Additive holdout: geo wired to the CyclesRender "holdout" input
        // renders as a Cycles holdout (a transparent matte of its shape),
        // independent of any CyclesRenderPass visMap above.
        if (holdoutObjects && holdoutObjects->count(visKey)) {
            obj->set_use_holdout(true);
        }

        obj->tag_update(scene);
    }
}

bool
CyclesRenderer::renderToBufferWithCamera(const SceneGraph& sg,
                                          double camTX, double camTY, double camTZ,
                                          double camRX, double camRY, double camRZ,
                                          double focalLength, double hAperture, double vAperture,
                                          std::vector<float>& outPixels,
                                          int width, int height, int samples,
                                          double time,
                                          DeepPixelData* outDeep,
                                          int deepMaxSamples,
                                          float deepMergeThreshold,
                                          float deepAlphaMergeThreshold)
{
    initialize(width, height, samples);
    syncSceneWithCamera(sg, camTX, camTY, camTZ, camRX, camRY, camRZ, focalLength, hAperture, vAperture, time);

    // Deep output: enable the film-side kernel feature and install the deep
    // driver BEFORE the render starts — PathTrace::render() syncs the deep
    // device buffers and kernel pointers automatically each render.
    if (outDeep) {
        _impl->session->scene->film->set_use_deep_output(true);
        _impl->session->scene->film->set_deep_max_samples(deepMaxSamples);
        _impl->session->scene->film->tag_modified();

        auto deepDriver = ccl::make_unique<ccl::DeepOutputDriver>(_impl->session->device.get());
        deepDriver->set_enabled(true);
        deepDriver->set_merge_threshold(deepMergeThreshold);
        deepDriver->set_alpha_merge_threshold(deepAlphaMergeThreshold);
        deepDriver->reset(width, height, deepMaxSamples);
        _impl->session->set_deep_output_driver(std::move(deepDriver));
    } else if (_impl->session->scene->film->get_use_deep_output()) {
        // A previous deep render on this reused session must not keep paying
        // the kernel-side accumulation cost.
        _impl->session->scene->film->set_use_deep_output(false);
        _impl->session->scene->film->tag_modified();
        _impl->session->set_deep_output_driver(nullptr);
    }

    _impl->session->set_output_driver(
        ccl::make_unique<ccl::NatronBufferOutputDriver>(&outPixels, width, height));

    startRender();
    waitForRender();

    // See the multipass path: never return a cancelled render as success —
    // Natron would cache the black frame.
    if (_impl->session->progress.get_cancel()) {
        fprintf(stderr, "[CyclesRenderer] render cancelled mid-flight — discarding partial buffer (not cached)\n");
        fflush(stderr);
        return false;
    }

    // Harvest deep samples: hand the Combined pass to the driver as the
    // beauty buffer (Deep Recolor distributes its RGB into the samples via
    // log-domain alpha scaling), then copy out the processed per-pixel lists.
    // NatronBufferOutputDriver stores Cycles-native bottom-up rows, which is
    // what the driver's global pixel indexing expects — no flip here (the
    // DeepImage conversion in the caller flips to top-down).
    if (outDeep) {
        outDeep->clear();
        ccl::DeepOutputDriver* d = _impl->session->get_deep_output_driver();
        if (d && !outPixels.empty()) {
            d->set_beauty_buffer(outPixels.data(), width, height);
            std::unique_ptr<std::vector<std::vector<blender::DeepSample>>> processed(
                d->get_processed_deep_data());
            if (processed && (int)processed->size() == width * height) {
                outDeep->resize(processed->size());
                for (std::size_t i = 0; i < processed->size(); ++i) {
                    const std::vector<blender::DeepSample>& src = (*processed)[i];
                    std::vector<DeepPixelSample>& dst = (*outDeep)[i];
                    dst.resize(src.size());
                    for (std::size_t s = 0; s < src.size(); ++s) {
                        dst[s].r = src[s].r;
                        dst[s].g = src[s].g;
                        dst[s].b = src[s].b;
                        dst[s].a = src[s].a;
                        dst[s].z = src[s].z;
                        dst[s].zback = src[s].z_back;
                    }
                }
            }
        }
    }

    return !outPixels.empty();
}

bool
CyclesRenderer::renderToBufferWithCameraMultiPass(const SceneGraph& sg,
                                                   double camTX, double camTY, double camTZ,
                                                   double camRX, double camRY, double camRZ,
                                                   double focalLength, double hAperture, double vAperture,
                                                   const std::vector<std::string>& requestedPasses,
                                                   std::map<std::string, std::vector<float>>& outPassBuffers,
                                                   int width, int height, int samples,
                                                   double time,
                                                   const std::map<std::string, ObjectVisibility>* visibilityMap,
                                                   const std::set<std::string>* activeLights,
                                                   const DOFParams* dof,
                                                   const MotionBlurParams* motionBlur,
                                                   const IntegratorParams* integrator,
                                                   MaterialProvider* materialOverride,
                                                   const std::set<std::string>* holdoutObjects,
                                                   DeepPixelData* outDeep,
                                                   int deepMaxSamples,
                                                   float deepMergeThreshold,
                                                   float deepAlphaMergeThreshold)
{
    initialize(width, height, samples);

    // Detect shadow-catcher objects so we can auto-inject the
    // PASS_SHADOW_CATCHER buffer for the post-render composite.
    // Catcher contributions are routed OUT of PASS_COMBINED by Cycles
    // whenever any object has the catcher flag set; without summing the
    // catcher pass back into Combined ourselves, the catcher disappears
    // from the output entirely.
    bool hasShadowCatcher = false;
    int shadowCatcherCount = 0;
    if (visibilityMap) {
        for (const auto& kv : *visibilityMap) {
            if (kv.second.isShadowCatcher) {
                hasShadowCatcher = true;
                ++shadowCatcherCount;
            }
        }
    }
    if (scDebugEnabled())
    fprintf(stderr, "[Cycles SC] renderMultiPass: visMap=%s entries=%d shadowCatchers=%d hasShadowCatcher=%d\n",
           visibilityMap ? "present" : "NULL",
           visibilityMap ? (int)visibilityMap->size() : 0,
           shadowCatcherCount, (int)hasShadowCatcher);
    // Track whether the caller explicitly asked for the matte pass as
    // a standalone AOV — affects whether we keep it after the composite.
    const bool userRequestedMatte = std::find(requestedPasses.begin(),
                                                requestedPasses.end(),
                                                std::string("ShadowCatcherMatte")) != requestedPasses.end();
    std::vector<std::string> effectiveRequested = requestedPasses;
    if (hasShadowCatcher && !userRequestedMatte) {
        effectiveRequested.push_back("ShadowCatcherMatte");
    }

    // Reflection matte: "ReflectionMatte" is NOT a Cycles pass — it's produced by
    // a second render (below) where flagged objects become white emitters. Pull it
    // out of the main render's pass list and remember it was requested.
    bool hasMatteObjects = false;
    if (visibilityMap) {
        for (const auto& kv : *visibilityMap) {
            if (kv.second.reflectionMatte) { hasMatteObjects = true; break; }
        }
    }
    const bool wantReflectionMatte =
        std::find(requestedPasses.begin(), requestedPasses.end(),
                  std::string("ReflectionMatte")) != requestedPasses.end();
    effectiveRequested.erase(
        std::remove(effectiveRequested.begin(), effectiveRequested.end(),
                    std::string("ReflectionMatte")),
        effectiveRequested.end());

    // Combined Diffuse/Glossy/Transmission planes: Cycles' kernel only writes the
    // direct/indirect/color sub-passes, never the category pass. We synthesize the
    // beauty-matching contribution = (Dir + Ind) * Col after the render, so for each
    // requested combined plane we make sure its three sub-passes are rendered. Track
    // the ones we add purely for the synthesis so we can drop them if the user didn't
    // ask for them. (combinedPlanes stays in scope for the synthesis pass below.)
    struct CombinedPlane {
        const char* name; const char* dir; const char* ind; const char* col;
        bool wanted = false; bool addedDir = false, addedInd = false, addedCol = false;
    };
    CombinedPlane combinedPlanes[] = {
        {"Diffuse",      "DiffDir",  "DiffInd",  "DiffCol"},
        {"Glossy",       "GlossDir", "GlossInd", "GlossCol"},
        {"Transmission", "TransDir", "TransInd", "TransCol"},
    };
    for (CombinedPlane& cp : combinedPlanes) {
        cp.wanted = std::find(requestedPasses.begin(), requestedPasses.end(),
                              std::string(cp.name)) != requestedPasses.end();
        effectiveRequested.erase(
            std::remove(effectiveRequested.begin(), effectiveRequested.end(),
                        std::string(cp.name)),
            effectiveRequested.end());
        if (!cp.wanted) continue;
        auto ensurePass = [&](const char* nm, bool& added) {
            if (std::find(effectiveRequested.begin(), effectiveRequested.end(),
                          std::string(nm)) == effectiveRequested.end()) {
                effectiveRequested.push_back(nm);
                added = true;
            }
        };
        ensurePass(cp.dir, cp.addedDir);
        ensurePass(cp.ind, cp.addedInd);
        ensurePass(cp.col, cp.addedCol);
    }
    // Diagnostic only (NATRON_DEBUG_SC): also pull the raw catcher accumulator
    // + sample count so the composite block below can report them. Users can
    // still request these as explicit AOVs — that path is unaffected.
    if (hasShadowCatcher && scDebugEnabled()) {
        effectiveRequested.push_back("ShadowCatcher");
        effectiveRequested.push_back("ShadowCatcherSampleCount");
    }

    syncSceneWithCamera(sg, camTX, camTY, camTZ, camRX, camRY, camRZ,
                        focalLength, hAperture, vAperture, time, effectiveRequested,
                        visibilityMap, activeLights, dof, motionBlur, integrator,
                        materialOverride, holdoutObjects);

    // Build the full list of pass names for the output driver.
    // This includes the requested standard passes plus any light group Combined passes.
    std::vector<std::string> allPassNames;

    // Always include Combined
    allPassNames.push_back("Combined");
    for (const auto& pn : effectiveRequested) {
        if (pn != "Combined") {
            allPassNames.push_back(pn);
        }
    }

    // Collect light group pass names from the scene
    const std::vector<SceneNode>& lgNodes = sg.nodes();
    std::set<std::string> lgNames;
    for (size_t i = 0; i < lgNodes.size(); ++i) {
        const SceneNode& sn = lgNodes[i];
        if (sn.type != eSceneNodeLight || !sn.visible) continue;
        NodePtr lgNode = sn.sourceNode.lock();
        if (!lgNode) continue;
        Light3D* lg3d = dynamic_cast<Light3D*>(lgNode->getEffectInstance().get());
        if (!lg3d) continue;
        std::string grp = lg3d->getLightGroup();
        if (!grp.empty()) lgNames.insert(grp);
    }
    for (const auto& grp : lgNames) {
        allPassNames.push_back("Combined_" + grp);
    }

    // Deep output: enable the kernel feature + install the deep driver before
    // the render; PathTrace syncs the deep device buffers automatically.
    if (outDeep) {
        _impl->session->scene->film->set_use_deep_output(true);
        _impl->session->scene->film->set_deep_max_samples(deepMaxSamples);
        _impl->session->scene->film->tag_modified();
        auto deepDriver = ccl::make_unique<ccl::DeepOutputDriver>(_impl->session->device.get());
        deepDriver->set_enabled(true);
        deepDriver->set_merge_threshold(deepMergeThreshold);
        deepDriver->set_alpha_merge_threshold(deepAlphaMergeThreshold);
        deepDriver->reset(width, height, deepMaxSamples);
        _impl->session->set_deep_output_driver(std::move(deepDriver));
    } else if (_impl->session->scene->film->get_use_deep_output()) {
        _impl->session->scene->film->set_use_deep_output(false);
        _impl->session->scene->film->tag_modified();
        _impl->session->set_deep_output_driver(nullptr);
    }

    _impl->session->set_output_driver(
        ccl::make_unique<ccl::NatronMultiPassOutputDriver>(&outPassBuffers, allPassNames, width, height));

    startRender();
    waitForRender();

    // A cancelled session returns from wait() with an empty/partial buffer.
    // Returning success would let Natron CACHE a black frame (seen as
    // "particles visible at one zoom level, black at another" — different
    // mip entries cached from different renders). Fail instead; the host
    // re-requests when it actually needs the frame.
    if (_impl->session->progress.get_cancel()) {
        fprintf(stderr, "[CyclesRenderer] render cancelled mid-flight — discarding partial buffer (not cached)\n");
        fflush(stderr);
        return false;
    }

    // Harvest deep samples BEFORE any host-side composite mutates Combined —
    // Deep Recolor must distribute the kernel's own beauty into the samples.
    if (outDeep) {
        outDeep->clear();
        ccl::DeepOutputDriver* d = _impl->session->get_deep_output_driver();
        auto itBeauty = outPassBuffers.find("Combined");
        if (d && itBeauty != outPassBuffers.end() && !itBeauty->second.empty()) {
            d->set_beauty_buffer(itBeauty->second.data(), width, height);
            std::unique_ptr<std::vector<std::vector<blender::DeepSample>>> processed(
                d->get_processed_deep_data());
            if (processed && (int)processed->size() == width * height) {
                outDeep->resize(processed->size());
                for (std::size_t i = 0; i < processed->size(); ++i) {
                    const std::vector<blender::DeepSample>& srcPix = (*processed)[i];
                    std::vector<DeepPixelSample>& dstPix = (*outDeep)[i];
                    dstPix.resize(srcPix.size());
                    for (std::size_t sIdx = 0; sIdx < srcPix.size(); ++sIdx) {
                        dstPix[sIdx].r = srcPix[sIdx].r;
                        dstPix[sIdx].g = srcPix[sIdx].g;
                        dstPix[sIdx].b = srcPix[sIdx].b;
                        dstPix[sIdx].a = srcPix[sIdx].a;
                        dstPix[sIdx].z = srcPix[sIdx].z;
                        dstPix[sIdx].zback = srcPix[sIdx].z_back;
                    }
                }
            }
        }
    }

    // Render-time shadow-catcher composite. With approximate mode on,
    // Cycles' PASS_SHADOW_CATCHER_MATTE accessor encodes:
    //   matte.rgb   = non-catcher objects only (the sphere in test).
    //   matte.alpha = sphere alpha (=1 over sphere) blended with
    //                 (1 - shadow_multiplier) over catcher pixels —
    //                 so catcher unshadowed = 0, catcher shadowed =
    //                 shadow_factor in [0,1].
    // To make the catcher VISIBLE in a single-pass output the way
    // Blender's viewer shows it, we alpha-over the matte onto a white
    // backdrop. After the over:
    //   sphere pixels  → sphere.rgb (alpha=1, backdrop contributes 0).
    //   catcher shadow → matte.rgb + (1-shadow_factor)*white. Where
    //                    matte.rgb is ~0 at catcher pixels, this comes
    //                    out as (1-shadow_factor)*white → bright in
    //                    unshadowed parts, dark in shadowed parts.
    //   empty bg       → also becomes white (acceptable for the standard
    //                    shadow-pass workflow — user can mask it out in
    //                    comp using the original alpha if they want
    //                    transparent background).
    if (hasShadowCatcher) {
        auto itC = outPassBuffers.find("Combined");
        auto itM = outPassBuffers.find("ShadowCatcherMatte");
        if (scDebugEnabled())
        fprintf(stderr, "[Cycles SC] composite: Combined=%s ShadowCatcherMatte=%s\n",
               itC != outPassBuffers.end() ? "found" : "MISSING",
               itM != outPassBuffers.end() ? "found" : "MISSING");
        if (itC != outPassBuffers.end() && itM != outPassBuffers.end()) {
            std::vector<float>& combined = itC->second;
            const std::vector<float>& matte = itM->second;
            const size_t nPix = std::min(combined.size(), matte.size()) / 4;
            if (scDebugEnabled()) {
                float mnA = 1e30f, mxA = -1e30f; int nzA = 0;
                for (size_t i = 0; i < nPix; ++i) {
                    const float a = matte[i * 4 + 3];
                    if (a != 0.0f) nzA++;
                    if (a > 0.001f && a < 1e9f) { if (a < mnA) mnA = a; if (a > mxA) mxA = a; }
                }
                fprintf(stderr, "[Cycles SC] matte ALPHA range=[%.4f, %.4f] nonZeroAlpha=%d/%d (shadow lives here)\n",
                       mnA, mxA, nzA, (int)nPix);
            }
            // Diagnostic: report the RAW catcher accumulator. If this is ~1.0
            // everywhere the catcher measured no shadow (problem is upstream in
            // the catcher path); if it dips below 1.0 under the sphere the
            // shadow IS measured and the matte math/read is dropping it.
            if (scDebugEnabled()) {
                auto itR = outPassBuffers.find("ShadowCatcher");
                if (itR != outPassBuffers.end()) {
                    const std::vector<float>& raw = itR->second;
                    const size_t rPix = raw.size() / 4;
                    float mn = 1e30f, mx = -1e30f; int below = 0;
                    for (size_t i = 0; i < rPix; ++i) {
                        const float v = raw[i * 4];
                        if (v < mn) mn = v; if (v > mx) mx = v;
                        if (v > 0.0f && v < 0.99f) below++;
                    }
                    fprintf(stderr, "[Cycles SC] RAW ShadowCatcher ch0 range=[%.4f, %.4f] pixelsBelow1.0=%d/%d\n",
                           mn, mx, below, (int)rPix);
                } else {
                    fprintf(stderr, "[Cycles SC] RAW ShadowCatcher pass NOT in buffers\n");
                }
                // Sample count: if this is 0 on the card, the catcher object
                // pass never accumulated -> the split isn't happening, which is
                // why the ratio is a flat 1.0.
                auto itSC = outPassBuffers.find("ShadowCatcherSampleCount");
                if (itSC != outPassBuffers.end()) {
                    const std::vector<float>& sc = itSC->second;
                    const size_t sPix = sc.size() / 4;
                    int withSamples = 0; float mx = 0;
                    for (size_t i = 0; i < sPix; ++i) {
                        const float v = sc[i * 4];
                        if (v > 0.0f) withSamples++;
                        if (v > mx) mx = v;
                    }
                    fprintf(stderr, "[Cycles SC] SampleCount pixelsWithCatcherSamples=%d/%d maxCount=%.1f\n",
                           withSamples, (int)sPix, mx);
                } else {
                    fprintf(stderr, "[Cycles SC] SampleCount pass NOT in buffers\n");
                }
            }
            // Output the shadow-catcher matte directly as a PREMULTIPLIED comp
            // pass: RGB = non-catcher objects (the sphere), ALPHA = shadow
            // density on the (invisible) catcher + object coverage. Background
            // and unshadowed catcher stay transparent (alpha 0). This comps
            // over a backplate as plate*(1-A) + RGB — the shadow darkens the
            // plate and the sphere overs on top. (Previously this flattened the
            // matte onto a white backdrop with alpha=1, which destroyed the
            // alpha and turned the background white.)
            for (size_t i = 0; i < nPix; ++i) {
                const size_t off = i * 4;
                combined[off + 0] = matte[off + 0];
                combined[off + 1] = matte[off + 1];
                combined[off + 2] = matte[off + 2];
                combined[off + 3] = matte[off + 3];
            }
            if (!userRequestedMatte) {
                outPassBuffers.erase(itM);
            }
        }
    }

    // --- Combined Diffuse/Glossy/Transmission planes: (Dir + Ind) * Col, per pixel. ---
    // The light sub-passes are HDR (incoming radiance), the color pass is albedo; their
    // product is the contribution as it lands in the beauty. Empty sub-passes are
    // zero-filled, so a missing direct/indirect lobe is harmless.
    for (CombinedPlane& cp : combinedPlanes) {
        if (!cp.wanted) continue;
        auto pd = outPassBuffers.find(cp.dir);
        auto pi = outPassBuffers.find(cp.ind);
        auto pc = outPassBuffers.find(cp.col);
        if (pd != outPassBuffers.end() && pi != outPassBuffers.end() && pc != outPassBuffers.end()) {
            const std::vector<float>& d = pd->second;
            const std::vector<float>& in = pi->second;
            const std::vector<float>& c = pc->second;
            const size_t n = std::min(d.size(), std::min(in.size(), c.size()));
            std::vector<float> combined(n, 0.0f);
            const size_t nPix = n / 4;
            for (size_t i = 0; i < nPix; ++i) {
                const size_t o = i * 4;
                for (int ch = 0; ch < 3; ++ch) {
                    combined[o + ch] = (d[o + ch] + in[o + ch]) * c[o + ch];
                }
                combined[o + 3] = 1.0f;
            }
            outPassBuffers[cp.name] = std::move(combined);
        }
        // Drop the sub-passes we pulled in only to build the combined plane.
        if (cp.addedDir) outPassBuffers.erase(cp.dir);
        if (cp.addedInd) outPassBuffers.erase(cp.ind);
        if (cp.addedCol) outPassBuffers.erase(cp.col);
    }

    // --- Reflection matte: second render with flagged objects as white emitters. ---
    // Cycles shader AOVs only write on the primary camera ray, so they can't carry a
    // matte through reflections. Instead we re-render the same scene with the flagged
    // objects turned into pure emitters (emission DOES show in reflections) and route
    // that render's Combined into the "ReflectionMatte" plane. This roughly doubles
    // render time, but only when the matte is actually requested + objects are flagged.
    if (wantReflectionMatte && hasMatteObjects) {
        _impl->emissiveMatteMode = true;
        std::vector<std::string> mattePasses;
        mattePasses.push_back("Combined");
        syncSceneWithCamera(sg, camTX, camTY, camTZ, camRX, camRY, camRZ,
                            focalLength, hAperture, vAperture, time, mattePasses,
                            visibilityMap, activeLights, dof, motionBlur, integrator,
                            materialOverride, holdoutObjects);

        std::map<std::string, std::vector<float>> matteBuffers;
        std::vector<std::string> matteAll;
        matteAll.push_back("Combined");
        _impl->session->set_output_driver(
            ccl::make_unique<ccl::NatronMultiPassOutputDriver>(&matteBuffers, matteAll, width, height));
        startRender();
        waitForRender();
        _impl->emissiveMatteMode = false;

        // Cancelled second pass -> fail the whole render (no black cache).
        if (_impl->session->progress.get_cancel()) {
            fprintf(stderr, "[CyclesRenderer] matte pass cancelled mid-flight — discarding\n");
            fflush(stderr);
            return false;
        }

        auto itMC = matteBuffers.find("Combined");
        if (itMC != matteBuffers.end()) {
            outPassBuffers["ReflectionMatte"] = std::move(itMC->second);
        }
    }

    return !outPassBuffers.empty();
}

void
CyclesRenderer::setSamples(int samples)
{
    _impl->samples = samples;
}

void
CyclesRenderer::setDenoise(bool enabled)
{
    _impl->denoise = enabled;
}

int
CyclesRenderer::getWidth() const
{
    return _impl->width;
}

int
CyclesRenderer::getHeight() const
{
    return _impl->height;
}

// Map a case-insensitive copy of `in` for comparisons (channels / format
// names from JSON come in with arbitrary casing).
static std::string toLowerCopy_(const std::string& in)
{
    std::string out = in;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Map the ExrOutputOptions strings to (TypeDesc, OIIO compression name).
// Unknown values silently fall back to the defaults (FLOAT + zip).
static void resolveExrOptions_(const CyclesRenderer::ExrOutputOptions& opts,
                                OIIO::TypeDesc& outType,
                                std::string& outCompression)
{
    outType        = OIIO::TypeDesc::FLOAT;
    outCompression = "zip";

    const std::string b = toLowerCopy_(opts.bitDepth);
    if (b.find("half") != std::string::npos || b.find("16") != std::string::npos) {
        outType = OIIO::TypeDesc::HALF;
    } else if (b.find("full") != std::string::npos || b.find("32") != std::string::npos) {
        outType = OIIO::TypeDesc::FLOAT;
    }
    // "8-bit Integer" is invalid for EXR — keep default float.

    const std::string c = toLowerCopy_(opts.compression);
    if      (c.find("zips")  != std::string::npos) outCompression = "zips";
    else if (c.find("zip")   != std::string::npos) outCompression = "zip";
    else if (c.find("piz")   != std::string::npos) outCompression = "piz";
    else if (c.find("dwaa")  != std::string::npos) outCompression = "dwaa";
    else if (c.find("dwab")  != std::string::npos) outCompression = "dwab";
    else if (c.find("pxr24") != std::string::npos) outCompression = "pxr24";
    else if (c.find("b44a")  != std::string::npos) outCompression = "b44a";
    else if (c.find("b44")   != std::string::npos) outCompression = "b44";
    else if (c.find("rle")   != std::string::npos) outCompression = "rle";
    else if (c.find("none")  != std::string::npos) outCompression = "none";
}

// Convert the first 3 (RGB) channels of an interleaved buffer in place from the
// config's scene_linear space to `target`, via OCIO (OIIO::ColorConfig). No-op
// (returns false) when target is empty, "Raw", the scene_linear space itself, or
// when OCIO is unavailable / the transform can't be built. nch is the buffer's
// channel count (>=3); alpha and extra channels are left untouched.
static bool
convertSceneLinearRGB_(float* interleaved, int width, int height, int nch,
                       const std::string& target)
{
    if (!interleaved || nch < 3 || target.empty()) {
        return false;
    }
    if (target == "Raw" || target == "raw") {
        return false;
    }
    const std::string sceneLinear = getOcioSceneLinearName();
    if (target == sceneLinear) {
        return false;  // identity
    }
    static OIIO::ColorConfig config;
    if ( !OIIO::ColorConfig::supportsOpenColorIO() ) {
        return false;
    }
    const std::string from = sceneLinear.empty() ? std::string("scene_linear") : sceneLinear;
    OIIO::ColorProcessorHandle proc = config.createColorProcessor(from, target);
    if (!proc) {
        return false;
    }
    // RGB over an nch-interleaved buffer: process 3 channels with a per-pixel
    // stride of nch floats so alpha/extra channels are skipped.
    proc->apply(interleaved, width, height, 3,
                (OIIO::stride_t)sizeof(float),
                (OIIO::stride_t)(nch * sizeof(float)),
                (OIIO::stride_t)(nch * (size_t)width * sizeof(float)));
    return true;
}

bool
CyclesRenderer::saveMultiLayerEXR(const std::string& filepath,
                                    const std::map<std::string, std::vector<float>>& passBuffers,
                                    int width, int height)
{
    // Defaults match the long-standing manual button: float32 + ZIP.
    ExrOutputOptions defaults;
    return saveMultiLayerEXR(filepath, passBuffers, width, height, defaults);
}

bool
CyclesRenderer::saveMultiLayerEXR(const std::string& filepath,
                                    const std::map<std::string, std::vector<float>>& passBuffers,
                                    int width, int height,
                                    const ExrOutputOptions& opts)
{
    // Map pass names to EXR layer/channel names
    struct LayerDef { std::string passName; std::string prefix; int nCh; std::vector<std::string> chans; };
    std::vector<LayerDef> layers;

    auto add = [&](const char* pass, const char* prefix, int n, std::vector<std::string> ch) {
        if (passBuffers.count(pass)) layers.push_back({pass, prefix, n, ch});
    };

    add("Combined", "Color",           4, {"R","G","B","A"});
    add("DiffDir",  "DiffuseDirect",   3, {"R","G","B"});
    add("DiffInd",  "DiffuseIndirect", 3, {"R","G","B"});
    add("DiffCol",  "DiffuseColor",    3, {"R","G","B"});
    add("Diffuse",  "Diffuse",         3, {"R","G","B"});
    add("GlossDir", "GlossyDirect",    3, {"R","G","B"});
    add("GlossInd", "GlossyIndirect",  3, {"R","G","B"});
    add("GlossCol", "GlossyColor",     3, {"R","G","B"});
    add("Glossy",   "Glossy",          3, {"R","G","B"});
    add("Transmission", "Transmission", 3, {"R","G","B"});
    add("Emit",     "Emission",        3, {"R","G","B"});
    add("Env",      "Environment",     3, {"R","G","B"});
    add("AO",       "AO",             1, {"A"});
    add("Normal",   "Normal",          3, {"X","Y","Z"});
    add("Depth",    "depth",           1, {"Z"});
    add("UV",       "UV",              3, {"U","V","W"});
    add("ShadowCatcherMatte", "ShadowCatcherMatte", 4, {"R","G","B","A"});
    add("ReflectionMatte",    "ReflectionMatte",    4, {"R","G","B","A"});

    // Light group passes
    for (auto& entry : passBuffers) {
        if (entry.first.substr(0, 9) == "Combined_") {
            std::string grp = entry.first.substr(9);
            layers.push_back({entry.first, "LightGroup_" + grp, 3, {"R","G","B"}});
        }
    }

    if (layers.empty()) return false;

    // Build channel list
    // Combined/beauty uses unprefixed names (R,G,B,A) per EXR convention
    int totalCh = 0;
    std::vector<std::string> chanNames;
    for (auto& l : layers) {
        bool isBeauty = (l.passName == "Combined");
        for (auto& c : l.chans) {
            chanNames.push_back(isBeauty ? c : (l.prefix + "." + c));
            totalCh++;
        }
    }

    // Resolve per-pass bit-depth / compression from the options. Falls
    // back to FLOAT + ZIP when the spec strings are empty or unknown.
    OIIO::TypeDesc pixelType = OIIO::TypeDesc::FLOAT;
    std::string compression = "zip";
    resolveExrOptions_(opts, pixelType, compression);

    OIIO::ImageSpec spec(width, height, totalCh, pixelType);
    spec.channelnames = chanNames;
    spec.attribute("compression", compression);
    // Multi-layer EXR bundles colour AND data AOVs and must stay comp-ready, so
    // it is always written scene-linear and only TAGGED (never converted). For
    // EXR delivery in another space, use a Write node.
    {
        const std::string sceneLinear = getOcioSceneLinearName();
        if (!sceneLinear.empty()) {
            spec.attribute("oiio:ColorSpace", sceneLinear);
        }
    }

    auto out = OIIO::ImageOutput::create(filepath);
    if (!out) return false;
    if (!out->open(filepath, spec)) return false;

    // Interleave all channels
    std::vector<float> pixels(width * height * totalCh, 0.0f);
    int offset = 0;
    for (auto& l : layers) {
        auto it = passBuffers.find(l.passName);
        if (it == passBuffers.end()) { offset += l.nCh; continue; }
        const auto& src = it->second;

        for (int y = 0; y < height; ++y) {
            int srcY = (height - 1) - y; // flip Y (bottom-up → top-down)
            for (int x = 0; x < width; ++x) {
                int si = (srcY * width + x) * 4;
                int di = (y * width + x) * totalCh + offset;
                for (int c = 0; c < l.nCh; ++c) {
                    pixels[di + c] = src[si + c];
                }
            }
        }
        offset += l.nCh;
    }

    // Always write the source buffer as 32-bit float — OIIO converts to
    // the destination pixelType (HALF/FLOAT) per the spec we opened with.
    out->write_image(OIIO::TypeDesc::FLOAT, pixels.data());
    out->close();

    const char* typeLabel = (pixelType == OIIO::TypeDesc::HALF) ? "half"  : "float";
    printf("[CyclesRenderer] Saved EXR: %s (%dx%d, %d ch, %d layers, %s, %s)\n",
           filepath.c_str(), width, height, totalCh, (int)layers.size(),
           typeLabel, compression.c_str());
    return true;
}

// Detect lowercase extension (no leading dot). Returns empty for paths
// without a recognizable extension. Local helper — saveSingleImage uses
// this to dispatch on PNG/TIFF/JPEG output formats.
static std::string
extLower_(const std::string& path)
{
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return std::string();
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool
CyclesRenderer::saveSingleImage(const std::string& filepath,
                                  const std::vector<float>& rgbaBuffer,
                                  int width, int height,
                                  bool isCombined,
                                  const ExrOutputOptions& opts)
{
    if ((int)rgbaBuffer.size() < width * height * 4) return false;

    const std::string ext = extLower_(filepath);
    const bool isPNG  = (ext == "png");
    const bool isTIFF = (ext == "tif" || ext == "tiff");
    const bool isJPEG = (ext == "jpg" || ext == "jpeg");
    if (!isPNG && !isTIFF && !isJPEG) return false;

    // Pick destination TypeDesc per format constraints + bitDepth hint.
    // bitDepth strings already used elsewhere: "8-bit Integer", "16-bit Half",
    // "16-bit", "32-bit Full". We treat any "16" hint as uint16 for PNG/TIFF
    // (HALF only means anything for EXR) and any "32" as float32 (TIFF only).
    OIIO::TypeDesc pixelType = OIIO::TypeDesc::UINT8;
    const std::string b = toLowerCopy_(opts.bitDepth);
    if (isJPEG) {
        pixelType = OIIO::TypeDesc::UINT8;            // only legal option
    } else if (b.find("32") != std::string::npos || b.find("full") != std::string::npos) {
        pixelType = isTIFF ? OIIO::TypeDesc::FLOAT : OIIO::TypeDesc::UINT16;
    } else if (b.find("16") != std::string::npos || b.find("half") != std::string::npos) {
        pixelType = OIIO::TypeDesc::UINT16;
    } else if (b.find("8") != std::string::npos) {
        pixelType = OIIO::TypeDesc::UINT8;
    }

    // JPEG can't carry alpha; force RGB. Beauty in non-JPEG keeps alpha.
    const int nCh = (isJPEG || !isCombined) ? 3 : 4;

    OIIO::ImageSpec spec(width, height, nCh, pixelType);
    if (isCombined && nCh == 4) {
        spec.channelnames = { "R", "G", "B", "A" };
        spec.alpha_channel = 3;
    } else {
        spec.channelnames = { "R", "G", "B" };
    }
    // Colorspace tag: when opts.colorspace requests a real (non scene-linear, non
    // Raw) space we convert below and tag with it; otherwise tag the source space.
    {
        const std::string sceneLinear = getOcioSceneLinearName();
        std::string csTag;
        if (opts.colorspace.empty() || opts.colorspace == sceneLinear) {
            csTag = sceneLinear;
        } else if (opts.colorspace == "Raw" || opts.colorspace == "raw") {
            csTag = "Raw";
        } else {
            csTag = opts.colorspace;
        }
        if (!csTag.empty()) {
            spec.attribute("oiio:ColorSpace", csTag);
        }
    }

    // Compression handling per format.
    const std::string c = toLowerCopy_(opts.compression);
    if (isTIFF) {
        std::string tiffComp = "zip";                 // default
        if      (c.find("none") != std::string::npos) tiffComp = "none";
        else if (c.find("lzw")  != std::string::npos) tiffComp = "lzw";
        else if (c.find("zip")  != std::string::npos) tiffComp = "zip";
        spec.attribute("compression", tiffComp);
    } else if (isJPEG) {
        // opts.compression is a quality string ("95"), or "" → 95.
        int quality = 95;
        if (!c.empty()) {
            try { quality = std::stoi(c); } catch (...) { quality = 95; }
            if (quality < 1)   quality = 1;
            if (quality > 100) quality = 100;
        }
        spec.attribute("CompressionQuality", quality);
    }
    // PNG: deflate is implicit, no attribute needed.

    auto out = OIIO::ImageOutput::create(filepath);
    if (!out) return false;
    if (!out->open(filepath, spec)) return false;

    // Repack to the requested channel count, flipping Y on the way.
    std::vector<float> pixels((size_t)width * height * nCh, 0.0f);
    for (int y = 0; y < height; ++y) {
        const int srcY = (height - 1) - y;
        for (int x = 0; x < width; ++x) {
            const int si = (srcY * width + x) * 4;
            const int di = (y * width + x) * nCh;
            // RGB (and possibly A) — narrowing happens inside OIIO based
            // on spec.format vs. the FLOAT source TypeDesc we pass below.
            pixels[di + 0] = rgbaBuffer[si + 0];
            pixels[di + 1] = rgbaBuffer[si + 1];
            pixels[di + 2] = rgbaBuffer[si + 2];
            if (nCh == 4) pixels[di + 3] = rgbaBuffer[si + 3];
        }
    }
    // Convert scene-linear → output colorspace before quantizing to 8/16-bit, so
    // PNG/JPG/TIFF review images display correctly (no-op for Raw / scene-linear /
    // data passes — see convertSceneLinearRGB_).
    convertSceneLinearRGB_(pixels.data(), width, height, nCh, opts.colorspace);
    out->write_image(OIIO::TypeDesc::FLOAT, pixels.data());
    out->close();

    const char* fmtLabel = isPNG ? "PNG" : (isTIFF ? "TIFF" : "JPEG");
    const char* typeLabel =
        pixelType == OIIO::TypeDesc::UINT8  ? "u8"  :
        pixelType == OIIO::TypeDesc::UINT16 ? "u16" :
                                              "float";
    printf("[CyclesRenderer] Saved %s: %s (%dx%d, %d ch, %s)\n",
           fmtLabel, filepath.c_str(), width, height, nCh, typeLabel);
    return true;
}

bool
CyclesRenderer::smokeTest()
{
    ccl::path_init();

    ccl::SessionParams sessionParams;
    sessionParams.background = true;
    sessionParams.samples = 1;

    ccl::SceneParams sceneParams;

    auto session = ccl::make_unique<ccl::Session>(sessionParams, sceneParams);
    bool ok = (session != nullptr && session->scene.get() != nullptr);
    session.reset();

    return ok;
}

NATRON_NAMESPACE_EXIT
