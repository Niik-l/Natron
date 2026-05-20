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

#include "Project3D.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include "CameraMath.h"
#include "RotationConventions.h"

#include "../../../Global/GLIncludes.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "CameraProvider.h"
#include "../../GPUContextPool.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../OSGLContext.h"
#include "ReadGeo.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct Project3DPrivate
{
    // Options
    KnobChoiceWPtr projectOn; // Front/Back/Both
    KnobBoolWPtr   cropAtEdges;

    // Output
    KnobIntWPtr outputWidth, outputHeight;
};


Project3D::Project3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Project3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Project3D::~Project3D()
{
}

std::string
Project3D::getPluginDescription() const
{
    return tr("Project a 2D image through a camera onto 3D geometry.\n\n"
              "Input 0 (img): The 2D image to project\n"
              "Input 1 (projCam): Projection camera (Camera3D or ReadAlembicCamera)\n"
              "Input 2 (geo): Geometry to project onto (ReadGeo — optional, uses flat card)\n"
              "Input 3 (renderCam): Render camera — the output viewpoint\n\n"
              "Connect Camera3D or ReadAlembicCamera nodes to the camera inputs.\n"
              "Move the render camera to see parallax.").toStdString();
}

std::string
Project3D::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "img";
        case 1: return "projCam";
        case 2: return "geo";
        case 3: return "renderCam";
        default: return "";
    }
}

bool
Project3D::isInputOptional(int inputNb) const
{
    // img and projCam required; geo and renderCam optional
    return (inputNb == 2 || inputNb == 3);
}

void
Project3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Project3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Project3D::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== Knobs ====================

