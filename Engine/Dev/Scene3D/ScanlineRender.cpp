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
#include <cstdio>   // fprintf/fflush — used by Phase 3 GLSL helpers
#include <cstring>
#include <vector>

#include "RotationConventions.h"

#include "../../../Global/GLIncludes.h"

#include "../../AppInstance.h"
#include "../../Format.h"
#include "../../Project.h"
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
#include "UVProject.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct ScanlineRenderPrivate
{
    // Output
    KnobIntWPtr outputWidth, outputHeight;
    KnobButtonWPtr syncToProject; // copies project default format → width/height

    // Particle rendering
    KnobChoiceWPtr particleMode;   // Point, Disc, Sphere, Sprite
    KnobChoiceWPtr particleBlend;  // Additive, Over
    KnobDoubleWPtr particleScale;  // global size multiplier
    KnobDoubleWPtr particleMotionBlur; // velocity stretch amount (legacy cheat mode)
    KnobIntWPtr motionSamples;      // number of sub-frame samples (1 = off)
    KnobDoubleWPtr motionShutter;   // shutter open fraction (0-1, default 0.5)

    // Phase 3 experimental — GLSL/MRT pipeline. Default OFF until the GLSL
    // beauty render path lands (Phase 3C). When ON, the render() method
    // uses the GLSL/MRT path instead of fixed-function GL. The two paths
    // co-exist during the migration; Phase 3E retires fixed-function.
    KnobBoolWPtr useGlslPipeline;

    // Phase 3D — per-pixel AOV outputs. Each AOV is produced only when its
    // knob is on AND (for the MRT ones) the GLSL pipeline is on. Depth and
    // World Position are the exception: they come from the existing depth
    // attachment + inverse(MVP) and work under both fixed-function and GLSL.
    KnobBoolWPtr outputDepth;     // depth.Z plane (linear camera-space distance)
    KnobBoolWPtr outputPosition;  // world_position.xyz plane (reconstructed from depth)
    KnobBoolWPtr outputNormal;
    KnobBoolWPtr outputUV;
    KnobBoolWPtr outputPref;
    KnobBoolWPtr outputVelocity;
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
              "Comparable to scanline-render nodes found in other compositing DCCs.").toStdString();
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
    // RGBA for beauty. RGB for world_position / Normal / Pref / Velocity / UV
    // (3-channel AOVs). Alpha (1-channel) for depth. Without these, Natron may
    // refuse to allocate the AOV planes with correct bounds.
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
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
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Sync to Project"));
        k->setName("syncToProject");
        k->setHintToolTip(tr("Copy the current project default format's width and "
                             "height into the Width/Height knobs above."));
        outPage->addKnob(k); _imp->syncToProject = k;
    }

    // Phase 3 — GLSL/MRT pipeline toggle. Off by default while migration is
    // in progress; the GLSL beauty path lands in Phase 3C and the MRT AOVs in
    // Phase 3D. The fixed-function path stays the default until Phase 3E
    // retires it. When ON: scene draws via GLSL 3.3 core shaders, MRT enables
    // per-fragment AOV outputs (Normal / UV / Pref / Velocity).
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Use GLSL pipeline"));
        k->setName("useGlslPipeline");
        k->setDefaultValue(false);
        k->setHintToolTip(tr("EXPERIMENTAL (Phase 3 migration). Toggle the GLSL/MRT render path. "
                              "When OFF, uses the legacy fixed-function GL pipeline. "
                              "When ON, scene draws via GLSL 3.3 core shaders — required for "
                              "the Normal / UV / Pref / Velocity AOVs added in Phase 3D."));
        outPage->addKnob(k); _imp->useGlslPipeline = k;
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

    // Phase 3D — AOV outputs page. Each toggle adds a per-pixel arbitrary
    // output variable on top of beauty. They only fire when the GLSL pipeline
    // (Output page) is also on; otherwise the legacy fixed-function path
    // doesn't have a way to emit them.
    KnobPagePtr aovPage = AppManager::createKnob<KnobPage>(this, tr("AOVs"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Depth"));
        k->setName("outputDepth"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel linear camera-space distance (depth.Z). "
                              "Background pixels emit the camera Far value. Works "
                              "under both fixed-function and GLSL pipelines."));
        aovPage->addKnob(k); _imp->outputDepth = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("World Position"));
        k->setName("outputPosition"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel world-space surface position (world_position.x/y/z), "
                              "reconstructed from depth via inverse(proj * view). Background "
                              "pixels emit (0,0,0). Works under both fixed-function and GLSL."));
        aovPage->addKnob(k); _imp->outputPosition = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normal"));
        k->setName("outputNormal"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel surface normal in world space (Normal.x/y/z). "
                              "Requires the GLSL pipeline (Output page)."));
        aovPage->addKnob(k); _imp->outputNormal = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("UV"));
        k->setName("outputUV"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel texture coordinate (uv.u/v/w). "
                              "Requires the GLSL pipeline (Output page)."));
        aovPage->addKnob(k); _imp->outputUV = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Pref"));
        k->setName("outputPref"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel reference position in object space (Pref.x/y/z). "
                              "Useful as a texture-projection key. Requires the GLSL pipeline."));
        aovPage->addKnob(k); _imp->outputPref = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Velocity"));
        k->setName("outputVelocity"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel screen-space motion vector in pixels per frame "
                              "(Velocity.x/y/z). Z is reserved (0). Requires the GLSL "
                              "pipeline. Re-extracts geometry and camera at time-1."));
        aovPage->addKnob(k); _imp->outputVelocity = k;
    }
}

bool
ScanlineRender::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/, double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (!k) return false;

    // Sync to Project — copy project default format → width/height.
    KnobButtonPtr syncBtn = _imp->syncToProject.lock();
    if (syncBtn && k == syncBtn.get()) {
        AppInstancePtr app = getApp();
        if (app && app->getProject()) {
            Format fmt;
            app->getProject()->getProjectDefaultFormat(&fmt);
            const int pw = fmt.width();
            const int ph = fmt.height();
            KnobIntPtr wk = _imp->outputWidth.lock();
            KnobIntPtr hk = _imp->outputHeight.lock();
            if (pw > 0 && ph > 0 && wk && hk) {
                wk->setValue(pw);
                hk->setValue(ph);
            }
        }
        return true;
    }

    return false;
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

namespace {
// Phase 3D — forward declarations so the member function below can call
// these factories. Their definitions live further down in the same anonymous
// namespace (alongside the GLSL helpers).
static ImagePlaneDesc makeDepthPlane();
static ImagePlaneDesc makeWorldPositionPlane();
static ImagePlaneDesc makeNormalPlane();
static ImagePlaneDesc makeUvPlane();
static ImagePlaneDesc makePrefPlane();
static ImagePlaneDesc makeVelocityPlane();
} // namespace

