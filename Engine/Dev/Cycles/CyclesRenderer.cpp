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

// Cycles headers — must be included BEFORE CCL_NAMESPACE_BEGIN block below
#include "device/device.h"
#include "scene/camera.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
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
#include "Engine/Dev/Scene3D/SceneGraph.h"
#include "Engine/Dev/Scene3D/Light3D.h"
#include "Engine/Dev/Scene3D/MaterialProvider.h"
#include "Engine/Dev/Scene3D/ReadGeo.h"
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

// Output driver that writes to a file via OIIO
class NatronOutputDriver : public OutputDriver {
public:
    NatronOutputDriver(const std::string& filepath)
        : filepath_(filepath) {}

    void write_render_tile(const Tile& tile) override
    {
        FILE* dbg = fopen("D:/cycles_output_debug.log", "w");

        if (!(tile.size == tile.full_size)) {
            if (dbg) { fprintf(dbg, "Skipping partial tile\n"); fclose(dbg); }
            return;
        }

        const int width = tile.size.x;
        const int height = tile.size.y;
        if (dbg) fprintf(dbg, "write_render_tile: %dx%d to %s\n", width, height, filepath_.c_str());

        std::vector<float> pixels(width * height * 4);
        bool gotPixels = tile.get_pass_pixels("Combined", 4, pixels.data());
        if (dbg) fprintf(dbg, "get_pass_pixels(Combined): %s\n", gotPixels ? "YES" : "NO");

        if (!gotPixels) {
            if (dbg) fclose(dbg);
            return;
        }

        // Check if pixels are all zero
        float maxVal = 0;
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (pixels[i] > maxVal) maxVal = pixels[i];
        }
        if (dbg) fprintf(dbg, "Max pixel value: %f\n", maxVal);

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
            if (dbg) { fprintf(dbg, "OIIO::ImageOutput::create FAILED\n"); fclose(dbg); }
            return;
        }
        if (!image_output->open(filepath_, spec)) {
            if (dbg) { fprintf(dbg, "image_output->open FAILED\n"); fclose(dbg); }
            return;
        }

        image_output->write_image(OIIO::TypeDesc::FLOAT, flipped.data());
        image_output->close();

        if (dbg) { fprintf(dbg, "Image written successfully!\n"); fclose(dbg); }
    }