void
Project3D::initializeKnobs()
{
    // --- Options page ---
    KnobPagePtr optPage = AppManager::createKnob<KnobPage>(this, tr("Options"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Project On"));
        k->setName("projectOn");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("front", "Front Face", "Project only on front-facing surfaces"));
        entries.push_back(ChoiceOption("back", "Back Face", "Project only on back-facing surfaces"));
        entries.push_back(ChoiceOption("both", "Both Faces", "Project on all surfaces"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        optPage->addKnob(k); _imp->projectOn = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Crop at Image Edges"));
        k->setName("cropAtEdges"); k->setDefaultValue(true);
        optPage->addKnob(k); _imp->cropAtEdges = k;
    }

    // --- Output page ---
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

// ==================== Matrix helpers ====================

void
Project3D::buildViewMatrix(double tx, double ty, double tz,
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

// Project3D::buildProjectionMatrix removed — projection is now built via
// CameraMath::composeProjectionMatrix (independent fov_h / fov_v from both
// apertures). The old single-FOV form forced fy = fx / image_aspect, which
// produced V scaling tied to the render aspect instead of to the camera's V
// aperture — causing CG drift under camera motion when sensor aspect != image
// aspect.

// ==================== RoD ====================

StatusEnum
Project3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                 ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0;
    rod->y1 = 0;
    rod->x2 = _imp->outputWidth.lock()->getValue();
    rod->y2 = _imp->outputHeight.lock()->getValue();
    return eStatusOK;
}

// ==================== Render ====================

static void
mat4Multiply(const float a[16], const float b[16], float out[16])
{
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

static void
transformPoint(const float m[16], float x, float y, float z,
               float& outX, float& outY, float& outZ, float& outW)
{
    outX = m[0]*x + m[4]*y + m[8]*z  + m[12];
    outY = m[1]*x + m[5]*y + m[9]*z  + m[13];
    outZ = m[2]*x + m[6]*y + m[10]*z + m[14];
    outW = m[3]*x + m[7]*y + m[11]*z + m[15];
}

StatusEnum
Project3D::render(const RenderActionArgs& args)
{
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;
    if (!outImg) return eStatusFailed;

    int outW = _imp->outputWidth.lock()->getValue();
    int outH = _imp->outputHeight.lock()->getValue();

    // --- Get projection camera from input 1 ---
    EffectInstancePtr projCamEffect = getInput(1);
    CameraProvider* projCam = projCamEffect ? dynamic_cast<CameraProvider*>(projCamEffect.get()) : NULL;
    if (!projCam) return eStatusFailed; // Projection camera is required

    double projTX, projTY, projTZ, projRX, projRY, projRZ;
    projCam->getCameraPosition(args.time, projTX, projTY, projTZ, projRX, projRY, projRZ);
    double projFL = projCam->getCameraFocalLength(args.time);
    double projHA = projCam->getCameraHAperture(args.time);
    double projVA = projCam->getCameraVAperture(args.time);

    // --- Get render camera from input 3 (optional — falls back to projection camera) ---
    EffectInstancePtr renCamEffect = getInput(3);
    CameraProvider* renCam = renCamEffect ? dynamic_cast<CameraProvider*>(renCamEffect.get()) : NULL;
    if (!renCam) renCam = projCam; // Use projection camera if no render camera

    double renTX, renTY, renTZ, renRX, renRY, renRZ;
    renCam->getCameraPosition(args.time, renTX, renTY, renTZ, renRX, renRY, renRZ);
    double renFL = renCam->getCameraFocalLength(args.time);
    double renHA = renCam->getCameraHAperture(args.time);
    double renVA = renCam->getCameraVAperture(args.time);

    // --- Get input image (input 0) ---
    EffectInstancePtr imgInput = getInput(0);
    if (!imgInput) return eStatusFailed;

    RectI roiPixel;
    ImagePtr srcImg = getImage(0, args.time, RenderScale(), args.view,
                               NULL, NULL, false, true,
                               eStorageModeRAM, 0, &roiPixel);
    if (!srcImg) return eStatusFailed;

    RectI srcBounds = srcImg->getBounds();
    int srcW = srcBounds.width();
    int srcH = srcBounds.height();
    if (srcW <= 0 || srcH <= 0) return eStatusFailed;

    // --- Get geometry from input 2 (optional — uses flat card) ---
    std::vector<float> vertices;
    std::vector<int> triangleIndices;
    float geoTransform[16];
    for (int i = 0; i < 16; ++i) geoTransform[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    EffectInstancePtr geoInput = getInput(2);
    ReadGeo* readGeo = geoInput ? dynamic_cast<ReadGeo*>(geoInput.get()) : NULL;
    MeshDataPtr mesh;

    if (readGeo) {
        mesh = readGeo->getMeshData(args.time);
    }

    if (mesh && mesh->numVertices > 0) {
        vertices = mesh->vertices;
        triangleIndices = mesh->faceIndices;
        std::memcpy(geoTransform, mesh->transform, 16 * sizeof(float));
    } else {
        // Built-in card matching input image aspect ratio
        float imgAspect = (float)srcW / std::max(1, srcH);
        float hw = imgAspect * 0.5f;
        float hh = 0.5f;

        float cardVerts[] = {
            -hw, -hh, 0.0f,
             hw, -hh, 0.0f,
             hw,  hh, 0.0f,
            -hw,  hh, 0.0f,
        };
        vertices.assign(cardVerts, cardVerts + 12);

        int cardIdx[] = { 0, 1, 2,  0, 2, 3 };
        triangleIndices.assign(cardIdx, cardIdx + 6);
    }

    int numVerts = (int)(vertices.size() / 3);
    int numTris = (int)(triangleIndices.size() / 3);
    if (numVerts == 0 || numTris == 0) return eStatusFailed;

    // --- Build projection camera matrices ---
    // Projection uses both apertures from the projection camera. srcW/srcH are
    // NOT involved — see CameraMath.h.
    float projView[16], projProj[16], projVP[16];
    buildViewMatrix(projTX, projTY, projTZ, projRX, projRY, projRZ, projView);
    (void)srcW; (void)srcH;
    float projNear = (float)projCam->getCameraNear(args.time);
    float projFar = (float)projCam->getCameraFar(args.time);
    CameraMath::composeProjectionMatrix(projFL, projHA, projVA, projNear, projFar, projProj);
    mat4Multiply(projProj, projView, projVP);

    // --- Compute projective texture coordinates ---
    bool cropAtEdges = _imp->cropAtEdges.lock()->getValue();
    int projectOnMode = _imp->projectOn.lock()->getValue();

    std::vector<float> texCoords(numVerts * 2);

    // Geometry transform: row-major from Imath → column-major
    float geoTransformCM[16];
    if (mesh && mesh->numVertices > 0) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                geoTransformCM[c * 4 + r] = geoTransform[r * 4 + c];
    } else {
        std::memcpy(geoTransformCM, geoTransform, 16 * sizeof(float));
    }

    for (int v = 0; v < numVerts; ++v) {
        float lx = vertices[v * 3 + 0];
        float ly = vertices[v * 3 + 1];
        float lz = vertices[v * 3 + 2];

        // Local → world
        float wx, wy, wz, ww;
        transformPoint(geoTransformCM, lx, ly, lz, wx, wy, wz, ww);
        if (std::fabs(ww) > 1e-6f) { wx /= ww; wy /= ww; wz /= ww; }

        // World → projection camera clip space
        float cx, cy, cz, cw;
        transformPoint(projVP, wx, wy, wz, cx, cy, cz, cw);

        // Perspective divide → NDC → UV
        float u = 0.5f, vc = 0.5f;
        if (std::fabs(cw) > 1e-6f) {
            u = (cx / cw + 1.0f) * 0.5f;
            vc = (cy / cw + 1.0f) * 0.5f;
        }

        if (cropAtEdges) {
            if (u < 0.0f || u > 1.0f || vc < 0.0f || vc > 1.0f) {
                u = -1.0f;
                vc = -1.0f;
            }
        } else {
            u = std::max(0.0f, std::min(1.0f, u));
            vc = std::max(0.0f, std::min(1.0f, vc));
        }

        texCoords[v * 2 + 0] = u;
        texCoords[v * 2 + 1] = vc;
    }

    // --- Acquire GL context for FBO rendering ---
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

    // --- Set up render camera ---
    // Projection uses both apertures from the render camera. outW/outH are
    // NOT involved.
    float renView[16], renProj[16];
    (void)outW; (void)outH;
    float renNear = (float)renCam->getCameraNear(args.time);
    float renFar = (float)renCam->getCameraFar(args.time);
    buildViewMatrix(renTX, renTY, renTZ, renRX, renRY, renRZ, renView);
    CameraMath::composeProjectionMatrix(renFL, renHA, renVA, renNear, renFar, renProj);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(renProj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(renView);

    // --- Upload source image as texture ---
    GLuint srcTex = 0;
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<float> texData(srcW * srcH * 4, 0.0f);
    {
        Image::ReadAccess ra(srcImg.get());
        for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
            for (int x = srcBounds.x1; x < srcBounds.x2; ++x) {
                const float* pix = (const float*)ra.pixelAt(x, y);
                if (pix) {
                    int idx = ((y - srcBounds.y1) * srcW + (x - srcBounds.x1)) * 4;
                    texData[idx + 0] = pix[0];
                    texData[idx + 1] = pix[1];
                    texData[idx + 2] = pix[2];
                    texData[idx + 3] = (srcImg->getComponents().getNumComponents() >= 4) ? pix[3] : 1.0f;
                }
            }
        }
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, srcW, srcH, 0, GL_RGBA, GL_FLOAT, texData.data());

    // --- Render geometry with projected texture ---
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPushMatrix();
    glMultMatrixf(geoTransformCM);

    if (projectOnMode == 0) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else if (projectOnMode == 1) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_TRIANGLES);
    for (int t = 0; t < numTris; ++t) {
        for (int vi = 0; vi < 3; ++vi) {
            int idx = triangleIndices[t * 3 + vi];
            if (idx < 0 || idx >= numVerts) continue;

            float u = texCoords[idx * 2 + 0];
            float v = texCoords[idx * 2 + 1];

            if (u < 0.0f) {
                glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
            } else {
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            }

            glTexCoord2f(u, v);
            glVertex3f(vertices[idx * 3 + 0],
                       vertices[idx * 3 + 1],
                       vertices[idx * 3 + 2]);
        }
    }
    glEnd();

    if (projectOnMode < 2) {
        glDisable(GL_CULL_FACE);
    }

    glPopMatrix();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    // --- Read back pixels ---
    std::vector<float> pixels(outW * outH * 4);
    glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, pixels.data());

    RectI outBounds = outImg->getBounds();
    {
        Image::WriteAccess wa(outImg.get());
        for (int y = outBounds.y1; y < outBounds.y2; ++y) {
            for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;

                int fbX = x - outBounds.x1;
                int fbY = y - outBounds.y1;
                if (fbX >= 0 && fbX < outW && fbY >= 0 && fbY < outH) {
                    int idx = (fbY * outW + fbX) * 4;
                    dst[0] = pixels[idx + 0];
                    dst[1] = pixels[idx + 1];
                    dst[2] = pixels[idx + 2];
                    dst[3] = pixels[idx + 3];
                }
            }
        }
    }

    // Cleanup
    glDeleteTextures(1, &srcTex);
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

#include "moc_Project3D.cpp"