void
ScanlineRender::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                               EffectInstance::ComponentsNeededMap* comps,
                                               double* passThroughTime,
                                               int* passThroughView,
                                               int* passThroughInput)
{
    // Beauty on the default color plane is always produced.
    (*comps)[-1].push_back(ImagePlaneDesc::getRGBAComponents());

    // Phase 3D — declare each AOV plane only when its knob is on. The render
    // path checks the same knobs to decide whether to allocate MRT attachments
    // and run the readback/blit; keeping them gated here ensures we don't
    // promise planes we won't fill.
    KnobBoolPtr depthK    = _imp->outputDepth.lock();
    KnobBoolPtr posK      = _imp->outputPosition.lock();
    KnobBoolPtr normalK   = _imp->outputNormal.lock();
    KnobBoolPtr uvK       = _imp->outputUV.lock();
    KnobBoolPtr prefK     = _imp->outputPref.lock();
    KnobBoolPtr velocityK = _imp->outputVelocity.lock();
    if (depthK    && depthK->getValue())    (*comps)[-1].push_back(makeDepthPlane());
    if (posK      && posK->getValue())      (*comps)[-1].push_back(makeWorldPositionPlane());
    if (normalK   && normalK->getValue())   (*comps)[-1].push_back(makeNormalPlane());
    if (uvK       && uvK->getValue())       (*comps)[-1].push_back(makeUvPlane());
    if (prefK     && prefK->getValue())     (*comps)[-1].push_back(makePrefPlane());
    if (velocityK && velocityK->getValue()) (*comps)[-1].push_back(makeVelocityPlane());

    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0; // bg input passes through
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

// ==================== 4x4 matrix helpers (column-major, OpenGL convention) ====================

// out = a * b   (column-major). Used by the GLSL render path to compose
// projView * localMatrix into a per-mesh MVP uniform.
static void
mat4Mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; ++c) {
        for (int rIdx = 0; rIdx < 4; ++rIdx) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + rIdx] * b[c * 4 + k];
            }
            r[c * 4 + rIdx] = s;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

// Apply a 4x4 column-major matrix to (x,y,z,w). Available for future GLSL
// CPU-side prep (Phase 3D-3 Velocity AOV will use it for the previous-frame
// clip-space derivation when the GPU path needs CPU validation).
static void
mat4Apply(const float m[16], float x, float y, float z, float w,
          float* ox, float* oy, float* oz, float* ow)
{
    *ox = m[0]*x + m[4]*y + m[8]*z  + m[12]*w;
    *oy = m[1]*x + m[5]*y + m[9]*z  + m[13]*w;
    *oz = m[2]*x + m[6]*y + m[10]*z + m[14]*w;
    *ow = m[3]*x + m[7]*y + m[11]*z + m[15]*w;
}

// ==================== GLSL / MRT helpers (Phase 3B scaffolding) ====================
//
// Inert by default — only invoked when the "Use GLSL pipeline" knob on
// ScanlineRender is enabled. Phase 3C will start using them for the beauty
// pass; Phase 3D will add MRT AOVs (Normal / UV / Pref / Velocity) via
// glslSetupMrtFbo's multi-attachment support.
//
// Once Phase 3 is complete and fixed-function is retired, these can migrate
// to a dedicated `GlslRenderHelpers.{h,cpp}` for reuse by ParticleRender,
// Project3D, and UVProject.

namespace {

// Phase 3D — AOV plane factories. Each AOV is its own ImagePlaneDesc with a
// stable plane ID + channel name layout, so downstream nodes (Shuffle, Write)
// can address them by name. Channel names are short to keep EXR multipart
// metadata tidy. The plane ID/label pair follows Natron's convention of
// matching common DCC names (Nuke, Mantra) for round-trip compatibility.
static ImagePlaneDesc makeDepthPlane()
{
    // Channel name "Z" uppercase is the OpenEXR convention for depth (Houdini
    // Mantra, V-Ray, Arnold, Renderman). Lowercase "z" is recognized by Nuke
    // but is filtered out by stricter EXR readers.
    std::vector<std::string> channels;
    channels.push_back("Z");
    return ImagePlaneDesc("depth", "depth", "Z", channels);
}

static ImagePlaneDesc makeWorldPositionPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("world_position", "world_position", "xyz", channels);
}

static ImagePlaneDesc makeNormalPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("Normal", "Normal", "xyz", channels);
}

static ImagePlaneDesc makeUvPlane()
{
    std::vector<std::string> channels;
    channels.push_back("u"); channels.push_back("v"); channels.push_back("w");
    return ImagePlaneDesc("uv", "uv", "uvw", channels);
}

static ImagePlaneDesc makePrefPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("Pref", "Pref", "xyz", channels);
}

static ImagePlaneDesc makeVelocityPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("Velocity", "Velocity", "xyz", channels);
}

// Compile a single shader stage. Logs failure to stderr. Returns 0 on failure.
static GLuint
glslCompileShader(GLenum stage, const char* source, const char* stageName)
{
    GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 1, &source, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen + 1, 0);
        glGetShaderInfoLog(sh, logLen, NULL, log.data());
        std::fprintf(stderr, "[GLSL FAIL] %s compile:\n%s\n", stageName, log.data());
        std::fflush(stderr);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Compile + link a vertex+fragment shader pair. Returns 0 on failure.
// Caller owns the returned program — must glDeleteProgram() when done.
static GLuint
glslBuildProgram(const char* vertSrc, const char* fragSrc)
{
    GLuint vs = glslCompileShader(GL_VERTEX_SHADER, vertSrc, "vertex shader");
    if (!vs) return 0;
    GLuint fs = glslCompileShader(GL_FRAGMENT_SHADER, fragSrc, "fragment shader");
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen + 1, 0);
        glGetProgramInfoLog(prog, logLen, NULL, log.data());
        std::fprintf(stderr, "[GLSL FAIL] program link:\n%s\n", log.data());
        std::fflush(stderr);
        glDeleteProgram(prog);
        prog = 0;
    }
    // Shaders can be deleted now — they're attached to the program.
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// Create a multi-attachment FBO with N RGBA32F color attachments + a depth
// renderbuffer. Out params receive the FBO handle + texture/RB handles.
// `outColorTextures` will have `numColorAttachments` entries on success.
// Returns true on success; on failure, all created resources are deleted
// and out params are zeroed. Phase 3D consumers wire this up to MRT AOV
// blits — Phase 3C only uses the standalone helpers.
[[maybe_unused]] static bool
glslSetupMrtFbo(int width, int height, int numColorAttachments,
                GLuint* outFbo,
                std::vector<GLuint>* outColorTextures,
                GLuint* outDepthRb)
{
    *outFbo = 0;
    *outDepthRb = 0;
    outColorTextures->clear();
    if (numColorAttachments <= 0 || numColorAttachments > 8) {
        std::fprintf(stderr, "[GLSL FAIL] MRT: invalid attachment count %d (max 8)\n",
                     numColorAttachments);
        std::fflush(stderr);
        return false;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    std::vector<GLuint> colorTex(numColorAttachments, 0);
    std::vector<GLenum> drawBufs(numColorAttachments);
    for (int i = 0; i < numColorAttachments; ++i) {
        glGenTextures(1, &colorTex[i]);
        glBindTexture(GL_TEXTURE_2D, colorTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0 + i,
                               GL_TEXTURE_2D, colorTex[i], 0);
        drawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }

    GLuint depthRb = 0;
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);

    glDrawBuffers(numColorAttachments, drawBufs.data());

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GLSL FAIL] MRT FBO incomplete (status=0x%x, attachments=%d)\n",
                     status, numColorAttachments);
        std::fflush(stderr);
        for (GLuint t : colorTex) if (t) glDeleteTextures(1, &t);
        glDeleteRenderbuffers(1, &depthRb);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }

    *outFbo = fbo;
    *outDepthRb = depthRb;
    *outColorTextures = colorTex;
    return true;
}

