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

#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include "../../../Global/GLIncludes.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "CameraProvider.h"
#include "Card3D.h"
#include "../../GLShader.h"
#include "Light3D.h"
#include "../Particles/ParticleData.h"
#include "../Particles/ParticleEmitter.h"
#include "../Particles/ParticleGravity.h"
#include "ReadVDB.h"
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
    float crx = cosf((float)rx * (float)M_PI / 180.0f), srx = sinf((float)rx * (float)M_PI / 180.0f);
    float cry = cosf((float)ry * (float)M_PI / 180.0f), sry = sinf((float)ry * (float)M_PI / 180.0f);
    float crz = cosf((float)rz * (float)M_PI / 180.0f), srz = sinf((float)rz * (float)M_PI / 180.0f);

    float nsrx = -srx, nsry = -sry, nsrz = -srz;

    float r00 = crz * cry + nsrz * nsrx * nsry;
    float r01 = nsrz * crx;
    float r02 = -crz * nsry + nsrz * nsrx * cry;

    float r10 = -nsrz * cry + crz * nsrx * nsry;
    float r11 = crz * crx;
    float r12 = nsrz * nsry + crz * nsrx * cry;

    float r20 = crx * nsry;
    float r21 = -nsrx;
    float r22 = crx * cry;

    float ntx = -(float)tx, nty = -(float)ty, ntz = -(float)tz;

    out[0]  = r00; out[1]  = r10; out[2]  = r20; out[3]  = 0;
    out[4]  = r01; out[5]  = r11; out[6]  = r21; out[7]  = 0;
    out[8]  = r02; out[9]  = r12; out[10] = r22; out[11] = 0;
    out[12] = r00*ntx + r01*nty + r02*ntz;
    out[13] = r10*ntx + r11*nty + r12*ntz;
    out[14] = r20*ntx + r21*nty + r22*ntz;
    out[15] = 1;
}

static void
buildProjectionMatrix(double focalLength, double hAperture,
                      float aspect, float nearZ, float farZ,
                      float out[16])
{
    double fovDeg = 2.0 * std::atan(hAperture / (2.0 * focalLength)) * (180.0 / M_PI);
    float f = 1.0f / tanf((float)fovDeg * 0.5f * (float)M_PI / 180.0f);

    std::memset(out, 0, 16 * sizeof(float));
    out[0]  = f / aspect;
    out[5]  = f;
    out[10] = (farZ + nearZ) / (nearZ - farZ);
    out[11] = -1.0f;
    out[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
}

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
        int nv = (int)(out.verts.size() / 3);
        out.uvs.resize(nv * 2, 0.5f);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out.localMatrix[c * 4 + r] = mesh->transform[r * 4 + c];
        return true;
    }

    return false;
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
    double camFL = 50.0, camHA = 24.576;
    float camNear = 0.1f, camFar = 10000.0f;

    if (cam) {
        cam->getCameraPosition(args.time, camTX, camTY, camTZ, camRX, camRY, camRZ);
        camFL = cam->getCameraFocalLength(args.time);
        camHA = cam->getCameraHAperture(args.time);
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

    // Check for particle nodes
    ParticleEmitter* pEmitter = dynamic_cast<ParticleEmitter*>(geoEffect.get());
    ParticleGravity* pGravity = dynamic_cast<ParticleGravity*>(geoEffect.get());
    if (pEmitter) {
        particleData = pEmitter->getParticleData(args.time);
    } else if (pGravity) {
        particleData = pGravity->getParticleData(args.time);
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

                // Check for particles in scene
                ParticleEmitter* sEmitter = dynamic_cast<ParticleEmitter*>(sceneInput.get());
                ParticleGravity* sGravity = dynamic_cast<ParticleGravity*>(sceneInput.get());
                if (sEmitter) {
                    particleData = sEmitter->getParticleData(args.time);
                } else if (sGravity) {
                    particleData = sGravity->getParticleData(args.time);
                } else {
                    GeoData geo;
                    if (extractGeometry(sceneInput, args.time, args.view, geo)) {
                        geoObjects.push_back(geo);
                    }
                }
            }
        } else {
            GeoData geo;
            if (extractGeometry(geoEffect, args.time, args.view, geo)) {
                geoObjects.push_back(geo);
            }
        }
    }

    if (geoObjects.empty() && !particleData && !volume3d && !readVdb) return eStatusFailed;

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

    // --- Create FBO ---
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
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &colorTex);
        glDeleteRenderbuffers(1, &depthRB);
        OSGLContext::unsetCurrentContextNoRender();
        pool->releaseGLContextFromRender(glContext);
        return eStatusFailed;
    }

    glViewport(0, 0, outW, outH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // --- Set up camera matrices ---
    float viewMatrix[16], projMatrix[16];
    float aspect = (float)outW / std::max(1, outH);
    buildViewMatrix(camTX, camTY, camTZ, camRX, camRY, camRZ, viewMatrix);
    buildProjectionMatrix(camFL, camHA, aspect, camNear, camFar, projMatrix);

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

    // --- Render particles as billboarded quads ---
    if (particleData && particleData->numParticles() > 0) {
        // Get camera right/up vectors from modelview matrix for billboarding
        float mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        float rightX = mv[0], rightY = mv[4], rightZ = mv[8];
        float upX    = mv[1], upY    = mv[5], upZ    = mv[9];

        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive for bright particles

        glBegin(GL_QUADS);
        for (int i = 0; i < particleData->numParticles(); ++i) {
            const Particle& p = particleData->particles[i];
            float ageFrac = (p.life > 0) ? (p.age / p.life) : 1.0f;
            float alpha = p.a * (1.0f - ageFrac);
            if (alpha < 0.001f) continue;

            float hs = p.size * 0.5f;
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

    // --- Read back pixels ---
    std::vector<float> pixels(outW * outH * 4);
    glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, pixels.data());

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

    OSGLContext::unsetCurrentContextNoRender();
    pool->releaseGLContextFromRender(glContext);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ScanlineRender.cpp"