private:
    std::string filepath_;
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
                                     double focalLength, double hAperture,
                                     double time)
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
    {
        FILE* ldbg = fopen("D:/cycles_light_debug.log", "w");

        // === First pass: find dome lights and set background ===
        bool hasDome = false;
        const std::vector<SceneNode>& lightScan = sg.nodes();
        for (size_t i = 0; i < lightScan.size(); ++i) {
            const SceneNode& sn = lightScan[i];
            if (sn.type != eSceneNodeLight || !sn.visible) continue;
            NodePtr node = sn.sourceNode.lock();
            if (!node) continue;
            Light3D* light3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
            if (!light3d || light3d->getLightType() != Light3D::eLightDome) continue;

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
                ccl::BackgroundNode* bgNode = bgGraph->create_node<ccl::BackgroundNode>();
                bgNode->set_strength(strengthScale);
                bgGraph->connect(envTex->output("Color"), bgNode->input("Color"));
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
            hasDome = true;
            if (ldbg) fprintf(ldbg, "Dome light: env='%s' color=(%.2f,%.2f,%.2f) strength=%.1f\n",
                              envMap.c_str(), lr, lg, lb, strengthScale);
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
        if (ldbg) fprintf(ldbg, "Scanning %d scene nodes for lights...\n", (int)lightScan.size());

        for (size_t i = 0; i < lightScan.size(); ++i) {
            const SceneNode& sn = lightScan[i];
            if (ldbg) fprintf(ldbg, "  node[%d] type=%d name='%s' visible=%d\n",
                              (int)i, (int)sn.type, sn.name.c_str(), sn.visible);

            if (sn.type != eSceneNodeLight || !sn.visible) continue;

            NodePtr node = sn.sourceNode.lock();
            if (!node) { if (ldbg) fprintf(ldbg, "    -> sourceNode expired\n"); continue; }

            Light3D* light3d = dynamic_cast<Light3D*>(node->getEffectInstance().get());
            if (!light3d) { if (ldbg) fprintf(ldbg, "    -> not a Light3D\n"); continue; }

            Light3D::LightType ltype = light3d->getLightType();
            // Skip dome lights — already handled as background
            if (ltype == Light3D::eLightDome) continue;

            double ltx, lty, ltz, lr, lg, lb, lint, lexp;
            light3d->getLightParams(time, ltx, lty, ltz, lr, lg, lb, lint, lexp);
            // Apply exposure: multiply intensity by 2^exposure
            lint *= pow(2.0, lexp);

            if (ldbg) fprintf(ldbg, "    -> Light3D: type=%d pos=(%.1f,%.1f,%.1f) color=(%.2f,%.2f,%.2f) intensity=%.2f\n",
                              (int)ltype, ltx, lty, ltz, lr, lg, lb, lint);

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
            ccl::Object* obj = scene->create_node<ccl::Object>();
            obj->set_geometry(light);
            obj->set_visibility(ccl::PATH_RAY_ALL_VISIBILITY & ~ccl::PATH_RAY_CAMERA);
            obj->set_tfm(buildLightTransform((float)ltx, (float)lty, (float)ltz, lrx, lry, lrz));
            obj->tag_update(scene);
        }

        if (ldbg) fclose(ldbg);
    }

    // --- Camera from Euler angles ---
    // CameraProvider gives: world position (tx,ty,tz) + euler rotation (rx,ry,rz) in degrees
    //
    // Natron camera convention: at zero rotation, camera looks down -Z (like OpenGL).
    // Cycles camera convention: camera looks down +Z in local space.
    // So we need to flip the Z axis: negate the forward column of the rotation matrix.
    {
        float crx = cosf((float)camRX * (float)M_PI / 180.0f);
        float srx = sinf((float)camRX * (float)M_PI / 180.0f);
        float cry = cosf((float)camRY * (float)M_PI / 180.0f);
        float sry = sinf((float)camRY * (float)M_PI / 180.0f);
        float crz = cosf((float)camRZ * (float)M_PI / 180.0f);
        float srz = sinf((float)camRZ * (float)M_PI / 180.0f);

        // Rotation matrix Ry * Rx * Rz (same order as SceneGraph::buildTRS)
        // This gives the camera's local axes in world space.
        float r00 = cry * crz + sry * srx * srz;     // right.x
        float r01 = crx * srz;                         // right.y
        float r02 = -sry * crz + cry * srx * srz;     // right.z
        float r10 = -cry * srz + sry * srx * crz;     // up.x
        float r11 = crx * crz;                         // up.y
        float r12 = sry * srz + cry * srx * crz;      // up.z
        float r20 = sry * crx;                         // forward.x (Natron -Z)
        float r21 = -srx;                              // forward.y
        float r22 = cry * crx;                         // forward.z

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

        // FOV from focal length and horizontal aperture
        float fovRad = 2.0f * atanf((float)hAperture / (2.0f * (float)focalLength));
        if (fovRad < 0.01f || fovRad > 3.0f) fovRad = 45.0f * (float)M_PI / 180.0f;

        // DEBUG: log camera transform and FOV
        {
            FILE* cdbg = fopen("D:/cycles_camera_matrix.log", "w");
            if (cdbg) {
                fprintf(cdbg, "Camera pos: (%.2f, %.2f, %.2f)\n", camTX, camTY, camTZ);
                fprintf(cdbg, "Camera rot: (%.2f, %.2f, %.2f)\n", camRX, camRY, camRZ);
                fprintf(cdbg, "FOV: %.2f deg (focal=%.1f, aperture=%.4f)\n",
                        fovRad * 180.0f / (float)M_PI, focalLength, hAperture);
                fprintf(cdbg, "Camera matrix:\n");
                fprintf(cdbg, "  row0: [%.4f %.4f %.4f %.4f]\n", cameraTfm.x.x, cameraTfm.x.y, cameraTfm.x.z, cameraTfm.x.w);
                fprintf(cdbg, "  row1: [%.4f %.4f %.4f %.4f]\n", cameraTfm.y.x, cameraTfm.y.y, cameraTfm.y.z, cameraTfm.y.w);
                fprintf(cdbg, "  row2: [%.4f %.4f %.4f %.4f]\n", cameraTfm.z.x, cameraTfm.z.y, cameraTfm.z.z, cameraTfm.z.w);
                fclose(cdbg);
            }
        }

        scene->camera->set_fov(fovRad);

        scene->camera->set_full_width(_impl->width);
        scene->camera->set_full_height(_impl->height);
        scene->camera->set_nearclip(0.1f);
        scene->camera->set_farclip(10000.0f);
        scene->camera->update(scene);
    }

    // --- Render passes ---
    {
        ccl::Pass* pass = scene->create_node<ccl::Pass>();
        pass->set_name(ccl::ustring("Combined"));
        pass->set_type(ccl::PASS_COMBINED);
    }

    scene->film->set_exposure(1.0f);

    // --- Integrator ---
    scene->integrator->set_max_bounce(4);
    scene->integrator->set_max_diffuse_bounce(4);
    scene->integrator->set_max_glossy_bounce(4);
    scene->integrator->tag_update(scene, ccl::Integrator::UPDATE_ALL);

    // --- Sync geometry from SceneGraph ---
    const std::vector<SceneNode>& sceneNodes = sg.nodes();
    for (size_t i = 0; i < sceneNodes.size(); ++i) {
        const SceneNode& sn = sceneNodes[i];
        if (!sn.visible) continue;

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
                // ReadGeo — load actual mesh data from Alembic
                NodePtr meshSrcNode = sn.sourceNode.lock();
                if (!meshSrcNode) continue;
                ReadGeo* readGeo = dynamic_cast<ReadGeo*>(meshSrcNode->getEffectInstance().get());
                if (!readGeo) continue;
                MeshDataPtr meshData = readGeo->getMeshData(time);
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
        obj->tag_update(scene);
    }
}

bool
CyclesRenderer::renderToBufferWithCamera(const SceneGraph& sg,
                                          double camTX, double camTY, double camTZ,
                                          double camRX, double camRY, double camRZ,
                                          double focalLength, double hAperture,
                                          std::vector<float>& outPixels,
                                          int width, int height, int samples,
                                          double time)
{
    initialize(width, height, samples);
    syncSceneWithCamera(sg, camTX, camTY, camTZ, camRX, camRY, camRZ, focalLength, hAperture, time);

    _impl->session->set_output_driver(
        ccl::make_unique<ccl::NatronBufferOutputDriver>(&outPixels, width, height));

    startRender();
    waitForRender();

    return !outPixels.empty();
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