// Tear down an MRT FBO created by glslSetupMrtFbo. Safe to call with zero handles.
[[maybe_unused]] static void
glslTeardownMrtFbo(GLuint fbo, const std::vector<GLuint>& colorTextures, GLuint depthRb)
{
    if (fbo) glDeleteFramebuffers(1, &fbo);
    for (GLuint t : colorTextures) if (t) glDeleteTextures(1, &t);
    if (depthRb) glDeleteRenderbuffers(1, &depthRb);
}

} // anonymous namespace

// Invert a 4x4 column-major matrix. Returns false on singular. Used by
// the GLSL render path to compute the normal matrix from the local-to-world
// transform (inverse-transpose of the upper 3x3).
static bool
mat4Invert(float out[16], const float m[16])
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return false;
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * invDet;
    return true;
}

// ==================== Phase 3C: beauty shaders ====================
//
// GLSL passthrough shaders for the beauty plane. Supports both regular UV
// texturing AND STW projective texturing (UVProject Perspective mode) via a
// u_hasTexture uniform: 0 = no texture (white), 1 = regular UV, 2 = STW.
//
// The fragment shader has output locations for 5 attachments (beauty + 4 AOVs)
// so Phase 3D can extend without recompiling. AOV writes are gated by
// u_writeNormal / u_writeUV / u_writePref / u_writeVelocity uniforms — all
// default to 0 in Phase 3C, so only attachment 0 (beauty) gets meaningful
// data. Unused attachments either don't exist in the FBO (driver silently
// drops writes) or write zeros — both fine.

static const char* kBeautyVert =
    "#version 330 core\n"
    "layout(location = 0) in vec3 in_pos;\n"
    "layout(location = 1) in vec2 in_uv;\n"
    "layout(location = 2) in vec4 in_stw;       // s, t, 0, w (for projective texturing)\n"
    "layout(location = 3) in vec3 in_normal;    // object-space normal (Phase 3D)\n"
    "layout(location = 4) in vec3 in_prevPos;   // previous-frame object-space position (Phase 3D-3 — velocity)\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prevMvp;                   // previous-frame MVP for velocity computation\n"
    "uniform mat3 u_normalMatrix;              // transpose(inverse(localMatrix3x3)) for world normals\n"
    "uniform vec2 u_viewportSize;              // (width, height) in pixels — scales NDC delta to screen pixels\n"
    "out vec2 v_uv;\n"
    "out vec4 v_stw;\n"
    "out vec3 v_worldNormal;\n"
    "out vec3 v_prefPos;                       // object-space position (= in_pos) — Pref AOV\n"
    "out vec4 v_currClipPos;                   // current-frame clip-space position — for velocity\n"
    "out vec4 v_prevClipPos;                   // previous-frame clip-space position — for velocity\n"
    "void main() {\n"
    "    v_uv          = in_uv;\n"
    "    v_stw         = in_stw;\n"
    "    v_worldNormal = u_normalMatrix * in_normal;\n"
    "    v_prefPos     = in_pos;\n"
    "    v_currClipPos = u_mvp * vec4(in_pos, 1.0);\n"
    "    v_prevClipPos = u_prevMvp * vec4(in_prevPos, 1.0);\n"
    "    gl_Position   = v_currClipPos;\n"
    "}\n";

static const char* kBeautyFrag =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_stw;\n"
    "in vec3 v_worldNormal;\n"
    "in vec3 v_prefPos;\n"
    "in vec4 v_currClipPos;\n"
    "in vec4 v_prevClipPos;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_viewportSize;\n"
    "uniform int u_hasTexture;       // 0=no, 1=regular UV, 2=STW projective\n"
    "uniform int u_writeNormal;      // attachment 1 (Phase 3D)\n"
    "uniform int u_writeUV;          // attachment 2 (Phase 3D)\n"
    "uniform int u_writePref;        // attachment 3 (Phase 3D)\n"
    "uniform int u_writeVelocity;    // attachment 4 (Phase 3D)\n"
    "layout(location = 0) out vec4 out_color;\n"
    "layout(location = 1) out vec4 out_normal;\n"
    "layout(location = 2) out vec4 out_uv;\n"
    "layout(location = 3) out vec4 out_pref;\n"
    "layout(location = 4) out vec4 out_velocity;\n"
    "void main() {\n"
    "    // --- Beauty (attachment 0) ---\n"
    "    if (u_hasTexture == 2) {\n"
    "        if (v_stw.w <= 0.0) discard;\n"
    "        vec2 uv = v_stw.xy / v_stw.w;\n"
    "        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
    "            out_color = vec4(0.0);\n"
    "        } else {\n"
    "            out_color = texture(u_tex, uv);\n"
    "        }\n"
    "    } else if (u_hasTexture == 1) {\n"
    "        out_color = texture(u_tex, v_uv);\n"
    "    } else {\n"
    "        out_color = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "    }\n"
    "    // --- Normal AOV ---\n"
    "    if (u_writeNormal == 1) {\n"
    "        vec3 n = normalize(v_worldNormal);\n"
    "        out_normal = vec4(n, 1.0);\n"
    "    } else { out_normal = vec4(0.0); }\n"
    "    // --- UV AOV ---\n"
    "    if (u_writeUV == 1) {\n"
    "        out_uv = vec4(v_uv, 0.0, 1.0);\n"
    "    } else { out_uv = vec4(0.0); }\n"
    "    // --- Pref AOV (object-space position, before localMatrix) ---\n"
    "    if (u_writePref == 1) {\n"
    "        out_pref = vec4(v_prefPos, 1.0);\n"
    "    } else { out_pref = vec4(0.0); }\n"
    "    // --- Velocity AOV (screen-pixels delta between previous + current frame) ---\n"
    "    if (u_writeVelocity == 1) {\n"
    "        vec2 currNdc = v_currClipPos.xy / v_currClipPos.w;\n"
    "        vec2 prevNdc = v_prevClipPos.xy / v_prevClipPos.w;\n"
    "        vec2 deltaNdc = currNdc - prevNdc;\n"
    "        vec2 deltaPx = deltaNdc * (u_viewportSize * 0.5);\n"
    "        out_velocity = vec4(deltaPx, 0.0, 1.0);\n"
    "    } else { out_velocity = vec4(0.0); }\n"
    "}\n";

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
    std::vector<float> uvs;      // u,v interleaved (used when stw is empty)
    std::vector<float> stw;      // s,t,w interleaved — populated by UVProject (Perspective +
                                 //   Generate Perspective). When non-empty, takes precedence
                                 //   over uvs and the render loop emits glTexCoord4f(s,t,0,w)
                                 //   so OpenGL does perspective-correct fragment-level divide.
    std::vector<int> triIndices;
    float localMatrix[16];
    ImagePtr texImg;

    // Phase 3 GLSL/MRT extensions. Empty / identity for fixed-function path.
    // Phase 3D populates these for the Normal / Pref / Velocity AOVs:
    //   normals          — object-space per-vertex normals (xyz interleaved)
    //   prevVerts        — previous-frame object-space positions (xyz interleaved)
    //   prevLocalMatrix  — previous-frame local→world transform (col-major)
    std::vector<float> normals;
    std::vector<float> prevVerts;
    float prevLocalMatrix[16];

    GeoData()
    {
        // Identity for prevLocalMatrix so velocity = 0 by default (static).
        for (int i = 0; i < 16; ++i) prevLocalMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
};

