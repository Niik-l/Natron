/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

#include "ScanlineRender.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include "RotationConventions.h"

#include "../../../Global/GLIncludes.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "CameraMath.h"
#include "CameraProvider.h"
#include "Card3D.h"
#include "../../GLShader.h"
#include "Light3D.h"
#include "../Particles/ParticleData.h"
#include "../Particles/ParticleInstance.h"
#include "../Particles/ParticleProvider.h"
#include "ReadVDB.h"
#include "ReadAlembicArchive.h"
#include "Volume3D.h"
#include "Cube3D.h"
#include "Cylinder3D.h"
#include "Scene3D.h"
#include "../../GPUContextPool.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../OSGLContext.h"
#include "ReadGeo.h"
#include "SceneGraph.h"
#include "Sphere3D.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct ScanlineRenderPrivate
{
    // Output
    KnobIntWPtr outputWidth, outputHeight;

    // Particle rendering
    KnobChoiceWPtr particleMode;   // Point, Disc, Sphere, Sprite
    KnobChoiceWPtr particleBlend;  // Additive, Over
    KnobDoubleWPtr particleScale;  // global size multiplier
    KnobDoubleWPtr particleMotionBlur; // velocity stretch amount (legacy cheat mode)
    KnobIntWPtr motionSamples;      // number of sub-frame samples (1 = off)
    KnobDoubleWPtr motionShutter;   // shutter open fraction (0-1, default 0.5)
};


ScanlineRender::ScanlineRender(NodePtr node)
    : EffectInstance(node)
    , _imp(new ScanlineRenderPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ScanlineRender::~ScanlineRender()
{
}

std::string
ScanlineRender::getPluginDescription() const
{
    return tr("Render a 3D scene through a camera to a 2D image.\n\n"
              "Input 0 (bg): Optional background image (composited behind)\n"
              "Input 1 (obj/scn): 3D geometry (Sphere3D, Card3D, ReadGeo)\n"
              "Input 2 (cam): Camera (Camera3D or ReadAlembicCamera)\n\n"
              "The geometry's img input provides the texture.\n"
              "The camera defines the viewpoint.\n\n"
              "Equivalent to Nuke's ScanlineRender node.").toStdString();
}

std::string
ScanlineRender::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "bg";
        case 1: return "obj/scn";
        case 2: return "cam";
        default: return "";
    }
}

bool
ScanlineRender::isInputOptional(int inputNb) const
{
    // bg and cam are optional; obj/scn is required
    return (inputNb == 0 || inputNb == 2);
}

