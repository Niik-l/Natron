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

#include <cmath>
#include <cstring>
#include <map>
#include <set>

// Cycles headers — must be included BEFORE CCL_NAMESPACE_BEGIN block below
#include "device/device.h"
#include "scene/camera.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/pointcloud.h"
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
#include "session/output_driver.h"
#include "session/session.h"

#include "util/image.h"
#include "util/path.h"
#include "util/progress.h"
#include "util/string.h"
#include "util/unique_ptr.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>

// Natron headers
#include "Engine/Dev/Scene3D/CameraMath.h"
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include "Engine/Dev/Scene3D/RotationConventions.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/Dev/Scene3D/MaterialProvider.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
#include "Engine/Dev/Scene3D/RenderPass.h"
#include "Engine/Dev/Scene3D/ReadVDB.h"
#include "Engine/Dev/Scene3D/Volume3D.h"
#include "Engine/Dev/Particles/ParticleProvider.h"
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
    bool denoise = true;
    bool initialized = false;

    // Pixel buffer for readback
    std::vector<float> pixelBuffer;  // RGBA float, width*height*4

    Impl() {}
    ~Impl()
    {
        if (session) {
            session->cancel();
            session.reset();
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

static ccl::Shader*
createMaterialShader(ccl::Scene* scene, MaterialProvider* matProvider, double time)
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

    // Base color texture
    std::string texFile = mat->getMaterialTextureFile();
    if (!texFile.empty()) {
        ccl::ImageTextureNode* imgTex = graph->create_node<ccl::ImageTextureNode>();
        imgTex->set_filename(ccl::ustring(texFile));
        std::string diffCS = mat->getMaterialDiffuseColorspace();
        if (diffCS == "Raw") diffCS = "Non-Color";
        if (diffCS == "Linear") diffCS = "__builtin_raw";
        imgTex->set_colorspace(ccl::ustring(diffCS));
        graph->connect(texCoord->output("UV"), imgTex->input("Vector"));
        graph->connect(imgTex->output("Color"), principled->input("Base Color"));
    }

    // Normal map
    std::string normalFile = mat->getMaterialNormalMapFile();
    if (!normalFile.empty()) {
        ccl::ImageTextureNode* normalTex = graph->create_node<ccl::ImageTextureNode>();
        normalTex->set_filename(ccl::ustring(normalFile));
        normalTex->set_colorspace(ccl::ustring("Non-Color"));
        ccl::NormalMapNode* normalMap = graph->create_node<ccl::NormalMapNode>();
        normalMap->set_strength((float)mat->getMaterialNormalStrength(time));
        graph->connect(texCoord->output("UV"), normalTex->input("Vector"));
        graph->connect(normalTex->output("Color"), normalMap->input("Color"));
        graph->connect(normalMap->output("Normal"), principled->input("Normal"));
    }

    // Roughness map
    std::string roughFile = mat->getMaterialRoughnessMapFile();
    if (!roughFile.empty()) {
        ccl::ImageTextureNode* roughTex = graph->create_node<ccl::ImageTextureNode>();
        roughTex->set_filename(ccl::ustring(roughFile));
        roughTex->set_colorspace(ccl::ustring("Non-Color"));
        graph->connect(texCoord->output("UV"), roughTex->input("Vector"));
        graph->connect(roughTex->output("Color"), principled->input("Roughness"));
    }

    // Metallic map
    std::string metalFile = mat->getMaterialMetallicMapFile();
    if (!metalFile.empty()) {
        ccl::ImageTextureNode* metalTex = graph->create_node<ccl::ImageTextureNode>();
        metalTex->set_filename(ccl::ustring(metalFile));
        metalTex->set_colorspace(ccl::ustring("Non-Color"));
        graph->connect(texCoord->output("UV"), metalTex->input("Vector"));
        graph->connect(metalTex->output("Color"), principled->input("Metallic"));
    }

    // Emission map
    std::string emissionFile = mat->getMaterialEmissionMapFile();
    if (!emissionFile.empty()) {
        ccl::ImageTextureNode* emissionTex = graph->create_node<ccl::ImageTextureNode>();
        emissionTex->set_filename(ccl::ustring(emissionFile));
        std::string emCS = mat->getMaterialEmissionColorspace();
        if (emCS == "Raw") emCS = "Non-Color";
        if (emCS == "Linear") emCS = "__builtin_raw";
        emissionTex->set_colorspace(ccl::ustring(emCS));
        graph->connect(texCoord->output("UV"), emissionTex->input("Vector"));
        graph->connect(emissionTex->output("Color"), principled->input("Emission Color"));
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
}

CyclesRenderer::~CyclesRenderer()
{
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
                                     const IntegratorParams* integrator)
{
    if (!_impl->initialized) return;

    // Cancel any in-progress render
    if (_impl->session) {
        _impl->session->cancel();
        _impl->session.reset();
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
            // Dome light ALWAYS contributes to lighting.
            // "Renderable" controls camera visibility only (like Arnold's skydome Camera flag).
            bool domeVisibleInCamera = light3d->isRenderable();

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

            // Read rotation knobs from the light node
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
            // Apply active lights filter if provided
            if (activeLights && !activeLights->empty()) {
                if (activeLights->find(sn.name) == activeLights->end()) {
                    // Light not in active set — skip it entirely
                    continue;
                }
            }

            ccl::Object* obj = scene->create_node<ccl::Object>();
            obj->set_geometry(light);
            obj->set_visibility(ccl::PATH_RAY_ALL_VISIBILITY & ~ccl::PATH_RAY_CAMERA);
            obj->set_tfm(buildLightTransform((float)ltx, (float)lty, (float)ltz, lrx, lry, lrz));

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

    scene->integrator->tag_update(scene, ccl::Integrator::UPDATE_ALL);

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

                // Motion blur setup — compute shutter open/close offsets
                bool mbEnabled = motionBlur && motionBlur->enabled;
                float shutterOpen = 0, shutterClose = 0;
                if (mbEnabled) {
                    float st = motionBlur->shutterTime;
                    switch (motionBlur->shutterPosition) {
                        case 0: shutterOpen = 0; shutterClose = st; break;           // Start
                        case 1: shutterOpen = -st * 0.5f; shutterClose = st * 0.5f; break; // Center
                        case 2: shutterOpen = -st; shutterClose = 0; break;          // End
                    }
                }

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

            // Create PointCloud geometry
            ccl::PointCloud* pc = scene->create_node<ccl::PointCloud>();
            pc->reserve(count);

            for (int pi = 0; pi < count; ++pi) {
                const Particle& p = particleData->particles[pi];
                pc->add_point(ccl::make_float3(p.px, p.py, p.pz),
                              p.size > 0.001f ? p.size : 0.01f,
                              0);
            }

            // Motion blur: evaluate particles at a second time step
            if (motionBlur && motionBlur->enabled) {
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

                // Get particle data at shutter open time
                ParticleDataPtr dataOpen;
                double timeOpen = time + shutterOpen;
                if (provider) dataOpen = provider->getParticleData(timeOpen);

                // Get particle data at shutter close time
                ParticleDataPtr dataClose;
                double timeClose = time + shutterClose;
                if (provider) dataClose = provider->getParticleData(timeClose);

                if (dataOpen && dataClose &&
                    dataOpen->numParticles() > 0 && dataClose->numParticles() > 0) {
                    // Use 3 motion steps: open, center (implicit), close
                    pc->set_motion_steps(3);
                    pc->set_use_motion_blur(true);

                    // Motion attribute stores positions for step 0 (open) and step 1 (close)
                    // Center (step at index motion_steps/2) is the main points array
                    ccl::Attribute* motionAttr = pc->attributes.add(
                        ccl::ATTR_STD_MOTION_VERTEX_POSITION);
                    ccl::float4* motionData = motionAttr->data_float4();

                    // Step 0 = shutter open positions
                    int openCount = std::min(count, dataOpen->numParticles());
                    for (int pi = 0; pi < count; ++pi) {
                        if (pi < openCount) {
                            const Particle& po = dataOpen->particles[pi];
                            float r = po.size > 0.001f ? po.size : 0.01f;
                            motionData[pi] = ccl::make_float4(po.px, po.py, po.pz, r);
                        } else {
                            // Particle didn't exist at shutter open — use center position
                            const Particle& p = particleData->particles[pi];
                            float r = p.size > 0.001f ? p.size : 0.01f;
                            motionData[pi] = ccl::make_float4(p.px, p.py, p.pz, r);
                        }
                    }

                    // Step 1 = shutter close positions
                    int closeCount = std::min(count, dataClose->numParticles());
                    for (int pi = 0; pi < count; ++pi) {
                        if (pi < closeCount) {
                            const Particle& pc2 = dataClose->particles[pi];
                            float r = pc2.size > 0.001f ? pc2.size : 0.01f;
                            motionData[count + pi] = ccl::make_float4(pc2.px, pc2.py, pc2.pz, r);
                        } else {
                            const Particle& p = particleData->particles[pi];
                            float r = p.size > 0.001f ? p.size : 0.01f;
                            motionData[count + pi] = ccl::make_float4(p.px, p.py, p.pz, r);
                        }
                    }
                }
            }

            // Create a simple emissive-ish shader using particle color
            // Use the first particle's color as a base (per-point color via attribute)
            ccl::Shader* pShader = scene->create_node<ccl::Shader>();
            auto pGraph = ccl::make_unique<ccl::ShaderGraph>();
            ccl::PrincipledBsdfNode* pBsdf = pGraph->create_node<ccl::PrincipledBsdfNode>();

            // Use vertex color attribute for per-particle color
            ccl::AttributeNode* colorAttr = pGraph->create_node<ccl::AttributeNode>();
            colorAttr->set_attribute(ccl::ustring("vertex_color"));
            pGraph->connect(colorAttr->output("Color"), pBsdf->input("Base Color"));
            pGraph->connect(colorAttr->output("Color"), pBsdf->input("Emission Color"));
            pBsdf->set_emission_strength(1.0f);
            pBsdf->set_roughness(0.5f);

            pGraph->connect(pBsdf->output("BSDF"), pGraph->output()->input("Surface"));
            pShader->set_graph(std::move(pGraph));
            pShader->tag_update(scene);

            ccl::array<ccl::Node*> used_shaders;
            used_shaders.push_back_slow(pShader);
            pc->set_used_shaders(used_shaders);

            // Set per-point color attribute
            ccl::Attribute* vcol = pc->attributes.add(ccl::ustring("vertex_color"),
                                                       ccl::TypeRGBA,
                                                       ccl::ATTR_ELEMENT_VERTEX);
            ccl::float4* colorData = vcol->data_float4();
            for (int pi = 0; pi < count; ++pi) {
                const Particle& p = particleData->particles[pi];
                float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
                float alpha = p.a * (1.0f - ageFrac);
                colorData[pi] = ccl::make_float4(p.r, p.g, p.b, alpha);
            }

            // Create object
            ccl::Object* obj = scene->create_node<ccl::Object>();
            obj->set_geometry(pc);
            obj->set_tfm(ccl::transform_identity());

            // Apply RenderPass visibility if provided
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

        // Per-object material shader
        ccl::Shader* objShader = nullptr;
        NodePtr srcNode = sn.sourceNode.lock();
        if (srcNode) {
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

        ccl::Object* obj = scene->create_node<ccl::Object>();
        obj->set_geometry(mesh);
        obj->set_tfm(natronMatrixToCyclesTransform(sn.worldMatrix));

        // Apply RenderPass visibility if provided
        if (visibilityMap) {
            auto it = visibilityMap->find(sn.name);
            if (it != visibilityMap->end()) {
                const ObjectVisibility& vis = it->second;
                if (vis.isExcluded) {
                    obj->set_visibility(0);
                } else {
                    obj->set_visibility(vis.rayVisibility);
                    obj->set_use_holdout(vis.isHoldout);
                    obj->set_is_shadow_catcher(vis.isShadowCatcher);
                    if (vis.isShadowCatcher) {
                        scene->film->set_use_approximate_shadow_catcher(true);
                    }
                }
            } else {
                // Object not in any category — excluded
                obj->set_visibility(0);
            }
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
                                          double time)
{
    initialize(width, height, samples);
    syncSceneWithCamera(sg, camTX, camTY, camTZ, camRX, camRY, camRZ, focalLength, hAperture, vAperture, time);

    _impl->session->set_output_driver(
        ccl::make_unique<ccl::NatronBufferOutputDriver>(&outPixels, width, height));

    startRender();
    waitForRender();

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
                                                   const IntegratorParams* integrator)
{
    initialize(width, height, samples);
    syncSceneWithCamera(sg, camTX, camTY, camTZ, camRX, camRY, camRZ,
                        focalLength, hAperture, vAperture, time, requestedPasses,
                        visibilityMap, activeLights, dof, motionBlur, integrator);

    // Build the full list of pass names for the output driver.
    // This includes the requested standard passes plus any light group Combined passes.
    std::vector<std::string> allPassNames;

    // Always include Combined
    allPassNames.push_back("Combined");
    for (const auto& pn : requestedPasses) {
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

    _impl->session->set_output_driver(
        ccl::make_unique<ccl::NatronMultiPassOutputDriver>(&outPassBuffers, allPassNames, width, height));

    startRender();
    waitForRender();

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

bool
CyclesRenderer::saveMultiLayerEXR(const std::string& filepath,
                                    const std::map<std::string, std::vector<float>>& passBuffers,
                                    int width, int height)
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
    add("GlossDir", "GlossyDirect",    3, {"R","G","B"});
    add("GlossInd", "GlossyIndirect",  3, {"R","G","B"});
    add("GlossCol", "GlossyColor",     3, {"R","G","B"});
    add("Emit",     "Emission",        3, {"R","G","B"});
    add("Env",      "Environment",     3, {"R","G","B"});
    add("AO",       "AO",             1, {"A"});
    add("Normal",   "Normal",          3, {"X","Y","Z"});
    add("Depth",    "depth",           1, {"Z"});
    add("UV",       "UV",              3, {"U","V","W"});

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

    OIIO::ImageSpec spec(width, height, totalCh, OIIO::TypeDesc::FLOAT);
    spec.channelnames = chanNames;

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

    out->write_image(OIIO::TypeDesc::FLOAT, pixels.data());
    out->close();

    printf("[CyclesRenderer] Saved EXR: %s (%dx%d, %d ch, %d layers)\n",
           filepath.c_str(), width, height, totalCh, (int)layers.size());
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