// Fan-triangulate a polygon-soup mesh (faceIndices + per-face vertex counts)
// into a flat triangle index array. Required: most DCC exports (Maya / Houdini /
// Blender Alembic) store quads or n-gons, NOT triangles. Treating faceIndices
// as raw triangle indices crosses face boundaries and produces garbage.
static void
fanTriangulate(const std::vector<int>& faceIndices,
               const std::vector<int>& faceCounts,
               std::vector<int>& outTris)
{
    outTris.clear();
    if (faceCounts.empty()) {
        // No face-count info — assume input is already triangulated.
        outTris = faceIndices;
        return;
    }
    // Upper bound: every face contributes (count-2) triangles → (count-2)*3 ints.
    // For an all-quad mesh that's (4-2)*3 = 6 per face. Reserve generously.
    outTris.reserve(faceIndices.size() * 2);

    size_t offset = 0;
    for (size_t f = 0; f < faceCounts.size(); ++f) {
        const int c = faceCounts[f];
        if (c < 3 || offset + (size_t)c > faceIndices.size()) {
            offset += (size_t)std::max(0, c);
            continue;
        }
        const int v0 = faceIndices[offset];
        for (int i = 1; i + 1 < c; ++i) {
            outTris.push_back(v0);
            outTris.push_back(faceIndices[offset + i]);
            outTris.push_back(faceIndices[offset + i + 1]);
        }
        offset += (size_t)c;
    }
}