void
ScanlineRender::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ScanlineRender::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ScanlineRender::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ScanlineRender::initializeKnobs()
{
    KnobPagePtr outPage = AppManager::createKnob<KnobPage>(this, tr("Output"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Width"));
        k->setName("outputWidth"); k->setDefaultValue(1920);
        k->setMinimum(1); k->setDisplayMinimum(320); k->setDisplayMaximum(4096);
        outPage->addKnob(k); _imp->outputWidth = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Height"));
        k->setName("outputHeight"); k->setDefaultValue(1080);
        k->setMinimum(1); k->setDisplayMinimum(240); k->setDisplayMaximum(4096);
        outPage->addKnob(k); _imp->outputHeight = k;
    }

    // Particle rendering knobs
    KnobPagePtr partPage = AppManager::createKnob<KnobPage>(this, tr("Particles"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Particle Mode"));
        k->setName("particleMode"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Point", "", "Single pixel per particle (fast preview)"));
        entries.push_back(ChoiceOption("Disc", "", "Camera-facing filled circle with soft edge"));
        entries.push_back(ChoiceOption("Sphere", "", "Lit sphere with simple N dot L shading"));
        entries.push_back(ChoiceOption("Sprite", "", "Camera-facing quad (current behavior)"));
        k->populateChoices(entries);
        k->setDefaultValue(3); // Sprite default (matches current behavior)
        partPage->addKnob(k); _imp->particleMode = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Blend Mode"));
        k->setName("particleBlend"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Additive", "", "Bright, glowy — fire, sparks, energy"));
        entries.push_back(ChoiceOption("Over", "", "Alpha composite — solid particles, smoke, debris"));
        k->populateChoices(entries);
        k->setDefaultValue(0); // Additive default
        partPage->addKnob(k); _imp->particleBlend = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Particle Scale"));
        k->setName("particleScale"); k->setDefaultValue(1.0);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Global multiplier on particle size."));
        partPage->addKnob(k); _imp->particleScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Motion Blur (stretch)"));
        k->setName("particleMotionBlur"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(5.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Fast velocity-stretch motion blur (cheat). Stretches particles/instances along velocity. Use Motion Samples for physically-accurate blur."));
        partPage->addKnob(k); _imp->particleMotionBlur = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Motion Samples"));
        k->setName("motionSamples"); k->setDefaultValue(1);
        k->setMinimum(1); k->setMaximum(32);
        k->setDisplayMinimum(1); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Physically-accurate motion blur via multi-sample accumulation. 1 = off. 4-8 = typical quality. 16 = film quality. Render cost scales linearly."));
        partPage->addKnob(k); _imp->motionSamples = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shutter"));
        k->setName("motionShutter"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Shutter open duration as fraction of frame time. 0.5 = 180 degree shutter (film standard). Only used when Motion Samples > 1."));
        partPage->addKnob(k); _imp->motionShutter = k;
    }
}

StatusEnum
ScanlineRender::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                      ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = _imp->outputWidth.lock()->getValue();
    rod->y2 = _imp->outputHeight.lock()->getValue();
    return eStatusOK;
}

// ==================== Helpers ====================

static void
buildViewMatrix(double tx, double ty, double tz,
                double rx, double ry, double rz,
                float out[16])
{
    // View matrix = inverse of camera-to-world transform.
    // Camera-to-world uses Natron's standard extrinsic XYZ convention
    // (M = Rz*Ry*Rx column-vector, Maya/Blender/Houdini default, same as
    // SceneGraph::buildTRS and ImGuizmo). The inverse is M^T.
    double mInv[3][3];
    RotationConventions::composeInverse(rx, ry, rz, mInv);

    const float ntx = -(float)tx, nty = -(float)ty, ntz = -(float)tz;

    // Pack into column-major float[16]: out[col*4 + row] = mInv[row][col].
    out[0]  = (float)mInv[0][0]; out[1]  = (float)mInv[1][0]; out[2]  = (float)mInv[2][0]; out[3]  = 0.f;
    out[4]  = (float)mInv[0][1]; out[5]  = (float)mInv[1][1]; out[6]  = (float)mInv[2][1]; out[7]  = 0.f;
    out[8]  = (float)mInv[0][2]; out[9]  = (float)mInv[1][2]; out[10] = (float)mInv[2][2]; out[11] = 0.f;
    out[12] = (float)(mInv[0][0]*ntx + mInv[0][1]*nty + mInv[0][2]*ntz);
    out[13] = (float)(mInv[1][0]*ntx + mInv[1][1]*nty + mInv[1][2]*ntz);
    out[14] = (float)(mInv[2][0]*ntx + mInv[2][1]*nty + mInv[2][2]*ntz);
    out[15] = 1.f;
}

// Projection matrix is now built via CameraMath::composeProjectionMatrix
// (independent fov_h / fov_v from both apertures). Image aspect is no longer
// used to derive the Y FOV — that was a long-standing bug producing CG drift
// proportional to camera motion when sensor aspect != image aspect.

// ==================== Volume ray marching shaders ====================

static const char* volumeVertexShader =
    "varying vec3 v_WorldPos;\n"
    "void main() {\n"
    "    v_WorldPos = vec3(gl_ModelViewMatrix * gl_Vertex);\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "}\n";

static const char* volumeFragmentShader =
    "varying vec3 v_WorldPos;\n"
    "uniform sampler3D u_VolumeData;\n"
    "uniform vec3 u_VolumeMin;\n"
    "uniform vec3 u_VolumeMax;\n"
    "uniform vec3 u_CameraPos;\n"
    "uniform float u_Density;\n"
    "uniform vec3 u_VolumeColor;\n"
    "uniform float u_StepSize;\n"
    "uniform vec3 u_LightPos;\n"
    "uniform vec3 u_LightColor;\n"
    "uniform float u_LightIntensity;\n"
    "uniform float u_ShadowDensity;\n"
    "uniform int u_ShadowSteps;\n"
    "uniform int u_LightEnabled;\n"
    "\n"
    "vec2 intersectBox(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax) {\n"
    "    vec3 invR = vec3(1.0) / rd;\n"
    "    vec3 t0 = (bmin - ro) * invR;\n"
    "    vec3 t1 = (bmax - ro) * invR;\n"
    "    vec3 tmin = min(t0, t1);\n"
    "    vec3 tmax = max(t0, t1);\n"
    "    float tNear = max(max(tmin.x, tmin.y), tmin.z);\n"
    "    float tFar = min(min(tmax.x, tmax.y), tmax.z);\n"
    "    return vec2(tNear, tFar);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec3 rayDir = normalize(v_WorldPos - u_CameraPos);\n"
    "    vec2 tHit = intersectBox(u_CameraPos, rayDir, u_VolumeMin, u_VolumeMax);\n"
    "    float tNear = max(tHit.x, 0.0);\n"
    "    float tFar = tHit.y;\n"
    "    if (tNear >= tFar) discard;\n"
    "\n"
    "    vec4 accum = vec4(0.0);\n"
    "    vec3 boxSize = u_VolumeMax - u_VolumeMin;\n"
    "\n"
    "    for (float t = tNear; t < tFar; t += u_StepSize) {\n"
    "        vec3 pos = u_CameraPos + rayDir * t;\n"
    "        vec3 texCoord = (pos - u_VolumeMin) / boxSize;\n"
    "        float samp = texture3D(u_VolumeData, texCoord).r;\n"
    "        if (samp < 0.001) continue;\n"
    "\n"
    "        float d = samp * u_Density * u_StepSize;\n"
    "        vec3 col = u_VolumeColor;\n"
    "\n"
    "        // Lighting with shadow ray\n"
    "        if (u_LightEnabled > 0) {\n"
    "            vec3 lightDir = normalize(u_LightPos - pos);\n"
    "            float lightDist = length(u_LightPos - pos);\n"
    "            float shadowStep = lightDist / float(u_ShadowSteps);\n"
    "            float shadowAccum = 0.0;\n"
    "            for (int s = 1; s <= u_ShadowSteps; s++) {\n"
    "                vec3 shadowPos = pos + lightDir * shadowStep * float(s);\n"
    "                vec3 shadowTC = (shadowPos - u_VolumeMin) / boxSize;\n"
    "                if (shadowTC.x >= 0.0 && shadowTC.x <= 1.0 &&\n"
    "                    shadowTC.y >= 0.0 && shadowTC.y <= 1.0 &&\n"
    "                    shadowTC.z >= 0.0 && shadowTC.z <= 1.0) {\n"
    "                    shadowAccum += texture3D(u_VolumeData, shadowTC).r * shadowStep;\n"
    "                }\n"
    "            }\n"
    "            float lightAmount = exp(-shadowAccum * u_ShadowDensity);\n"
    "            col = u_VolumeColor * u_LightColor * lightAmount * u_LightIntensity;\n"
    "        }\n"
    "\n"
    "        vec4 sampleColor = vec4(col * d, d);\n"
    "        accum.rgb += (1.0 - accum.a) * sampleColor.rgb;\n"
    "        accum.a += (1.0 - accum.a) * sampleColor.a;\n"
    "        if (accum.a > 0.98) break;\n"
    "    }\n"
    "    gl_FragColor = accum;\n"
    "}\n";

// ==================== Geometry extraction helper ====================

struct GeoData {
    std::vector<float> verts;    // x,y,z interleaved
    std::vector<float> uvs;      // u,v interleaved
    std::vector<int> triIndices;
    float localMatrix[16];
    ImagePtr texImg;
};

static bool
extractGeometry(EffectInstancePtr effect, double time, ViewIdx view, GeoData& out)
{
    out.verts.clear();
    out.uvs.clear();
    out.triIndices.clear();
    SceneGraph::buildTRS(0,0,0, 0,0,0, 1,1,1, out.localMatrix);
    out.texImg.reset();

    if (!effect) return false;

    Sphere3D* sphere = dynamic_cast<Sphere3D*>(effect.get());
    if (sphere) {
        std::vector<Sphere3D::SphereVertex> sv;
        sphere->generateSphereMesh(time, sv, out.triIndices);
        out.verts.resize(sv.size() * 3);
        out.uvs.resize(sv.size() * 2);
        for (size_t i = 0; i < sv.size(); ++i) {
            out.verts[i*3+0] = sv[i].x; out.verts[i*3+1] = sv[i].y; out.verts[i*3+2] = sv[i].z;
            out.uvs[i*2+0] = sv[i].u; out.uvs[i*2+1] = sv[i].v;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy,sz;
        sphere->getSphereTransform(time, tx,ty,tz, rx,ry,rz, sx,sy,sz);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,(float)sz, out.localMatrix);
        if (sphere->getInput(0)) {
            RectI roi;
            out.texImg = sphere->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    Card3D* card = dynamic_cast<Card3D*>(effect.get());
    if (card) {
        std::vector<Card3D::CardVertex> cv;
        card->generateCardMesh(time, cv, out.triIndices);
        out.verts.resize(cv.size() * 3);
        out.uvs.resize(cv.size() * 2);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy;
        card->getCardTransform(time, tx,ty,tz, rx,ry,rz, sx,sy);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,1.0f, out.localMatrix);
        if (card->getInput(0)) {
            RectI roi;
            out.texImg = card->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    Cube3D* cube = dynamic_cast<Cube3D*>(effect.get());
    if (cube) {
        std::vector<Cube3D::CubeVertex> cv;
        cube->generateCubeMesh(time, cv, out.triIndices);
        out.verts.resize(cv.size() * 3);
        out.uvs.resize(cv.size() * 2);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy,sz;
        cube->getCubeTransform(time, tx,ty,tz, rx,ry,rz, sx,sy,sz);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,(float)sz, out.localMatrix);
        if (cube->getInput(0)) {
            RectI roi;
            out.texImg = cube->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    Cylinder3D* cyl = dynamic_cast<Cylinder3D*>(effect.get());
    if (cyl) {
        std::vector<Cylinder3D::CylinderVertex> cv;
        cyl->generateCylinderMesh(time, cv, out.triIndices);
        out.verts.resize(cv.size() * 3);
        out.uvs.resize(cv.size() * 2);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy,sz;
        cyl->getCylinderTransform(time, tx,ty,tz, rx,ry,rz, sx,sy,sz);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,(float)sz, out.localMatrix);
        if (cyl->getInput(0)) {
            RectI roi;
            out.texImg = cyl->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    ReadGeo* readGeo = dynamic_cast<ReadGeo*>(effect.get());
    if (readGeo) {
        MeshDataPtr mesh = readGeo->getMeshData(time);
        if (!mesh || mesh->numVertices == 0) return false;
        out.verts = mesh->vertices;
        out.triIndices = mesh->faceIndices;
        const int nv = (int)(out.verts.size() / 3);

        // Per-vertex UVs from the per-face-vertex array. First occurrence of each
        // vertex wins (lossy for UV seams; correct for typical clean DMP meshes).
        out.uvs.assign(nv * 2, 0.5f);
        if (mesh->hasUVs && mesh->uvs.size() == mesh->faceIndices.size() * 2) {
            std::vector<char> set((size_t)nv, 0);
            for (size_t i = 0; i < mesh->faceIndices.size(); ++i) {
                const int v = mesh->faceIndices[i];
                if (v >= 0 && v < nv && !set[v]) {
                    out.uvs[v * 2 + 0] = mesh->uvs[i * 2 + 0];
                    out.uvs[v * 2 + 1] = mesh->uvs[i * 2 + 1];
                    set[v] = 1;
                }
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out.localMatrix[c * 4 + r] = mesh->transform[r * 4 + c];

        // Optional Image input (input 1) — per-mesh texture for the scanline.
        if (readGeo->getInput(1)) {
            RectI roi;
            out.texImg = readGeo->getImage(1, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    return false;
}

// Wrapper that handles both single-emit nodes (one GeoData via the existing
// extractGeometry) and multi-emit nodes (ReadAlembicArchive — one GeoData per
// visible mesh entry, world transform composed from the archive's parent chain).
// Appends 0..N entries to `out`.
static void
extractGeometries(EffectInstancePtr effect, double time, ViewIdx view, std::vector<GeoData>& out)
{
    if (!effect) return;

    ReadAlembicArchive* abcArchive = dynamic_cast<ReadAlembicArchive*>(effect.get());
    if (abcArchive) {
        ImagePtr sharedTex;
        if (abcArchive->getInput(1)) {
            RectI roi;
            sharedTex = abcArchive->getImage(1, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }

        const int count = abcArchive->getSceneNodeCount();
        for (int i = 0; i < count; ++i) {
            MeshDataPtr mesh = abcArchive->getMeshDataAt(i);
            if (!mesh || mesh->numVertices == 0) continue;

            GeoData g;
            SceneGraph::buildTRS(0,0,0, 0,0,0, 1,1,1, g.localMatrix);
            if (!abcArchive->getEntryWorldMatrix(i, time, g.localMatrix)) continue;

            g.verts = mesh->vertices;
            g.triIndices = mesh->faceIndices;
            const int nv = (int)(g.verts.size() / 3);
            g.uvs.assign(nv * 2, 0.5f);
            if (mesh->hasUVs && mesh->uvs.size() == mesh->faceIndices.size() * 2) {
                std::vector<char> set((size_t)nv, 0);
                for (size_t k = 0; k < mesh->faceIndices.size(); ++k) {
                    const int v = mesh->faceIndices[k];
                    if (v >= 0 && v < nv && !set[v]) {
                        g.uvs[v * 2 + 0] = mesh->uvs[k * 2 + 0];
                        g.uvs[v * 2 + 1] = mesh->uvs[k * 2 + 1];
                        set[v] = 1;
                    }
                }
            }
            g.texImg = sharedTex;
            out.push_back(g);
        }
        return;
    }

    // Single-result path: delegate to the existing extractGeometry helper.
    GeoData g;
    if (extractGeometry(effect, time, view, g)) {
        out.push_back(g);
    }
}

// Helper: render one GeoData object (must be called within active GL context with camera set up)
static void
renderGeoObject(const GeoData& geo)
{
    int numVerts = (int)(geo.verts.size() / 3);
    int numTris = (int)(geo.triIndices.size() / 3);
    if (numVerts == 0 || numTris == 0) return;

    // Upload texture if available
    GLuint srcTex = 0;
    bool hasTexture = false;

    if (geo.texImg) {
        RectI texBounds = geo.texImg->getBounds();
        int texW = texBounds.width();
        int texH = texBounds.height();
        if (texW > 0 && texH > 0) {
            glGenTextures(1, &srcTex);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            std::vector<float> texData(texW * texH * 4, 0.0f);
            {
                Image::ReadAccess ra(geo.texImg.get());
                for (int y = texBounds.y1; y < texBounds.y2; ++y) {
                    for (int x = texBounds.x1; x < texBounds.x2; ++x) {
                        const float* pix = (const float*)ra.pixelAt(x, y);
                        if (pix) {
                            int idx = ((y - texBounds.y1) * texW + (x - texBounds.x1)) * 4;
                            texData[idx + 0] = pix[0];
                            texData[idx + 1] = pix[1];
                            texData[idx + 2] = pix[2];
                            texData[idx + 3] = (geo.texImg->getComponents().getNumComponents() >= 4) ? pix[3] : 1.0f;
                        }
                    }
                }
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, texW, texH, 0, GL_RGBA, GL_FLOAT, texData.data());
            hasTexture = true;
        }
    }

    if (hasTexture) glEnable(GL_TEXTURE_2D);

    glPushMatrix();
    glMultMatrixf(geo.localMatrix);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_TRIANGLES);
    for (int t = 0; t < numTris; ++t) {
        for (int vi = 0; vi < 3; ++vi) {
            int idx = geo.triIndices[t * 3 + vi];
            if (idx < 0 || idx >= numVerts) continue;
            if (hasTexture && (int)geo.uvs.size() > idx * 2 + 1) {
                glTexCoord2f(geo.uvs[idx * 2 + 0], geo.uvs[idx * 2 + 1]);
            }
            glVertex3f(geo.verts[idx * 3 + 0], geo.verts[idx * 3 + 1], geo.verts[idx * 3 + 2]);
        }
    }
    glEnd();

    glPopMatrix();
    if (hasTexture) {
        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &srcTex);
    }
}

// ==================== Render ====================

StatusEnum
ScanlineRender::render(const RenderActionArgs& args)
{
    assert(!args.outputPlanes.empty());
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    int outW = _imp->outputWidth.lock()->getValue();
    int outH = _imp->outputHeight.lock()->getValue();

    // --- Get camera from input 2 ---
    EffectInstancePtr camEffect = getInput(2);
    CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;

    double camTX = 0, camTY = 0, camTZ = 5, camRX = 0, camRY = 0, camRZ = 0;
    double camFL = 50.0, camHA = 24.576, camVA = 18.672;
    float camNear = 0.1f, camFar = 10000.0f;

    if (cam) {
        cam->getCameraPosition(args.time, camTX, camTY, camTZ, camRX, camRY, camRZ);
        camFL = cam->getCameraFocalLength(args.time);
        camHA = cam->getCameraHAperture(args.time);
        camVA = cam->getCameraVAperture(args.time);
        camNear = (float)cam->getCameraNear(args.time);
        camFar = (float)cam->getCameraFar(args.time);
    }

    // --- Collect geometry objects and/or particles ---
    std::vector<GeoData> geoObjects;
    ParticleDataPtr particleData;

    EffectInstancePtr geoEffect = getInput(1);
    if (!geoEffect) return eStatusFailed;

    // Check for volume nodes
    Volume3D* volume3d = dynamic_cast<Volume3D*>(geoEffect.get());
    ReadVDB* readVdb = dynamic_cast<ReadVDB*>(geoEffect.get());

    // Check for particle instance node (must check BEFORE ParticleProvider
    // since ParticleInstance inherits from it)
    ParticleInstance* particleInstancer = dynamic_cast<ParticleInstance*>(geoEffect.get());

    // Underlying particle data for motion blur offsetting (works for both sprites and instances)
    ParticleDataPtr motionBlurPData;

    // Check for particle nodes (only if NOT an instancer — instancer renders geo, not sprites)
    if (!particleInstancer) {
        ParticleProvider* pProvider = dynamic_cast<ParticleProvider*>(geoEffect.get());
        if (pProvider) {
            particleData = pProvider->getParticleData(args.time);
            motionBlurPData = particleData;
        }
    } else {
        // Instancer: get the upstream particle data for motion blur offsetting
        motionBlurPData = particleInstancer->getParticleData(args.time);
    }

    // Light detection (declared early so Scene iteration can find lights)
    Light3D* light3d = dynamic_cast<Light3D*>(geoEffect.get());

    // Check if input is a Scene3D (multi-object aggregator)
    if (!particleData) {
        Scene3D* scene = dynamic_cast<Scene3D*>(geoEffect.get());
        if (scene) {
            for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
                EffectInstancePtr sceneInput = scene->getInput(i);
                if (!sceneInput) continue;

                // Check for volumes in scene
                Volume3D* sVol = dynamic_cast<Volume3D*>(sceneInput.get());
                if (sVol && !volume3d) { volume3d = sVol; continue; }

                ReadVDB* sVdb = dynamic_cast<ReadVDB*>(sceneInput.get());
                if (sVdb && !readVdb) { readVdb = sVdb; continue; }

                // Check for lights in scene (already handled above, but also here)
                Light3D* sLight = dynamic_cast<Light3D*>(sceneInput.get());
                if (sLight) { if (!light3d) light3d = sLight; continue; }

                // Check for particle instancer in scene
                ParticleInstance* sInstancer = dynamic_cast<ParticleInstance*>(sceneInput.get());
                if (sInstancer) {
                    if (!particleInstancer) particleInstancer = sInstancer;
                    continue;
                }

                // Check for particles in scene
                ParticleProvider* sProvider = dynamic_cast<ParticleProvider*>(sceneInput.get());
                if (sProvider) {
                    particleData = sProvider->getParticleData(args.time);
                    motionBlurPData = particleData;
                } else {
                    extractGeometries(sceneInput, args.time, args.view, geoObjects);
                }
            }
        } else {
            extractGeometries(geoEffect, args.time, args.view, geoObjects);
        }
    }

    if (geoObjects.empty() && !particleData && !particleInstancer && !volume3d && !readVdb) return eStatusFailed;

    // --- Acquire GL context ---
    GPUContextPool* pool = appPTR->getGPUContextPool();
    if (!pool) return eStatusFailed;

    OSGLContextPtr glContext;
    try {
        glContext = pool->attachGLContextToRender(true);
    } catch (...) {
        return eStatusFailed;
    }
    if (!glContext) return eStatusFailed;

    glContext->setContextCurrentNoRender();

    // --- Determine MSAA sample count (clamp to driver max) ---
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    int samples = std::min(4, (int)maxSamples); // 4x MSAA default
    if (samples < 1) samples = 1;

    // --- Create MSAA FBO (multisampled color + depth renderbuffers) ---
    GLuint msFBO = 0, msColorRB = 0, msDepthRB = 0;
    glGenFramebuffers(1, &msFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, msFBO);

    glGenRenderbuffers(1, &msColorRB);
    glBindRenderbuffer(GL_RENDERBUFFER, msColorRB);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA32F, outW, outH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColorRB);

    glGenRenderbuffers(1, &msDepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepthRB);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, outW, outH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msDepthRB);

    GLenum msStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (msStatus != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &msFBO);
        glDeleteRenderbuffers(1, &msColorRB);
        glDeleteRenderbuffers(1, &msDepthRB);
        OSGLContext::unsetCurrentContextNoRender();
        pool->releaseGLContextFromRender(glContext);
        return eStatusFailed;
    }

    // --- Create resolve FBO (single-sample, used for glReadPixels) ---
    GLuint fbo = 0, colorTex = 0, depthRB = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, outW, outH, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glGenRenderbuffers(1, &depthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, outW, outH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &msFBO);
        glDeleteRenderbuffers(1, &msColorRB);
        glDeleteRenderbuffers(1, &msDepthRB);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &colorTex);
        glDeleteRenderbuffers(1, &depthRB);
        OSGLContext::unsetCurrentContextNoRender();
        pool->releaseGLContextFromRender(glContext);
        return eStatusFailed;
    }

    // Render into the multisampled FBO
    glBindFramebuffer(GL_FRAMEBUFFER, msFBO);

    glViewport(0, 0, outW, outH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE); // enable MSAA for multisampled FBO

    // --- Set up camera matrices ---
    // Projection comes from both apertures via CameraMath. Image aspect
    // (outW/outH) intentionally not used here — fixes CG drift under camera
    // motion when sensor aspect != image aspect.
    float viewMatrix[16], projMatrix[16];
    buildViewMatrix(camTX, camTY, camTZ, camRX, camRY, camRZ, viewMatrix);
    CameraMath::composeProjectionMatrix(camFL, camHA, camVA, camNear, camFar, projMatrix);

    // --- Motion blur setup ---
    int motionSamples = _imp->motionSamples.lock() ? _imp->motionSamples.lock()->getValue() : 1;
    float motionShutter = _imp->motionShutter.lock() ? (float)_imp->motionShutter.lock()->getValueAtTime(args.time) : 0.5f;
    if (motionSamples < 1) motionSamples = 1;

    // Stretch cheat: only active when multi-sample is off. Shutter multiplies the stretch.
    float motionBlurKnob = _imp->particleMotionBlur.lock() ? (float)_imp->particleMotionBlur.lock()->getValueAtTime(args.time) : 0.0f;
    float motionBlur = (motionSamples == 1) ? (motionBlurKnob * motionShutter) : 0.0f;

    // Save original particle positions for per-sample offsetting
    // (works for both sprite rendering and instance rendering)
    std::vector<std::array<float, 3>> origParticlePos;
    if (motionSamples > 1 && motionBlurPData) {
        origParticlePos.reserve(motionBlurPData->particles.size());
        for (const Particle& p : motionBlurPData->particles) {
            origParticlePos.push_back({p.px, p.py, p.pz});
        }
    }

    // Accumulator for multi-sample blur
    std::vector<float> accumPixels;

    // === MULTI-SAMPLE RENDER LOOP ===
    for (int sample = 0; sample < motionSamples; ++sample) {
        // Compute sub-frame time offset, centered around 0
        float sampleDt = (motionSamples > 1)
            ? (((float)sample / (motionSamples - 1)) - 0.5f) * motionShutter
            : 0.0f;

        // Apply offset to particle positions (affects both sprites and instances)
        if (motionSamples > 1 && motionBlurPData) {
            for (size_t i = 0; i < motionBlurPData->particles.size(); ++i) {
                Particle& p = motionBlurPData->particles[i];
                p.px = origParticlePos[i][0] + p.vx * sampleDt;
                p.py = origParticlePos[i][1] + p.vy * sampleDt;
                p.pz = origParticlePos[i][2] + p.vz * sampleDt;
            }
        }

        // Clear the MSAA FBO for this sample
        glBindFramebuffer(GL_FRAMEBUFFER, msFBO);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(projMatrix);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(viewMatrix);

        // --- Render all geometry objects ---
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (size_t gi = 0; gi < geoObjects.size(); ++gi) {
        renderGeoObject(geoObjects[gi]);
    }

    // --- Render particles ---
    if (particleData && particleData->numParticles() > 0) {
        int partMode = _imp->particleMode.lock() ? _imp->particleMode.lock()->getValue() : 3;
        int blendMode = _imp->particleBlend.lock() ? _imp->particleBlend.lock()->getValue() : 0;
        float globalScale = _imp->particleScale.lock() ? (float)_imp->particleScale.lock()->getValueAtTime(args.time) : 1.0f;

        // Get camera right/up/forward vectors from modelview matrix for billboarding
        float mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        float rightX = mv[0], rightY = mv[4], rightZ = mv[8];
        float upX    = mv[1], upY    = mv[5], upZ    = mv[9];
        float fwdX   = mv[2], fwdY   = mv[6], fwdZ   = mv[10];

        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        if (blendMode == 0)
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);         // Additive
        else
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Over

        // Disable depth writes so particles blend instead of occluding each other,
        // but keep depth test so they still respect scene geometry
        glDepthMask(GL_FALSE);

        // Note: GL_LINE_SMOOTH/POLYGON_SMOOTH removed — MSAA handles AA properly.
        // GL_POINT_SMOOTH still useful for round point sprites in Point mode.
        glEnable(GL_POINT_SMOOTH);
        glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

        if (partMode == 0) {
            // --- Point mode ---
            // Set point size based on global scale (default ~3 pixels)
            glPointSize(3.0f * globalScale);
            glLineWidth(2.0f * globalScale);
            if (motionBlur > 0.001f) {
                // Motion blur: draw lines from tail to head with gradient alpha
                glBegin(GL_LINES);
                for (int i = 0; i < particleData->numParticles(); ++i) {
                    const Particle& p = particleData->particles[i];
                    float alpha = p.a;
                    if (alpha < 0.001f) continue;
                    // Project velocity to camera plane
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);
                    if (vLen < 0.0001f) {
                        glColor4f(p.r, p.g, p.b, alpha);
                        glVertex3f(p.px, p.py, p.pz);
                        glVertex3f(p.px, p.py, p.pz);
                        continue;
                    }
                    float stretch = vLen * motionBlur;
                    // Tail (alpha 0) → Head (full alpha)
                    glColor4f(p.r, p.g, p.b, 0.0f);
                    glVertex3f(p.px - vPX/vLen * stretch, p.py - vPY/vLen * stretch, p.pz - vPZ/vLen * stretch);
                    glColor4f(p.r, p.g, p.b, alpha);
                    glVertex3f(p.px, p.py, p.pz);
                }
                glEnd();
            } else {
                glBegin(GL_POINTS);
                for (int i = 0; i < particleData->numParticles(); ++i) {
                    const Particle& p = particleData->particles[i];
                    float alpha = p.a;
                    if (alpha < 0.001f) continue;
                    glColor4f(p.r, p.g, p.b, alpha);
                    glVertex3f(p.px, p.py, p.pz);
                }
                glEnd();
            }

        } else if (partMode == 1) {
            // --- Disc mode (camera-facing circle with soft edge) ---
            // Approximate a circle with a triangle fan per particle
            const int segments = 32;
            float cosTable[33], sinTable[33];
            for (int s = 0; s <= segments; ++s) {
                float a = 2.0f * (float)M_PI * s / segments;
                cosTable[s] = std::cos(a);
                sinTable[s] = std::sin(a);
            }

            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                float alpha = p.a;
                if (alpha < 0.001f) continue;
                float hs = p.size * 0.5f * globalScale;

                // Motion blur — elliptical fan, soft in all directions
                if (motionBlur > 0.001f) {
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);

                    if (vLen > 0.0001f) {
                        float dx = vPX / vLen, dy = vPY / vLen, dz = vPZ / vLen;
                        float px = fwdY * dz - fwdZ * dy;
                        float py = fwdZ * dx - fwdX * dz;
                        float pz = fwdX * dy - fwdY * dx;
                        float pLen = std::sqrt(px*px + py*py + pz*pz);
                        if (pLen > 0.0001f) { px /= pLen; py /= pLen; pz /= pLen; }

                        float halfLen = vLen * motionBlur * 0.5f + hs;
                        float halfWid = hs;
                        // Center the ellipse so the bright spot is at the particle position (head)
                        float cx = p.px - dx * halfLen * 0.5f;
                        float cy = p.py - dy * halfLen * 0.5f;
                        float cz = p.pz - dz * halfLen * 0.5f;

                        const int segs = 16;
                        glBegin(GL_TRIANGLE_FAN);
                        glColor4f(p.r, p.g, p.b, alpha);
                        glVertex3f(p.px, p.py, p.pz);
                        glColor4f(p.r, p.g, p.b, 0.0f);
                        for (int s = 0; s <= segs; ++s) {
                            float a = 2.0f * (float)M_PI * s / segs;
                            float ca = std::cos(a), sa = std::sin(a);
                            float ex = dx * ca * halfLen + px * sa * halfWid;
                            float ey = dy * ca * halfLen + py * sa * halfWid;
                            float ez = dz * ca * halfLen + pz * sa * halfWid;
                            glVertex3f(cx + ex, cy + ey, cz + ez);
                        }
                        glEnd();
                        continue;
                    }
                }

                // No motion blur — standard camera-facing disc
                glBegin(GL_TRIANGLE_FAN);
                glColor4f(p.r, p.g, p.b, alpha);
                glVertex3f(p.px, p.py, p.pz);
                glColor4f(p.r, p.g, p.b, alpha * 0.0f);
                for (int s = 0; s <= segments; ++s) {
                    float ex = rightX * cosTable[s] + upX * sinTable[s];
                    float ey = rightY * cosTable[s] + upY * sinTable[s];
                    float ez = rightZ * cosTable[s] + upZ * sinTable[s];
                    glVertex3f(p.px + ex * hs, p.py + ey * hs, p.pz + ez * hs);
                }
                glEnd();
            }

        } else if (partMode == 2) {
            // --- Sphere mode (lit sphere with N.L shading, or stretched fan for motion blur) ---
            // When motion blur is on, use elliptical fan with soft alpha (like Disc mode).
            // When off, use full 3D sphere mesh with N.L shading.
            if (motionBlur < 0.001f) {
                // Static: enable depth writes + back-face culling for 3D mesh
                glDepthMask(GL_TRUE);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            }
            // Light direction in view space (from top-right)
            float lx = 0.5f, ly = 0.7f, lz = 0.5f;
            float ll = std::sqrt(lx*lx + ly*ly + lz*lz);
            lx /= ll; ly /= ll; lz /= ll;

            const int rings = 10, sectors = 14;
            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                float alpha = p.a;
                if (alpha < 0.001f) continue;
                float rad = p.size * 0.5f * globalScale;

                // Motion blur path: use elliptical fan (like Disc mode)
                if (motionBlur > 0.001f) {
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);
                    if (vLen > 0.0001f) {
                        float ddx = vPX / vLen, ddy = vPY / vLen, ddz = vPZ / vLen;
                        float ppx = fwdY * ddz - fwdZ * ddy;
                        float ppy = fwdZ * ddx - fwdX * ddz;
                        float ppz = fwdX * ddy - fwdY * ddx;
                        float pLen = std::sqrt(ppx*ppx + ppy*ppy + ppz*ppz);
                        if (pLen > 0.0001f) { ppx /= pLen; ppy /= pLen; ppz /= pLen; }

                        float halfLen = vLen * motionBlur * 0.5f + rad;
                        float halfWid = rad;
                        float cx = p.px - ddx * halfLen * 0.5f;
                        float cy = p.py - ddy * halfLen * 0.5f;
                        float cz = p.pz - ddz * halfLen * 0.5f;

                        const int segs = 16;
                        glBegin(GL_TRIANGLE_FAN);
                        glColor4f(p.r, p.g, p.b, alpha);
                        glVertex3f(p.px, p.py, p.pz);
                        glColor4f(p.r, p.g, p.b, 0.0f);
                        for (int s = 0; s <= segs; ++s) {
                            float a = 2.0f * (float)M_PI * s / segs;
                            float ca = std::cos(a), sa = std::sin(a);
                            float ex = ddx * ca * halfLen + ppx * sa * halfWid;
                            float ey = ddy * ca * halfLen + ppy * sa * halfWid;
                            float ez = ddz * ca * halfLen + ppz * sa * halfWid;
                            glVertex3f(cx + ex, cy + ey, cz + ez);
                        }
                        glEnd();
                        continue;
                    }
                }

                glBegin(GL_TRIANGLES);
                for (int r = 0; r < rings; ++r) {
                    float phi0 = (float)M_PI * r / rings;
                    float phi1 = (float)M_PI * (r + 1) / rings;
                    float cp0 = std::cos(phi0), sp0 = std::sin(phi0);
                    float cp1 = std::cos(phi1), sp1 = std::sin(phi1);

                    for (int s = 0; s < sectors; ++s) {
                        float th0 = 2.0f * (float)M_PI * s / sectors;
                        float th1 = 2.0f * (float)M_PI * (s + 1) / sectors;
                        float ct0 = std::cos(th0), st0 = std::sin(th0);
                        float ct1 = std::cos(th1), st1 = std::sin(th1);

                        // 4 vertices of the quad on the sphere surface
                        float nx00 = sp0*ct0, ny00 = cp0, nz00 = sp0*st0;
                        float nx10 = sp1*ct0, ny10 = cp1, nz10 = sp1*st0;
                        float nx01 = sp0*ct1, ny01 = cp0, nz01 = sp0*st1;
                        float nx11 = sp1*ct1, ny11 = cp1, nz11 = sp1*st1;

                        // Transform normals to world space for billboard
                        auto worldPos = [&](float nx, float ny, float nz, float& wx, float& wy, float& wz) {
                            wx = p.px + (rightX * nx + upX * ny + fwdX * nz) * rad;
                            wy = p.py + (rightY * nx + upY * ny + fwdY * nz) * rad;
                            wz = p.pz + (rightZ * nx + upZ * ny + fwdZ * nz) * rad;
                        };
                        auto shade = [&](float nx, float ny, float nz) -> float {
                            float d = nx * lx + ny * ly + nz * lz;
                            return 0.15f + 0.85f * std::max(0.0f, d); // ambient + diffuse
                        };

                        float wx, wy, wz;
                        // Triangle 1
                        float s00 = shade(nx00, ny00, nz00);
                        glColor4f(p.r * s00, p.g * s00, p.b * s00, alpha);
                        worldPos(nx00, ny00, nz00, wx, wy, wz); glVertex3f(wx, wy, wz);
                        float s10 = shade(nx10, ny10, nz10);
                        glColor4f(p.r * s10, p.g * s10, p.b * s10, alpha);
                        worldPos(nx10, ny10, nz10, wx, wy, wz); glVertex3f(wx, wy, wz);
                        float s11 = shade(nx11, ny11, nz11);
                        glColor4f(p.r * s11, p.g * s11, p.b * s11, alpha);
                        worldPos(nx11, ny11, nz11, wx, wy, wz); glVertex3f(wx, wy, wz);
                        // Triangle 2
                        glColor4f(p.r * s00, p.g * s00, p.b * s00, alpha);
                        worldPos(nx00, ny00, nz00, wx, wy, wz); glVertex3f(wx, wy, wz);
                        glColor4f(p.r * s11, p.g * s11, p.b * s11, alpha);
                        worldPos(nx11, ny11, nz11, wx, wy, wz); glVertex3f(wx, wy, wz);
                        float s01 = shade(nx01, ny01, nz01);
                        glColor4f(p.r * s01, p.g * s01, p.b * s01, alpha);
                        worldPos(nx01, ny01, nz01, wx, wy, wz); glVertex3f(wx, wy, wz);
                    }
                }
                glEnd();
            }

            // Restore for subsequent rendering
            if (motionBlur < 0.001f) {
                glDisable(GL_CULL_FACE);
                glDepthMask(GL_FALSE);
            }

        } else {
            // --- Sprite mode (camera-facing quad, with optional velocity stretch) ---
            glBegin(GL_QUADS);
            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                float alpha = p.a;
                if (alpha < 0.001f) continue;

                float hs = p.size * 0.5f * globalScale;

                if (motionBlur > 0.001f) {
                    // Elliptical triangle fan: soft in ALL directions
                    // Center at particle position (offset slightly toward velocity)
                    // Perimeter is an ellipse stretched along velocity
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);

                    if (vLen > 0.0001f) {
                        float dx = vPX / vLen, dy = vPY / vLen, dz = vPZ / vLen;
                        float px = fwdY * dz - fwdZ * dy;
                        float py = fwdZ * dx - fwdX * dz;
                        float pz = fwdX * dy - fwdY * dx;
                        float pLen = std::sqrt(px*px + py*py + pz*pz);
                        if (pLen > 0.0001f) { px /= pLen; py /= pLen; pz /= pLen; }

                        float halfLen = vLen * motionBlur * 0.5f + hs;
                        float halfWid = hs;
                        // Center position (between tail and head)
                        float cx = p.px - dx * halfLen * 0.5f + dx * halfLen * 0.5f;
                        float cy = p.py - dy * halfLen * 0.5f + dy * halfLen * 0.5f;
                        float cz = p.pz - dz * halfLen * 0.5f + dz * halfLen * 0.5f;
                        // Actually use particle position as the bright center (head end)
                        cx = p.px - dx * halfLen + dx * halfLen;
                        cy = p.py - dy * halfLen + dy * halfLen;
                        cz = p.pz - dz * halfLen + dz * halfLen;
                        // Center the ellipse so the bright spot is at the particle's current position
                        // (head of the streak), tail extends backward
                        float centerOffsetBack = halfLen * 0.5f; // shift center backward so head ends at p
                        cx = p.px - dx * centerOffsetBack;
                        cy = p.py - dy * centerOffsetBack;
                        cz = p.pz - dz * centerOffsetBack;
                        float ellLen = halfLen; // ellipse semi-major axis along velocity

                        glEnd(); // end QUADS block
                        const int segs = 16;
                        glBegin(GL_TRIANGLE_FAN);
                        glColor4f(p.r, p.g, p.b, alpha);
                        glVertex3f(p.px, p.py, p.pz); // bright center at particle position
                        glColor4f(p.r, p.g, p.b, 0.0f);
                        for (int s = 0; s <= segs; ++s) {
                            float a = 2.0f * (float)M_PI * s / segs;
                            float ca = std::cos(a), sa = std::sin(a);
                            // Ellipse: stretch along velocity (dx,dy,dz), width perpendicular (px,py,pz)
                            float ex = dx * ca * ellLen + px * sa * halfWid;
                            float ey = dy * ca * ellLen + py * sa * halfWid;
                            float ez = dz * ca * ellLen + pz * sa * halfWid;
                            glVertex3f(cx + ex, cy + ey, cz + ez);
                        }
                        glEnd();
                        glBegin(GL_QUADS); // resume quads
                        continue;
                    }
                }

                // No motion blur or stationary particle — normal camera-facing quad
                float rx = rightX * hs, ry = rightY * hs, rz = rightZ * hs;
                float ux = upX * hs,    uy = upY * hs,    uz = upZ * hs;

                glColor4f(p.r, p.g, p.b, alpha);
                glVertex3f(p.px - rx - ux, p.py - ry - uy, p.pz - rz - uz);
                glVertex3f(p.px + rx - ux, p.py + ry - uy, p.pz + rz - uz);
                glVertex3f(p.px + rx + ux, p.py + ry + uy, p.pz + rz + uz);
                glVertex3f(p.px - rx + ux, p.py - ry + uy, p.pz - rz + uz);
            }
            glEnd();
        }

        // Restore state
        glDepthMask(GL_TRUE);
        glDisable(GL_POINT_SMOOTH);
    }

    // --- Render geo instances at particle positions ---
    if (particleInstancer) {
        std::vector<ParticleInstance::GeoInstance> instances;
        particleInstancer->getInstances(args.time, instances);

        if (!instances.empty()) {
            // 3D geo with blending + back-face culling
            // Depth writes OFF only when using stretch cheat (translucent blending needs it)
            // Depth writes ON for solid geo in all other cases (including multi-sample mode)
            glEnable(GL_DEPTH_TEST);
            if (motionBlur > 0.001f) {
                glDepthMask(GL_FALSE); // stretch cheat mode — translucent
            } else {
                glDepthMask(GL_TRUE);  // solid opaque geo (multi-sample averages give the blur)
            }
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Collect mesh data from each connected geo input
            struct InstanceGeo {
                std::vector<float> verts; // x,y,z triples
                std::vector<int> tris;    // triangle indices
                bool valid;
                // Bounding box in local space (for per-vertex alpha normalization)
                float bboxMinY, bboxMaxY;
                float bboxCenterY, bboxHalfY;
            };
            InstanceGeo geos[4] = {};

            for (int g = 0; g < 4; ++g) {
                EffectInstancePtr geoInput = particleInstancer->getInput(g + 1);
                if (!geoInput) continue;

                // Try Cube3D
                Cube3D* cube = dynamic_cast<Cube3D*>(geoInput.get());
                if (cube) {
                    std::vector<Cube3D::CubeVertex> cv;
                    std::vector<int> ci;
                    cube->generateCubeMesh(args.time, cv, ci);
                    geos[g].verts.resize(cv.size() * 3);
                    for (size_t v = 0; v < cv.size(); ++v) {
                        geos[g].verts[v*3] = cv[v].x;
                        geos[g].verts[v*3+1] = cv[v].y;
                        geos[g].verts[v*3+2] = cv[v].z;
                    }
                    geos[g].tris = ci;
                    geos[g].valid = true;
                    continue;
                }

                // Try Sphere3D — generate a simple sphere mesh
                Sphere3D* sphere = dynamic_cast<Sphere3D*>(geoInput.get());
                if (sphere) {
                    const int rings = 16, sectors = 24;
                    float rad = 0.5f; // unit sphere, scaled by instance
                    for (int r = 0; r <= rings; ++r) {
                        float phi = (float)M_PI * r / rings;
                        for (int s = 0; s <= sectors; ++s) {
                            float theta = 2.0f * (float)M_PI * s / sectors;
                            geos[g].verts.push_back(rad * std::sin(phi) * std::cos(theta));
                            geos[g].verts.push_back(rad * std::cos(phi));
                            geos[g].verts.push_back(rad * std::sin(phi) * std::sin(theta));
                        }
                    }
                    for (int r = 0; r < rings; ++r) {
                        for (int s = 0; s < sectors; ++s) {
                            int i0 = r * (sectors + 1) + s;
                            int i1 = i0 + sectors + 1;
                            geos[g].tris.push_back(i0);
                            geos[g].tris.push_back(i1);
                            geos[g].tris.push_back(i0 + 1);
                            geos[g].tris.push_back(i0 + 1);
                            geos[g].tris.push_back(i1);
                            geos[g].tris.push_back(i1 + 1);
                        }
                    }
                    geos[g].valid = true;
                    continue;
                }
            }

            // Compute bounding box (Y axis) for each loaded geo — used for motion blur alpha fade
            for (int g = 0; g < 4; ++g) {
                if (!geos[g].valid || geos[g].verts.empty()) continue;
                float mnY = geos[g].verts[1];
                float mxY = geos[g].verts[1];
                for (size_t v = 1; v < geos[g].verts.size() / 3; ++v) {
                    float y = geos[g].verts[v * 3 + 1];
                    if (y < mnY) mnY = y;
                    if (y > mxY) mxY = y;
                }
                geos[g].bboxMinY = mnY;
                geos[g].bboxMaxY = mxY;
                geos[g].bboxCenterY = (mnY + mxY) * 0.5f;
                geos[g].bboxHalfY = (mxY - mnY) * 0.5f;
                if (geos[g].bboxHalfY < 0.0001f) geos[g].bboxHalfY = 0.0001f;
            }

            // Render each instance
            for (size_t i = 0; i < instances.size(); ++i) {
                const ParticleInstance::GeoInstance& inst = instances[i];
                int gi = inst.geoSourceIndex;
                if (gi < 0 || gi >= 4 || !geos[gi].valid) continue;

                glPushMatrix();
                glTranslatef(inst.px, inst.py, inst.pz);

                // Motion blur: stretch the instance along velocity direction
                if (motionBlur > 0.001f) {
                    float vLen = std::sqrt(inst.vx * inst.vx + inst.vy * inst.vy + inst.vz * inst.vz);
                    if (vLen > 0.0001f) {
                        // Build a frame where Y axis is along velocity
                        float vy_n = inst.vy / vLen;
                        float vx_n = inst.vx / vLen;
                        float vz_n = inst.vz / vLen;
                        // Rotation: align world Y to velocity direction
                        float angle = std::acos(std::max(-1.0f, std::min(1.0f, vy_n))) * 180.0f / (float)M_PI;
                        // Axis = cross(Y, velocity)
                        float axX = vz_n;
                        float axZ = -vx_n;
                        float axLen = std::sqrt(axX*axX + axZ*axZ);
                        if (axLen > 0.0001f && std::abs(angle) > 0.01f) {
                            glRotatef(angle, axX/axLen, 0, axZ/axLen);
                        }
                        // Stretch Y by speed * motionBlur, keep XZ uniform
                        float stretchAmt = 1.0f + vLen * motionBlur;
                        glScalef(inst.sx, inst.sy * stretchAmt, inst.sz);
                    } else {
                        if (inst.ry != 0) glRotatef(inst.ry, 0, 1, 0);
                        if (inst.rx != 0) glRotatef(inst.rx, 1, 0, 0);
                        if (inst.rz != 0) glRotatef(inst.rz, 0, 0, 1);
                        glScalef(inst.sx, inst.sy, inst.sz);
                    }
                } else {
                    if (inst.ry != 0) glRotatef(inst.ry, 0, 1, 0);
                    if (inst.rx != 0) glRotatef(inst.rx, 1, 0, 0);
                    if (inst.rz != 0) glRotatef(inst.rz, 0, 0, 1);
                    glScalef(inst.sx, inst.sy, inst.sz);
                }

                // Per-vertex alpha for cheat motion blur (stretch mode only).
                // motionBlur is already 0 when motionSamples > 1, so this is clean.
                bool useVertexAlpha = (motionBlur > 0.001f &&
                                        std::sqrt(inst.vx*inst.vx + inst.vy*inst.vy + inst.vz*inst.vz) > 0.0001f);

                const InstanceGeo& geo = geos[gi];
                glBegin(GL_TRIANGLES);
                for (size_t t = 0; t < geo.tris.size(); ++t) {
                    int vi = geo.tris[t];
                    float vx_local = geo.verts[vi*3];
                    float vy_local = geo.verts[vi*3+1];
                    float vz_local = geo.verts[vi*3+2];
                    if (useVertexAlpha) {
                        // Fade alpha by normalized distance from bbox center along local Y.
                        // Works for any geo regardless of size or Y range.
                        float normY = (vy_local - geo.bboxCenterY) / geo.bboxHalfY;
                        float t01 = std::abs(normY);
                        if (t01 > 1.0f) t01 = 1.0f;
                        float fade = 1.0f - t01 * 0.7f; // 30% min alpha at bbox extremes
                        float vertAlpha = inst.a * fade;
                        glColor4f(inst.r, inst.g, inst.b, vertAlpha);
                    } else {
                        glColor4f(inst.r, inst.g, inst.b, inst.a);
                    }
                    glVertex3f(vx_local, vy_local, vz_local);
                }
                glEnd();

                glPopMatrix();
            }

            // Restore state
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
        }
    }

    // --- Render ReadVDB volume with ray marching shader ---
    if (readVdb) {
        ReadVDB::VDBVolumeData vdbData;
        if (readVdb->getVolumeData(args.time, vdbData) && vdbData.resolution > 0) {
            GLuint volTex = 0;
            glGenTextures(1, &volTex);
            glBindTexture(GL_TEXTURE_3D, volTex);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_LUMINANCE, vdbData.resolution, vdbData.resolution, vdbData.resolution, 0,
                         GL_LUMINANCE, GL_FLOAT, vdbData.densityData.data());

            GLShaderPtr shader = std::make_shared<GLShader>();
            std::string shaderError;
            bool ok = shader->addShader(GLShader::eShaderTypeVertex, volumeVertexShader, &shaderError)
                   && shader->addShader(GLShader::eShaderTypeFragment, volumeFragmentShader, &shaderError)
                   && shader->link(&shaderError);

            if (ok) {
                // Apply ReadVDB transform to the bounding box
                double vtx, vty, vtz, vsx, vsy, vsz;
                readVdb->getTransform(args.time, vtx, vty, vtz, vsx, vsy, vsz);

                float x0 = vdbData.bboxMinX * (float)vsx + (float)vtx;
                float y0 = vdbData.bboxMinY * (float)vsy + (float)vty;
                float z0 = vdbData.bboxMinZ * (float)vsz + (float)vtz;
                float x1 = vdbData.bboxMaxX * (float)vsx + (float)vtx;
                float y1 = vdbData.bboxMaxY * (float)vsy + (float)vty;
                float z1 = vdbData.bboxMaxZ * (float)vsz + (float)vtz;

                // Step size relative to volume size
                float volSize = std::max({x1 - x0, y1 - y0, z1 - z0});
                float stepSize = volSize / (float)vdbData.resolution;

                shader->bind();
                U32 progId = shader->getShaderID();
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMin"), x0, y0, z0);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMax"), x1, y1, z1);
                glUniform3f(glGetUniformLocation(progId, "u_CameraPos"), (float)camTX, (float)camTY, (float)camTZ);
                glUniform1f(glGetUniformLocation(progId, "u_Density"), vdbData.density);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeColor"), vdbData.colorR, vdbData.colorG, vdbData.colorB);
                glUniform1f(glGetUniformLocation(progId, "u_StepSize"), stepSize);
                glUniform1i(glGetUniformLocation(progId, "u_VolumeData"), 0);

                // Light uniforms
                if (light3d) {
                    double ltx, lty, ltz, lr, lg, lb, lint;
                    double lexp_unused;
                    light3d->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp_unused);
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 1);
                    glUniform3f(glGetUniformLocation(progId, "u_LightPos"), (float)ltx, (float)lty, (float)ltz);
                    glUniform3f(glGetUniformLocation(progId, "u_LightColor"), (float)lr, (float)lg, (float)lb);
                    glUniform1f(glGetUniformLocation(progId, "u_LightIntensity"), (float)lint);

                    // Get shadow params from light's knobs
                    KnobIPtr sdKnob = light3d->getKnobByName("shadowDensity");
                    KnobIPtr ssKnob = light3d->getKnobByName("shadowSteps");
                    float shadowDens = sdKnob ? (float)dynamic_cast<KnobDouble*>(sdKnob.get())->getValueAtTime(args.time) : 1.0f;
                    int shadowSteps = ssKnob ? dynamic_cast<KnobInt*>(ssKnob.get())->getValueAtTime(args.time) : 16;
                    glUniform1f(glGetUniformLocation(progId, "u_ShadowDensity"), shadowDens);
                    glUniform1i(glGetUniformLocation(progId, "u_ShadowSteps"), shadowSteps);
                } else {
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 0);
                }

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_3D, volTex);

                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_FRONT);

                glBegin(GL_QUADS);
                // 6 faces
                glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0); glVertex3f(x0,y1,z0);
                glVertex3f(x0,y0,z1); glVertex3f(x0,y1,z1); glVertex3f(x1,y1,z1); glVertex3f(x1,y0,z1);
                glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z0); glVertex3f(x0,y1,z1); glVertex3f(x0,y0,z1);
                glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z1); glVertex3f(x1,y1,z0);
                glVertex3f(x0,y0,z0); glVertex3f(x0,y0,z1); glVertex3f(x1,y0,z1); glVertex3f(x1,y0,z0);
                glVertex3f(x0,y1,z0); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1);
                glEnd();

                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                shader->unbind();
            }
            glDeleteTextures(1, &volTex);
        }
    }

    // --- Render procedural volume with ray marching shader ---
    if (volume3d) {
        Volume3D::VolumeParams vp = volume3d->getVolumeParams(args.time);

        // Generate 3D density data
        std::vector<float> volData;
        int volRes = 0;
        volume3d->generateVolumeData(args.time, volData, volRes);

        if (volRes > 0 && !volData.empty()) {
            // Upload 3D texture
            GLuint volTex = 0;
            glGenTextures(1, &volTex);
            glBindTexture(GL_TEXTURE_3D, volTex);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_LUMINANCE, volRes, volRes, volRes, 0,
                         GL_LUMINANCE, GL_FLOAT, volData.data());

            // Compile ray marching shader
            GLShaderPtr shader = std::make_shared<GLShader>();
            std::string shaderError;
            bool vsOk = shader->addShader(GLShader::eShaderTypeVertex, volumeVertexShader, &shaderError);
            bool fsOk = shader->addShader(GLShader::eShaderTypeFragment, volumeFragmentShader, &shaderError);
            bool linkOk = vsOk && fsOk && shader->link(&shaderError);

            if (linkOk) {
                // Volume bounding box in world space
                float halfX = vp.scaleX * 0.5f;
                float halfY = vp.scaleY * 0.5f;
                float halfZ = vp.scaleZ * 0.5f;

                float volMinX = vp.centerX - halfX;
                float volMinY = vp.centerY - halfY;
                float volMinZ = vp.centerZ - halfZ;
                float volMaxX = vp.centerX + halfX;
                float volMaxY = vp.centerY + halfY;
                float volMaxZ = vp.centerZ + halfZ;

                shader->bind();

                // Set uniforms
                U32 progId = shader->getShaderID();
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMin"), volMinX, volMinY, volMinZ);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMax"), volMaxX, volMaxY, volMaxZ);
                glUniform3f(glGetUniformLocation(progId, "u_CameraPos"), (float)camTX, (float)camTY, (float)camTZ);
                glUniform1f(glGetUniformLocation(progId, "u_Density"), vp.density);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeColor"), vp.colorR, vp.colorG, vp.colorB);
                glUniform1f(glGetUniformLocation(progId, "u_StepSize"), 0.02f);
                glUniform1i(glGetUniformLocation(progId, "u_VolumeData"), 0);

                // Light uniforms for procedural volume
                if (light3d) {
                    double ltx, lty, ltz, lr, lg, lb, lint;
                    double lexp_unused;
                    light3d->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp_unused);
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 1);
                    glUniform3f(glGetUniformLocation(progId, "u_LightPos"), (float)ltx, (float)lty, (float)ltz);
                    glUniform3f(glGetUniformLocation(progId, "u_LightColor"), (float)lr, (float)lg, (float)lb);
                    glUniform1f(glGetUniformLocation(progId, "u_LightIntensity"), (float)lint);
                    KnobIPtr sdKnob = light3d->getKnobByName("shadowDensity");
                    KnobIPtr ssKnob = light3d->getKnobByName("shadowSteps");
                    float shadowDens = sdKnob ? (float)dynamic_cast<KnobDouble*>(sdKnob.get())->getValueAtTime(args.time) : 1.0f;
                    int shadowSteps = ssKnob ? dynamic_cast<KnobInt*>(ssKnob.get())->getValueAtTime(args.time) : 16;
                    glUniform1f(glGetUniformLocation(progId, "u_ShadowDensity"), shadowDens);
                    glUniform1i(glGetUniformLocation(progId, "u_ShadowSteps"), shadowSteps);
                } else {
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 0);
                }

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_3D, volTex);

                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);

                // Draw proxy cube (back faces for correct ray entry when camera outside)
                glEnable(GL_CULL_FACE);
                glCullFace(GL_FRONT); // render back faces

                glBegin(GL_QUADS);
                // -Z face
                glVertex3f(volMinX, volMinY, volMinZ);
                glVertex3f(volMaxX, volMinY, volMinZ);
                glVertex3f(volMaxX, volMaxY, volMinZ);
                glVertex3f(volMinX, volMaxY, volMinZ);
                // +Z face
                glVertex3f(volMinX, volMinY, volMaxZ);
                glVertex3f(volMinX, volMaxY, volMaxZ);
                glVertex3f(volMaxX, volMaxY, volMaxZ);
                glVertex3f(volMaxX, volMinY, volMaxZ);
                // -X face
                glVertex3f(volMinX, volMinY, volMinZ);
                glVertex3f(volMinX, volMaxY, volMinZ);
                glVertex3f(volMinX, volMaxY, volMaxZ);
                glVertex3f(volMinX, volMinY, volMaxZ);
                // +X face
                glVertex3f(volMaxX, volMinY, volMinZ);
                glVertex3f(volMaxX, volMinY, volMaxZ);
                glVertex3f(volMaxX, volMaxY, volMaxZ);
                glVertex3f(volMaxX, volMaxY, volMinZ);
                // -Y face
                glVertex3f(volMinX, volMinY, volMinZ);
                glVertex3f(volMinX, volMinY, volMaxZ);
                glVertex3f(volMaxX, volMinY, volMaxZ);
                glVertex3f(volMaxX, volMinY, volMinZ);
                // +Y face
                glVertex3f(volMinX, volMaxY, volMinZ);
                glVertex3f(volMaxX, volMaxY, volMinZ);
                glVertex3f(volMaxX, volMaxY, volMaxZ);
                glVertex3f(volMinX, volMaxY, volMaxZ);
                glEnd();

                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);

                shader->unbind();
            }

            glDeleteTextures(1, &volTex);
        }
    }

    glDisable(GL_BLEND);

        // --- Resolve MSAA: blit multisampled FBO to single-sample FBO ---
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, outW, outH,
                          0, 0, outW, outH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Bind resolve FBO for readback
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        // --- Read back this sample's pixels ---
        std::vector<float> samplePixels(outW * outH * 4);
        glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, samplePixels.data());

        // Accumulate into the accumulator
        if (accumPixels.empty()) {
            accumPixels = samplePixels;
        } else {
            for (size_t i = 0; i < accumPixels.size(); ++i) {
                accumPixels[i] += samplePixels[i];
            }
        }
    } // === END MULTI-SAMPLE RENDER LOOP ===

    // Restore original particle positions
    if (motionSamples > 1 && motionBlurPData) {
        for (size_t i = 0; i < motionBlurPData->particles.size(); ++i) {
            Particle& p = motionBlurPData->particles[i];
            p.px = origParticlePos[i][0];
            p.py = origParticlePos[i][1];
            p.pz = origParticlePos[i][2];
        }
    }

    // Average the accumulator to get final pixels
    std::vector<float> pixels;
    if (motionSamples > 1) {
        pixels = std::move(accumPixels);
        float invN = 1.0f / (float)motionSamples;
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] *= invN;
        }
    } else {
        pixels = std::move(accumPixels);
    }

    // --- Composite with background if connected ---
    ImagePtr bgImg;
    EffectInstancePtr bgEffect = getInput(0);
    if (bgEffect) {
        RectI bgRoi;
        bgImg = getImage(0, args.time, RenderScale(), args.view,
                         NULL, NULL, false, true,
                         eStorageModeRAM, 0, &bgRoi);
    }

    // Copy to output
    RectI outBounds = outImg->getBounds();
    {
        Image::WriteAccess wa(outImg.get());
        Image::ReadAccess* bgRa = bgImg ? new Image::ReadAccess(bgImg.get()) : NULL;
        RectI bgBounds;
        if (bgImg) bgBounds = bgImg->getBounds();

        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;

                int fbX = x - outBounds.x1;
                int fbY = y - outBounds.y1;

                float fgR = 0, fgG = 0, fgB = 0, fgA = 0;
                if (fbX >= 0 && fbX < outW && fbY >= 0 && fbY < outH) {
                    int idx = (fbY * outW + fbX) * 4;
                    fgR = pixels[idx + 0];
                    fgG = pixels[idx + 1];
                    fgB = pixels[idx + 2];
                    fgA = pixels[idx + 3];
                }

                // Over composite: fg over bg
                if (bgRa && bgBounds.contains(x, y)) {
                    const float* bgPix = (const float*)bgRa->pixelAt(x, y);
                    if (bgPix) {
                        float bgR = bgPix[0], bgG = bgPix[1], bgB = bgPix[2], bgA = bgPix[3];
                        dst[0] = fgR + bgR * (1.0f - fgA);
                        dst[1] = fgG + bgG * (1.0f - fgA);
                        dst[2] = fgB + bgB * (1.0f - fgA);
                        dst[3] = fgA + bgA * (1.0f - fgA);
                        continue;
                    }
                }

                dst[0] = fgR;
                dst[1] = fgG;
                dst[2] = fgB;
                dst[3] = fgA;
            }
        }
        delete bgRa;
    }

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteRenderbuffers(1, &depthRB);
    glDeleteFramebuffers(1, &msFBO);
    glDeleteRenderbuffers(1, &msColorRB);
    glDeleteRenderbuffers(1, &msDepthRB);

    OSGLContext::unsetCurrentContextNoRender();
    pool->releaseGLContextFromRender(glContext);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ScanlineRender.cpp"