static bool
extractGeometry(EffectInstancePtr effect, double time, ViewIdx view, GeoData& out)
{
    out.verts.clear();
    out.uvs.clear();
    out.triIndices.clear();
    SceneGraph::buildTRS(0,0,0, 0,0,0, 1,1,1, out.localMatrix);
    out.texImg.reset();

    if (!effect) return false;
    // Honor "D" disable: a disabled node contributes nothing.
    {
        NodePtr n = effect->getNode();
        if (n && n->isNodeDisabled()) return false;
    }

    Sphere3D* sphere = dynamic_cast<Sphere3D*>(effect.get());
    if (sphere) {
        std::vector<Sphere3D::SphereVertex> sv;
        sphere->generateSphereMesh(time, sv, out.triIndices);
        out.verts.resize(sv.size() * 3);
        out.uvs.resize(sv.size() * 2);
        out.normals.resize(sv.size() * 3);
        for (size_t i = 0; i < sv.size(); ++i) {
            out.verts[i*3+0] = sv[i].x; out.verts[i*3+1] = sv[i].y; out.verts[i*3+2] = sv[i].z;
            out.uvs[i*2+0] = sv[i].u; out.uvs[i*2+1] = sv[i].v;
            out.normals[i*3+0] = sv[i].nx; out.normals[i*3+1] = sv[i].ny; out.normals[i*3+2] = sv[i].nz;
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
        out.normals.resize(cv.size() * 3);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
            out.normals[i*3+0] = cv[i].nx; out.normals[i*3+1] = cv[i].ny; out.normals[i*3+2] = cv[i].nz;
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
        out.normals.resize(cv.size() * 3);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
            out.normals[i*3+0] = cv[i].nx; out.normals[i*3+1] = cv[i].ny; out.normals[i*3+2] = cv[i].nz;
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
        out.normals.resize(cv.size() * 3);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
            out.normals[i*3+0] = cv[i].nx; out.normals[i*3+1] = cv[i].ny; out.normals[i*3+2] = cv[i].nz;
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
        fanTriangulate(mesh->faceIndices, mesh->faceCounts, out.triIndices);
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
    // Honor "D" disable: skip the whole branch under a disabled node.
    {
        NodePtr n = effect->getNode();
        if (n && n->isNodeDisabled()) return;
    }

    // UVProject: transparent UV-rewriting wrapper. Walk through to the upstream
    // geo, extract it normally, then apply the rewrite to every GeoData produced.
    UVProject* uvProj = dynamic_cast<UVProject*>(effect.get());
    if (uvProj) {
        EffectInstancePtr upstream = uvProj->getGeoInput();
        if (!upstream) return;
        const size_t prevCount = out.size();
        extractGeometries(upstream, time, view, out);

        // Optional projection-image override via UVProject's input 2 (img).
        ImagePtr projImg;
        if (uvProj->getImgInput()) {
            RectI roi;
            projImg = uvProj->getImage(2, time, RenderScale(), view,
                                       NULL, NULL, false, true,
                                       eStorageModeRAM, 0, &roi);
        }

        for (size_t i = prevCount; i < out.size(); ++i) {
            GeoData& g = out[i];
            std::vector<float> newUVs;
            std::vector<float> newSTW;
            int newComp = 0;
            uvProj->rewriteUVs(g.verts, g.localMatrix, time, newUVs, newSTW, newComp);
            if (newComp == 3) {
                g.stw = std::move(newSTW);
                g.uvs.clear(); // stw takes precedence
            } else if (newComp == 2) {
                g.uvs = std::move(newUVs);
                g.stw.clear();
            }
            // newComp == 0: Mode == Off — keep g.uvs as extracted upstream.

            if (projImg) {
                g.texImg = projImg; // override upstream texture
            }
        }
        return;
    }

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
            fanTriangulate(mesh->faceIndices, mesh->faceCounts, g.triIndices);
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

// Phase 3C — GLSL beauty render path. Parallel to renderGeoObject (below)
// but uses modern GL 3.3 core: VAO + interleaved VBO + IBO + uniform-driven
// MVP, sampled via the kBeautyVert/kBeautyFrag shader pair. Caller must have
// glUseProgram'd `program` before invoking. `prevProjViewMatrix` is the
// camera (proj * view) at the previous frame — used for Velocity AOV in
// Phase 3D; pass current projViewMatrix when velocity isn't needed.
// `writeNormal/UV/Pref/Velocity` request AOV outputs at MRT attachments 1-4
// (Phase 3D only — caller must have bound an MRT FBO with those slots).
static void
renderGeoObjectGlsl(const GeoData& geo, GLuint program,
                    const float projViewMatrix[16],
                    const float prevProjViewMatrix[16],
                    int viewportW, int viewportH,
                    bool writeNormal, bool writeUV, bool writePref, bool writeVelocity)
{
    const int numVerts = (int)(geo.verts.size() / 3);
    const int numTris  = (int)(geo.triIndices.size() / 3);
    if (numVerts == 0 || numTris == 0) return;

    // --- Upload texture if available (mirrors fixed-function path) ---
    GLuint srcTex = 0;
    int hasTextureMode = 0;  // 0=no, 1=UV, 2=STW
    if (geo.texImg) {
        RectI texBounds = geo.texImg->getBounds();
        const int texW = texBounds.width();
        const int texH = texBounds.height();
        if (texW > 0 && texH > 0) {
            glGenTextures(1, &srcTex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            std::vector<float> texData((size_t)texW * texH * 4, 0.0f);
            {
                Image::ReadAccess ra(geo.texImg.get());
                for (int y = texBounds.y1; y < texBounds.y2; ++y) {
                    for (int x = texBounds.x1; x < texBounds.x2; ++x) {
                        const float* pix = (const float*)ra.pixelAt(x, y);
                        if (!pix) continue;
                        int idx = ((y - texBounds.y1) * texW + (x - texBounds.x1)) * 4;
                        texData[idx + 0] = pix[0];
                        texData[idx + 1] = pix[1];
                        texData[idx + 2] = pix[2];
                        texData[idx + 3] = (geo.texImg->getComponents().getNumComponents() >= 4) ? pix[3] : 1.0f;
                    }
                }
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, texW, texH, 0, GL_RGBA, GL_FLOAT, texData.data());
            const bool useSTW = !geo.stw.empty() && (int)geo.stw.size() >= numVerts * 3;
            hasTextureMode = useSTW ? 2 : 1;
        }
    }

    // --- Per-mesh MVP = projView * localMatrix (current + previous) ---
    float mvp[16];
    mat4Mul(mvp, projViewMatrix, geo.localMatrix);
    float prevMvp[16];
    mat4Mul(prevMvp, prevProjViewMatrix, geo.prevLocalMatrix);

    // --- Normal matrix = transpose(inverse(localMatrix3x3)). For non-uniform
    // scale, normals need inverse-transpose to stay perpendicular to the
    // surface in world space. Identity fallback when localMatrix is singular.
    float normalMat[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
    {
        float m3[16] = {
            geo.localMatrix[0], geo.localMatrix[1], geo.localMatrix[2],  0,
            geo.localMatrix[4], geo.localMatrix[5], geo.localMatrix[6],  0,
            geo.localMatrix[8], geo.localMatrix[9], geo.localMatrix[10], 0,
            0, 0, 0, 1
        };
        float inv[16];
        if (mat4Invert(inv, m3)) {
            normalMat[0] = inv[0];  normalMat[1] = inv[4];  normalMat[2] = inv[8];
            normalMat[3] = inv[1];  normalMat[4] = inv[5];  normalMat[5] = inv[9];
            normalMat[6] = inv[2];  normalMat[7] = inv[6];  normalMat[8] = inv[10];
        }
    }

    // --- Interleaved VBO: pos(3) + uv(2) + stw(4) + normal(3) + prevPos(3) = 15 floats/vertex
    const int stride = 3 + 2 + 4 + 3 + 3;
    std::vector<float> interleaved((size_t)numVerts * stride, 0.0f);
    const bool hasPrevVerts = ((int)geo.prevVerts.size() >= numVerts * 3);
    for (int v = 0; v < numVerts; ++v) {
        float* row = &interleaved[(size_t)v * stride];
        row[0] = geo.verts[v * 3 + 0];
        row[1] = geo.verts[v * 3 + 1];
        row[2] = geo.verts[v * 3 + 2];
        if ((int)geo.uvs.size() > v * 2 + 1) {
            row[3] = geo.uvs[v * 2 + 0];
            row[4] = geo.uvs[v * 2 + 1];
        }
        if ((int)geo.stw.size() > v * 3 + 2) {
            row[5] = geo.stw[v * 3 + 0];
            row[6] = geo.stw[v * 3 + 1];
            row[7] = 0.0f;
            row[8] = geo.stw[v * 3 + 2];
        }
        if ((int)geo.normals.size() > v * 3 + 2) {
            row[9]  = geo.normals[v * 3 + 0];
            row[10] = geo.normals[v * 3 + 1];
            row[11] = geo.normals[v * 3 + 2];
        }
        if (hasPrevVerts) {
            row[12] = geo.prevVerts[v * 3 + 0];
            row[13] = geo.prevVerts[v * 3 + 1];
            row[14] = geo.prevVerts[v * 3 + 2];
        } else {
            row[12] = row[0]; row[13] = row[1]; row[14] = row[2];
        }
    }

    GLuint vao = 0, vbo = 0, ibo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(9 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(12 * sizeof(float)));

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(geo.triIndices.size() * sizeof(int)),
                 geo.triIndices.data(), GL_STREAM_DRAW);

    // --- Set uniforms + draw ---
    glUseProgram(program);
    GLint locMvp           = glGetUniformLocation(program, "u_mvp");
    GLint locPrevMvp       = glGetUniformLocation(program, "u_prevMvp");
    GLint locNormalMat     = glGetUniformLocation(program, "u_normalMatrix");
    GLint locViewportSize  = glGetUniformLocation(program, "u_viewportSize");
    GLint locTex           = glGetUniformLocation(program, "u_tex");
    GLint locHasTexture    = glGetUniformLocation(program, "u_hasTexture");
    GLint locWriteNormal   = glGetUniformLocation(program, "u_writeNormal");
    GLint locWriteUV       = glGetUniformLocation(program, "u_writeUV");
    GLint locWritePref     = glGetUniformLocation(program, "u_writePref");
    GLint locWriteVelocity = glGetUniformLocation(program, "u_writeVelocity");
    if (locMvp >= 0)           glUniformMatrix4fv(locMvp,        1, GL_FALSE, mvp);
    if (locPrevMvp >= 0)       glUniformMatrix4fv(locPrevMvp,    1, GL_FALSE, prevMvp);
    if (locNormalMat >= 0)     glUniformMatrix3fv(locNormalMat,  1, GL_FALSE, normalMat);
    if (locViewportSize >= 0)  glUniform2f(locViewportSize, (float)viewportW, (float)viewportH);
    if (locTex >= 0)           glUniform1i(locTex, 0);
    if (locHasTexture >= 0)    glUniform1i(locHasTexture, hasTextureMode);
    if (locWriteNormal >= 0)   glUniform1i(locWriteNormal,   writeNormal   ? 1 : 0);
    if (locWriteUV >= 0)       glUniform1i(locWriteUV,       writeUV       ? 1 : 0);
    if (locWritePref >= 0)     glUniform1i(locWritePref,     writePref     ? 1 : 0);
    if (locWriteVelocity >= 0) glUniform1i(locWriteVelocity, writeVelocity ? 1 : 0);

    glDrawElements(GL_TRIANGLES, numTris * 3, GL_UNSIGNED_INT, 0);

    // --- Cleanup ---
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    glDeleteVertexArrays(1, &vao);
    if (srcTex) glDeleteTextures(1, &srcTex);
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

    // STW path = UVProject in Perspective + Generate Perspective mode. When
    // active, we emit (s, t, 0, w) so GL does the perspective divide at the
    // fragment. Clamp-to-border with transparent border so out-of-frustum
    // samples come out as zero (matches a Crop-style out-of-frame behavior).
    const bool useSTW = hasTexture && !geo.stw.empty()
                        && (int)geo.stw.size() >= numVerts * 3;
    if (useSTW) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float borderColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    }

    glPushMatrix();
    glMultMatrixf(geo.localMatrix);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_TRIANGLES);
    for (int t = 0; t < numTris; ++t) {
        for (int vi = 0; vi < 3; ++vi) {
            int idx = geo.triIndices[t * 3 + vi];
            if (idx < 0 || idx >= numVerts) continue;
            if (useSTW) {
                glTexCoord4f(geo.stw[idx * 3 + 0],
                             geo.stw[idx * 3 + 1],
                             0.0f,
                             geo.stw[idx * 3 + 2]);
            } else if (hasTexture && (int)geo.uvs.size() > idx * 2 + 1) {
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
    // Honor the "D" disable knob: a disabled scene/geo input renders nothing.
    if (geoEffect->getNode() && geoEffect->getNode()->isNodeDisabled()) {
        return eStatusFailed;
    }

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
                // Honor "D" disable: skip disabled scene inputs entirely.
                if (sceneInput->getNode() && sceneInput->getNode()->isNodeDisabled()) continue;

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

    // --- Phase 3D: optional MRT attachments (Normal=1, UV=2, Pref=3, Velocity=4) ---
    // Each AOV's attachment is allocated only when its knob is on AND GLSL is on.
    // Attachments don't have to be contiguous — glDrawBuffers can carry GL_NONE for
    // skipped slots, e.g. [COLOR0, GL_NONE, COLOR2].
    const bool glslOn = _imp->useGlslPipeline.lock() && _imp->useGlslPipeline.lock()->getValue();
    const bool wantsNormalMrtPre   = glslOn && _imp->outputNormal.lock()   && _imp->outputNormal.lock()->getValue();
    const bool wantsUvMrtPre       = glslOn && _imp->outputUV.lock()       && _imp->outputUV.lock()->getValue();
    const bool wantsPrefMrtPre     = glslOn && _imp->outputPref.lock()     && _imp->outputPref.lock()->getValue();
    const bool wantsVelocityMrtPre = glslOn && _imp->outputVelocity.lock() && _imp->outputVelocity.lock()->getValue();
    GLuint msNormalRB = 0,   normalResolveTex   = 0;
    GLuint msUvRB = 0,       uvResolveTex       = 0;
    GLuint msPrefRB = 0,     prefResolveTex     = 0;
    GLuint msVelocityRB = 0, velocityResolveTex = 0;

    auto allocMrtAttachment = [&](int attachmentIndex, GLuint& msRB, GLuint& resolveTex,
                                  const char* aovName) {
        glBindFramebuffer(GL_FRAMEBUFFER, msFBO);
        glGenRenderbuffers(1, &msRB);
        glBindRenderbuffer(GL_RENDERBUFFER, msRB);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA32F_ARB, outW, outH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachmentIndex,
                                  GL_RENDERBUFFER, msRB);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &resolveTex);
        glBindTexture(GL_TEXTURE_2D, resolveTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, outW, outH, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachmentIndex,
                               GL_TEXTURE_2D, resolveTex, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, msFBO);
        bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (ok) ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        if (!ok) {
            std::fprintf(stderr, "[GLSL FAIL] %s AOV: MRT FBO incomplete after adding attachment %d.\n",
                         aovName, attachmentIndex);
            std::fflush(stderr);
            glDeleteRenderbuffers(1, &msRB);    msRB = 0;
            glDeleteTextures(1, &resolveTex);   resolveTex = 0;
        }
    };

    if (wantsNormalMrtPre)   allocMrtAttachment(1, msNormalRB,   normalResolveTex,   "Normal");
    if (wantsUvMrtPre)       allocMrtAttachment(2, msUvRB,       uvResolveTex,       "UV");
    if (wantsPrefMrtPre)     allocMrtAttachment(3, msPrefRB,     prefResolveTex,     "Pref");
    if (wantsVelocityMrtPre) allocMrtAttachment(4, msVelocityRB, velocityResolveTex, "Velocity");

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
    // Phase 3D — parallel accumulators for the GLSL-MRT AOVs. Averaging across
    // motion-blur samples is meaningful for normals, UVs, Pref, and velocity.
    std::vector<float> normalAccumPixels;
    std::vector<float> uvAccumPixels;
    std::vector<float> prefAccumPixels;
    std::vector<float> velocityAccumPixels;

    // Phase 3C — build the GLSL beauty program once if the GLSL pipeline knob
    // is ON. Done outside the multi-sample loop so we don't rebuild per sample.
    const bool useGlslBeauty = _imp->useGlslPipeline.lock() &&
                                _imp->useGlslPipeline.lock()->getValue();
    GLuint glslBeautyProg = 0;
    if (useGlslBeauty) {
        glslBeautyProg = glslBuildProgram(kBeautyVert, kBeautyFrag);
        // If the program fails to build, drop back to the fixed-function path
        // for this render (glslBeautyProg == 0 → the per-geo branch below uses
        // renderGeoObject instead of renderGeoObjectGlsl).
    }

    // Precompute projView = projMatrix * viewMatrix for the GLSL path.
    float projViewMatrix[16];
    mat4Mul(projViewMatrix, projMatrix, viewMatrix);

    // Phase 3D — latch the final wants*Mrt flags now that we know whether the
    // beauty program built and whether the MRT attachment was allocated. Any
    // failure latches `false` and we just skip that AOV silently.
    const bool wantsNormalMrt   = wantsNormalMrtPre   && (glslBeautyProg != 0) && (msNormalRB   != 0);
    const bool wantsUvMrt       = wantsUvMrtPre       && (glslBeautyProg != 0) && (msUvRB       != 0);
    const bool wantsPrefMrt     = wantsPrefMrtPre     && (glslBeautyProg != 0) && (msPrefRB     != 0);
    const bool wantsVelocityMrt = wantsVelocityMrtPre && (glslBeautyProg != 0) && (msVelocityRB != 0);
    const bool wantsAnyMrt      = wantsNormalMrt || wantsUvMrt || wantsPrefMrt || wantsVelocityMrt;

    // Phase 3D — previous-frame camera matrices for the Velocity AOV. Use
    // time - 1.0 as the reference. Output is pixels per frame.
    float prevViewMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float prevProjMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float prevProjViewMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (wantsVelocityMrt) {
        const double prevTime = args.time - 1.0;
        double pTx = (double)camTX, pTy = (double)camTY, pTz = (double)camTZ;
        double pRx = (double)camRX, pRy = (double)camRY, pRz = (double)camRZ;
        double pFL = (double)camFL, pHA = (double)camHA, pVA = (double)camVA;
        double pNear = (double)camNear, pFar = (double)camFar;
        if (cam) {
            cam->getCameraPosition(prevTime, pTx, pTy, pTz, pRx, pRy, pRz);
            pFL   = cam->getCameraFocalLength(prevTime);
            pHA   = cam->getCameraHAperture(prevTime);
            pVA   = cam->getCameraVAperture(prevTime);
            pNear = cam->getCameraNear(prevTime);
            pFar  = cam->getCameraFar(prevTime);
        }
        buildViewMatrix((float)pTx, (float)pTy, (float)pTz,
                        (float)pRx, (float)pRy, (float)pRz, prevViewMatrix);
        CameraMath::composeProjectionMatrix((float)pFL, (float)pHA, (float)pVA,
                                            (float)pNear, (float)pFar, prevProjMatrix);
        mat4Mul(prevProjViewMatrix, prevProjMatrix, prevViewMatrix);
    }

    // Phase 3D — extract previous-frame geometry. For animated meshes the
    // verts differ across frames; for static meshes only the local matrix
    // moves. renderGeoObjectGlsl auto-detects via `hasPrevVerts`.
    if (wantsVelocityMrt && geoEffect) {
        std::vector<GeoData> prevGeoObjects;
        const double prevTime = args.time - 1.0;
        Scene3D* scenePrev = dynamic_cast<Scene3D*>(geoEffect.get());
        if (scenePrev) {
            for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
                EffectInstancePtr sceneInputPrev = scenePrev->getInput(i);
                if (!sceneInputPrev) continue;
                if (dynamic_cast<Volume3D*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<ReadVDB*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<Light3D*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<ParticleInstance*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<ParticleProvider*>(sceneInputPrev.get())) continue;
                extractGeometries(sceneInputPrev, prevTime, args.view, prevGeoObjects);
            }
        } else {
            extractGeometries(geoEffect, prevTime, args.view, prevGeoObjects);
        }
        const size_t n = std::min(geoObjects.size(), prevGeoObjects.size());
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(geoObjects[i].prevLocalMatrix, prevGeoObjects[i].localMatrix, sizeof(float) * 16);
            if (prevGeoObjects[i].verts.size() == geoObjects[i].verts.size() &&
                prevGeoObjects[i].verts != geoObjects[i].verts) {
                geoObjects[i].prevVerts = std::move(prevGeoObjects[i].verts);
            }
        }
    }

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
        // Phase 3D — glDrawBuffers maps fragment-shader layout(location=N) to
        // physical color attachments. GL_NONE lets us skip un-allocated slots
        // so e.g. "UV only, no Normal" still works without re-compiling the
        // shader.
        if (wantsAnyMrt) {
            GLenum drawBufs[5] = {
                (GLenum)GL_COLOR_ATTACHMENT0,
                (GLenum)(wantsNormalMrt   ? GL_COLOR_ATTACHMENT1 : GL_NONE),
                (GLenum)(wantsUvMrt       ? GL_COLOR_ATTACHMENT2 : GL_NONE),
                (GLenum)(wantsPrefMrt     ? GL_COLOR_ATTACHMENT3 : GL_NONE),
                (GLenum)(wantsVelocityMrt ? GL_COLOR_ATTACHMENT4 : GL_NONE),
            };
            glDrawBuffers(5, drawBufs);
        } else {
            GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBufs);
        }
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
        if (useGlslBeauty && glslBeautyProg) {
            renderGeoObjectGlsl(geoObjects[gi], glslBeautyProg,
                                projViewMatrix, prevProjViewMatrix,
                                outW, outH,
                                wantsNormalMrt, wantsUvMrt,
                                wantsPrefMrt, wantsVelocityMrt);
        } else {
            renderGeoObject(geoObjects[gi]);
        }
    }
    // Return to fixed-function pipeline for particles / lights / etc. Also
    // restore single-attachment draw buffer so particles only paint into
    // beauty (their fixed-function path doesn't know about MRT slots).
    if (useGlslBeauty && glslBeautyProg) {
        glUseProgram(0);
        if (wantsAnyMrt) {
            GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBufs);
        }
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
        // Default read/draw buffer is attachment 0 (beauty). Blit it first.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, outW, outH,
                          0, 0, outW, outH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Phase 3D — blit MRT attachments as separate passes (glBlitFramebuffer
        // only operates on the currently-bound read/draw buffer pair).
        if (wantsNormalMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glDrawBuffer(GL_COLOR_ATTACHMENT1);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (wantsUvMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glDrawBuffer(GL_COLOR_ATTACHMENT2);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (wantsPrefMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT3);
            glDrawBuffer(GL_COLOR_ATTACHMENT3);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (wantsVelocityMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT4);
            glDrawBuffer(GL_COLOR_ATTACHMENT4);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        // Bind resolve FBO for readback
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        // --- Read back this sample's pixels ---
        std::vector<float> samplePixels(outW * outH * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, samplePixels.data());

        // Accumulate into the accumulator
        if (accumPixels.empty()) {
            accumPixels = samplePixels;
        } else {
            for (size_t i = 0; i < accumPixels.size(); ++i) {
                accumPixels[i] += samplePixels[i];
            }
        }

        // Phase 3D — read + accumulate per-AOV pixels.
        if (wantsNormalMrt) {
            std::vector<float> normalSamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, normalSamplePixels.data());
            if (normalAccumPixels.empty()) normalAccumPixels = normalSamplePixels;
            else for (size_t i = 0; i < normalAccumPixels.size(); ++i)
                normalAccumPixels[i] += normalSamplePixels[i];
        }
        if (wantsUvMrt) {
            std::vector<float> uvSamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, uvSamplePixels.data());
            if (uvAccumPixels.empty()) uvAccumPixels = uvSamplePixels;
            else for (size_t i = 0; i < uvAccumPixels.size(); ++i)
                uvAccumPixels[i] += uvSamplePixels[i];
        }
        if (wantsPrefMrt) {
            std::vector<float> prefSamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT3);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, prefSamplePixels.data());
            if (prefAccumPixels.empty()) prefAccumPixels = prefSamplePixels;
            else for (size_t i = 0; i < prefAccumPixels.size(); ++i)
                prefAccumPixels[i] += prefSamplePixels[i];
        }
        if (wantsVelocityMrt) {
            std::vector<float> velocitySamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT4);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, velocitySamplePixels.data());
            if (velocityAccumPixels.empty()) velocityAccumPixels = velocitySamplePixels;
            else for (size_t i = 0; i < velocityAccumPixels.size(); ++i)
                velocityAccumPixels[i] += velocitySamplePixels[i];
        }
    } // === END MULTI-SAMPLE RENDER LOOP ===

    // Phase 3C — release the GLSL beauty program.
    if (glslBeautyProg) {
        glDeleteProgram(glslBeautyProg);
        glslBeautyProg = 0;
    }

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

    // Phase 3D — average each AOV accumulator the same way.
    auto avgAovAccum = [&](std::vector<float>& accum) -> std::vector<float> {
        std::vector<float> out = std::move(accum);
        if (motionSamples > 1 && !out.empty()) {
            float invN = 1.0f / (float)motionSamples;
            for (size_t i = 0; i < out.size(); ++i) out[i] *= invN;
        }
        return out;
    };
    std::vector<float> normalPixels   = wantsNormalMrt   ? avgAovAccum(normalAccumPixels)   : std::vector<float>();
    std::vector<float> uvPixels       = wantsUvMrt       ? avgAovAccum(uvAccumPixels)       : std::vector<float>();
    std::vector<float> prefPixels     = wantsPrefMrt     ? avgAovAccum(prefAccumPixels)     : std::vector<float>();
    std::vector<float> velocityPixels = wantsVelocityMrt ? avgAovAccum(velocityAccumPixels) : std::vector<float>();

    // --- Composite with background if connected ---
    ImagePtr bgImg;
    EffectInstancePtr bgEffect = getInput(0);
    if (bgEffect) {
        RectI bgRoi;
        bgImg = getImage(0, args.time, RenderScale(), args.view,
                         NULL, NULL, false, true,
                         eStorageModeRAM, 0, &bgRoi);
    }

    // --- Depth + World Position readback ---
    // Multi-sampled depth doesn't compose meaningfully (motion-blurred depth is
    // unphysical), so we just take depth from the final sample's resolve.
    // Acceptable for DOF / fog / re-projection use cases.
    const bool emitDepth = _imp->outputDepth.lock()    && _imp->outputDepth.lock()->getValue();
    const bool emitPos   = _imp->outputPosition.lock() && _imp->outputPosition.lock()->getValue();
    const bool emitAovs  = emitDepth || emitPos;

    std::vector<float> depthBuf;
    if (emitAovs) {
        // Blit MSAA depth → single-sample resolve FBO so we can glReadPixels it.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, outW, outH,
                          0, 0, outW, outH,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        depthBuf.resize((size_t)outW * (size_t)outH);
        glReadPixels(0, 0, outW, outH, GL_DEPTH_COMPONENT, GL_FLOAT, depthBuf.data());
    }

    // Inverse(projection * view) for world-position reconstruction.
    float invMVP[16] = {0};
    bool haveInvMVP = false;
    if (emitPos) {
        float mvp[16];
        mat4Mul(mvp, projMatrix, viewMatrix);
        haveInvMVP = mat4Invert(invMVP, mvp);
    }

    // --- Per-plane output: iterate args.outputPlanes (Color + optional AOVs) ---
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator pit = args.outputPlanes.begin();
         pit != args.outputPlanes.end(); ++pit) {
        const ImagePlaneDesc& planeDesc = pit->first;
        ImagePtr outPlaneImg = pit->second;
        if (!outPlaneImg) continue;
        const RectI outBounds = outPlaneImg->getBounds();
        const std::string& planeID = planeDesc.getPlaneID();

        if (planeDesc.isColorPlane()) {
            // Beauty plane — composite foreground over background image.
            Image::WriteAccess wa(outPlaneImg.get());
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
        } else if (planeID == "depth" && emitDepth && !depthBuf.empty()) {
            // Linear camera-space distance (world units). Background pixels
            // (depth==1.0) emit camFar so downstream nodes have a clean
            // "infinity" value.
            Image::WriteAccess wa(outPlaneImg.get());
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        dst[0] = camFar;
                        continue;
                    }
                    float d = depthBuf[(size_t)fbY * outW + fbX];
                    if (d >= 1.0f - 1e-6f) {
                        dst[0] = camFar;
                    } else {
                        float zNdc = 2.0f * d - 1.0f;
                        dst[0] = (2.0f * camNear * camFar) /
                                 (camFar + camNear - zNdc * (camFar - camNear));
                    }
                }
            }
        } else if (planeID == "world_position" && emitPos && haveInvMVP && !depthBuf.empty()) {
            // Reconstruct world-space surface position via inverse(proj*view)
            // on NDC samples. Background pixels (depth==1.0) emit (0,0,0) since
            // no surface was hit.
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    float d = depthBuf[(size_t)fbY * outW + fbX];
                    if (d >= 1.0f - 1e-6f) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    float ndcX = 2.0f * ((float)fbX + 0.5f) / (float)outW - 1.0f;
                    float ndcY = 2.0f * ((float)fbY + 0.5f) / (float)outH - 1.0f;
                    float ndcZ = 2.0f * d - 1.0f;
                    float wX, wY, wZ, wW;
                    mat4Apply(invMVP, ndcX, ndcY, ndcZ, 1.0f, &wX, &wY, &wZ, &wW);
                    if (wW != 0.0f) {
                        float inv = 1.0f / wW;
                        if (outNumComp > 0) dst[0] = wX * inv;
                        if (outNumComp > 1) dst[1] = wY * inv;
                        if (outNumComp > 2) dst[2] = wZ * inv;
                    } else {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                    }
                }
            }
        } else if (planeID == "Normal" && wantsNormalMrt && !normalPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = normalPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = normalPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = normalPixels[idx + 2];
                }
            }
        } else if (planeID == "uv" && wantsUvMrt && !uvPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = uvPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = uvPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = 0.0f;
                }
            }
        } else if (planeID == "Pref" && wantsPrefMrt && !prefPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = prefPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = prefPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = prefPixels[idx + 2];
                }
            }
        } else if (planeID == "Velocity" && wantsVelocityMrt && !velocityPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = velocityPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = velocityPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = 0.0f;
                }
            }
        }
    } // for each output plane

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteRenderbuffers(1, &depthRB);
    glDeleteFramebuffers(1, &msFBO);
    glDeleteRenderbuffers(1, &msColorRB);
    glDeleteRenderbuffers(1, &msDepthRB);
    // Phase 3D — release optional MRT resources.
    if (msNormalRB)         glDeleteRenderbuffers(1, &msNormalRB);
    if (normalResolveTex)   glDeleteTextures(1, &normalResolveTex);
    if (msUvRB)             glDeleteRenderbuffers(1, &msUvRB);
    if (uvResolveTex)       glDeleteTextures(1, &uvResolveTex);
    if (msPrefRB)           glDeleteRenderbuffers(1, &msPrefRB);
    if (prefResolveTex)     glDeleteTextures(1, &prefResolveTex);
    if (msVelocityRB)       glDeleteRenderbuffers(1, &msVelocityRB);
    if (velocityResolveTex) glDeleteTextures(1, &velocityResolveTex);

    OSGLContext::unsetCurrentContextNoRender();
    pool->releaseGLContextFromRender(glContext);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ScanlineRender.cpp"
