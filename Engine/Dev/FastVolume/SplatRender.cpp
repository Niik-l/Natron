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

#include "SplatRender.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include <QtCore/QMutex>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../Project.h"
#include "../DotUtils.h"
#include "../Scene3D/CameraMath.h"
#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/ReadSplats.h"
#include "../Scene3D/RotationConventions.h"
#include "../Scene3D/SceneGraph.h"
#include "../Scene3D/SplatData.h"

#include "SplatCore.h"

NATRON_NAMESPACE_ENTER

// Conventions (same as FastVolumeRender, verified there):
//   1. Natron cameras look down local -Z, extrinsic XYZ rotation; SplatCore takes
//      the plain view matrix (inverse of that transform).
//   2. Natron images are bottom-up; the core writes row 0 = top, so the plane
//      write loop flips Y.
//   3. The projection comes from CameraMath (both apertures, as ScanlineRender),
//      so a splat render lines up with a ScanlineRender of the same camera.

struct SplatRenderPrivate
{
    QMutex rendererLock;
    std::unique_ptr<SplatCore::Renderer> renderer;   // lazy, lives with the node
    const SplatData* uploadedData = nullptr;         // what the renderer holds now
    unsigned long long uploadedVersion = 0;

    KnobDoubleWPtr splatScale, opacity;
    KnobChoiceWPtr shDegree;
    KnobBoolWPtr antialias, aovDepth;
};

namespace {

ImagePlaneDesc depthPlane()
{
    static const char* rgb3[] = {"R", "G", "B"};
    return ImagePlaneDesc("Depth", "Depth", "", rgb3, 3);
}

// sRGB to linear for a whole frame: table with linear interpolation below 1,
// the exact curve above.
struct SrgbToLinear
{
    enum { kSize = 4096 };
    float v[kSize + 2];
    static float exact(float c)
    {
        return c <= 0.04045f ? c / 12.92f : std::pow( (c + 0.055f) / 1.055f, 2.4f );
    }
    SrgbToLinear()
    {
        for (int i = 0; i <= kSize + 1; ++i) v[i] = exact( (float)i / (float)kSize );
    }
    float operator()(float c) const
    {
        if (c <= 0.f) return 0.f;
        if (c >= 1.f) return exact(c);
        const float f = c * (float)kSize;
        const int i = (int)f;
        return v[i] + (v[i + 1] - v[i]) * ( f - (float)i );
    }
};

// Inverse of the camera-to-world transform, column-major (same maths as
// ScanlineRender's view matrix).
void buildView(double tx, double ty, double tz, double rx, double ry, double rz, float out[16])
{
    double mInv[3][3];
    RotationConventions::composeInverse(rx, ry, rz, mInv);
    const double ntx = -tx, nty = -ty, ntz = -tz;
    out[0]  = (float)mInv[0][0]; out[1]  = (float)mInv[1][0]; out[2]  = (float)mInv[2][0]; out[3]  = 0.f;
    out[4]  = (float)mInv[0][1]; out[5]  = (float)mInv[1][1]; out[6]  = (float)mInv[2][1]; out[7]  = 0.f;
    out[8]  = (float)mInv[0][2]; out[9]  = (float)mInv[1][2]; out[10] = (float)mInv[2][2]; out[11] = 0.f;
    out[12] = (float)(mInv[0][0] * ntx + mInv[0][1] * nty + mInv[0][2] * ntz);
    out[13] = (float)(mInv[1][0] * ntx + mInv[1][1] * nty + mInv[1][2] * ntz);
    out[14] = (float)(mInv[2][0] * ntx + mInv[2][1] * nty + mInv[2][2] * ntz);
    out[15] = 1.f;
}

} // namespace

SplatRender::SplatRender(NodePtr node)
    : EffectInstance(node)
    , _imp(new SplatRenderPrivate)
{
}

SplatRender::~SplatRender() {}

std::string
SplatRender::getPluginDescription() const
{
    return "GPU renderer for 3D Gaussian splat scenes. Wire a ReadSplats node (or a Scene3D / "
           "Group3D that contains one) into the Scene input and a Camera3D into the Camera "
           "input. Each splat is drawn as a soft, semi-transparent ellipse, blended far to "
           "near, with the view-dependent colour stored in the scan: the same picture the "
           "standard 3DGS viewers give. Outputs linear premultiplied RGBA at the project "
           "format, plus a Depth plane to composite against ScanlineRender or CyclesRender. "
           "Splats carry the lighting they were captured with: they cannot be relit, and they "
           "do not appear in reflections of other renders. For view-dependent highlights set "
           "SH Degree on the ReadSplats node above 0.";
}

std::string
SplatRender::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "Scene";
        case 1: return "Camera";
    }
    return "";
}

bool
SplatRender::isInputOptional(int inputNb) const
{
    return inputNb == 1;   // no camera -> the scene is framed from the front
}

void
SplatRender::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
SplatRender::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
SplatRender::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("Controls") );

    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Splat Scale") );
        k->setName("splatScale");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(2.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip( tr("Size of every splat, as a multiple of its trained size. 1 is the scan as "
                              "captured. Lower values open gaps between splats and show how the scene is "
                              "built; values above 1 soften it.") );
        page->addKnob(k);
        _imp->splatScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Opacity") );
        k->setName("opacity");
        k->setDefaultValue(1.0);
        k->setMinimum(0.0);
        k->setDisplayMinimum(0.0);
        k->setDisplayMaximum(1.0);
        k->setAnimationEnabled(true);
        k->setHintToolTip( tr("Multiplies the opacity of every splat. Not the same as fading the picture: "
                              "at lower values what lies behind starts to show through.") );
        page->addKnob(k);
        _imp->opacity = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("SH Degree") );
        k->setName("shDegree");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("0", "0 (base colour)", "One fixed colour per splat.") );
        entries.push_back( ChoiceOption("1", "1", "") );
        entries.push_back( ChoiceOption("2", "2", "") );
        entries.push_back( ChoiceOption("3", "3 (full)", "All the view-dependent colour in the scan.") );
        k->populateChoices(entries);
        k->setDefaultValue(3);
        k->setHintToolTip( tr("How much of the splats' view-dependent colour (spherical harmonics) to use: "
                              "highlights and reflections that shift as the camera moves. Limited to what "
                              "the ReadSplats node keeps in memory (its own SH Degree knob).") );
        page->addKnob(k);
        _imp->shDegree = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Antialiasing") );
        k->setName("antialias");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Widen every splat by about a third of a pixel, as the standard 3DGS renderer "
                              "does. Scans are trained with it; off, distant detail shimmers and thin "
                              "splats break up.") );
        page->addKnob(k);
        _imp->antialias = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Depth AOV") );
        k->setName("aovDepth");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Produce a Depth plane: for each pixel the mean distance from the camera, "
                              "along its view axis, of the splats visible there, in world units (0 where "
                              "the picture is empty). Splat edges are soft, so depth is an average there.") );
        page->addKnob(k);
        _imp->aovDepth = k;
    }
}

StatusEnum
SplatRender::getPreferredMetadata(NodeMetadata& metadata)
{
    // Depends on the frame through an animated camera or transform: keep time
    // in the image key (as FastVolumeRender / CyclesRender do).
    metadata.setIsFrameVarying(true);
    metadata.setNComps(-1, 4);
    metadata.setBitDepth(-1, eImageBitDepthFloat);

    Format f;
    getApp()->getProject()->getProjectDefaultFormat(&f);
    RectI fmt;
    fmt.x1 = f.x1; fmt.y1 = f.y1;
    fmt.x2 = f.x2; fmt.y2 = f.y2;
    metadata.setOutputFormat(fmt);
    return eStatusOK;
}

StatusEnum
SplatRender::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                   ViewIdx /*view*/, RectD* rod)
{
    Format f;
    getApp()->getProject()->getProjectDefaultFormat(&f);
    rod->x1 = f.x1;
    rod->y1 = f.y1;
    rod->x2 = f.x2;
    rod->y2 = f.y2;
    return eStatusOK;
}

void
SplatRender::getComponentsNeededAndProduced(double time, ViewIdx view,
                                            EffectInstance::ComponentsNeededMap* comps,
                                            double* passThroughTime, int* passThroughView,
                                            int* passThroughInput)
{
    std::list<ImagePlaneDesc> produced;
    produced.push_back( ImagePlaneDesc::getRGBAComponents() );
    KnobBoolPtr d = _imp->aovDepth.lock();
    if ( !d || d->getValue() ) produced.push_back( depthPlane() );
    (*comps)[-1] = produced;

    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInput = 0;
}

StatusEnum
SplatRender::render(const RenderActionArgs& args)
{
    if ( !getInput(0) ) {
        setPersistentMessage(eMessageTypeError,
                             "SplatRender: connect a ReadSplats node (or a Scene3D / Group3D that contains "
                             "one) to the Scene input.");
        return eStatusFailed;
    }

    // ---- find the ReadSplats wired upstream of the Scene input, with its world
    // matrix (its own transform under any parent Group3D), through the scene graph.
    std::set<const Node*> upstream;
    {
        std::vector<EffectInstancePtr> todo;
        todo.push_back( getInput(0) );
        while ( !todo.empty() ) {
            EffectInstancePtr e = todo.back();
            todo.pop_back();
            if (!e) continue;
            NodePtr en = e->getNode();
            if ( !en || !upstream.insert( en.get() ).second ) continue;
            const int ni = e->getNInputs();
            for (int i = 0; i < ni; ++i) todo.push_back( e->getInput(i) );
        }
    }
    SceneGraph scene;
    {
        NodesList allNodes;
        getApp()->getProject()->getNodes_recursive(allNodes, true);
        scene.rebuild(allNodes, args.time);
    }
    ReadSplats* splats = NULL;
    int nSplatNodes = 0;
    float model[16];
    for (const SceneNode& sn : scene.nodes()) {
        if (sn.type != eSceneNodeSplats || !sn.visible) continue;
        NodePtr n = sn.sourceNode.lock();
        if ( !n || !upstream.count( n.get() ) || n->isNodeDisabled() ) continue;
        ReadSplats* rs = dynamic_cast<ReadSplats*>( n->getEffectInstance().get() );
        if (!rs) continue;
        ++nSplatNodes;
        if (!splats) {
            splats = rs;
            std::memcpy(model, sn.worldMatrix, sizeof model);
        }
    }
    if (!splats) {
        setPersistentMessage(eMessageTypeError, "SplatRender: no ReadSplats node in the connected scene.");
        return eStatusFailed;
    }
    SplatDataPtr data = splats->getSplatData();
    if (!data) {
        const std::string why = splats->getLastLoadError();
        setPersistentMessage( eMessageTypeError,
                              ( "SplatRender: the ReadSplats node has no scene. " +
                                ( why.empty() ? std::string("Set its File.") : why ) ).c_str() );
        return eStatusFailed;
    }

    // ---- draw parameters
    SplatCore::DrawParams prm;
    std::memcpy(prm.model, model, sizeof prm.model);
    {
        KnobDoublePtr k = _imp->splatScale.lock();
        prm.splatScale = k ? (float)k->getValueAtTime(args.time) : 1.f;
        k = _imp->opacity.lock();
        prm.opacityScale = k ? (float)k->getValueAtTime(args.time) : 1.f;
        KnobChoicePtr c = _imp->shDegree.lock();
        prm.shDegree = c ? c->getValue() : 3;
        KnobBoolPtr b = _imp->antialias.lock();
        prm.antialias = b ? b->getValue() : true;
    }
    {
        double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
        double focal = 50., hAp = 24.576, vAp = 18.672;
        float nearClip = 0.1f, farClip = 10000.f;
        EffectInstancePtr camEffect = skipDots( getInput(1) );
        CameraProvider* cp = camEffect ? dynamic_cast<CameraProvider*>( camEffect.get() ) : NULL;
        if (cp) {
            cp->getCameraPosition(args.time, tx, ty, tz, rx, ry, rz);
            focal = cp->getCameraFocalLength(args.time);
            hAp = cp->getCameraHAperture(args.time);
            vAp = cp->getCameraVAperture(args.time);
            nearClip = (float)cp->getCameraNear(args.time);
            farClip = (float)cp->getCameraFar(args.time);
        } else {
            // No camera: stand back along +Z from the middle of the scene's box.
            float c[3], half[3];
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (data->bboxMin[k] + data->bboxMax[k]);
                half[k] = 0.5f * (data->bboxMax[k] - data->bboxMin[k]);
            }
            const float wc[3] = {model[0] * c[0] + model[4] * c[1] + model[8] * c[2] + model[12],
                                 model[1] * c[0] + model[5] * c[1] + model[9] * c[2] + model[13],
                                 model[2] * c[0] + model[6] * c[1] + model[10] * c[2] + model[14]};
            const float s = std::cbrt( std::fabs( model[0] * (model[5] * model[10] - model[9] * model[6])
                                                  - model[4] * (model[1] * model[10] - model[9] * model[2])
                                                  + model[8] * (model[1] * model[6] - model[5] * model[2]) ) );
            const float radius = s * std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
            tx = wc[0]; ty = wc[1]; tz = wc[2] + 1.5 * radius;
            nearClip = std::max(1e-4f, radius * 1e-3f);
        }
        buildView(tx, ty, tz, rx, ry, rz, prm.view);
        float proj[16];
        CameraMath::composeProjectionMatrix(focal, hAp, vAp, nearClip, farClip, proj);
        prm.projX = proj[0];
        prm.projY = proj[5];
        prm.nearClip = nearClip;
    }

    // ---- render: upload only when the scene object changed; draw every time.
    const int W = args.roi.width();
    const int H = args.roi.height();
    SplatCore::RenderOutput rout;
    {
        QMutexLocker lock(&_imp->rendererLock);
        if (!_imp->renderer) {
            _imp->renderer.reset(new SplatCore::Renderer);
        }
        if ( !_imp->renderer->valid() ) {
            setPersistentMessage( eMessageTypeError,
                                  ( "SplatRender: GPU init failed: " + _imp->renderer->lastError() ).c_str() );
            return eStatusFailed;
        }
        if ( _imp->uploadedData != data.get() || _imp->uploadedVersion != data->version || !_imp->renderer->hasScene() ) {
            SplatCore::SceneDesc sd;
            sd.count = data->count;
            sd.pos = data->pos.data();
            sd.scale = data->scale.data();
            sd.rot = data->rot.data();
            sd.opacity = data->opacity.data();
            sd.dc = data->dc.data();
            sd.shRest = data->shRest.empty() ? NULL : data->shRest.data();
            sd.shDegree = data->shDegree;
            if ( !_imp->renderer->upload(sd) ) {
                _imp->uploadedData = NULL;
                setPersistentMessage( eMessageTypeError, ( "SplatRender: " + _imp->renderer->lastError() ).c_str() );
                return eStatusFailed;
            }
            _imp->uploadedData = data.get();
            _imp->uploadedVersion = data->version;
            std::fprintf( stderr, "[SplatRender] uploaded %llu splats, SH degree %d, %.0f MB on %s\n",
                          (unsigned long long)data->count, data->shDegree,
                          (double)SplatCore::Renderer::gpuBytesFor(data->count, data->shDegree) / 1048576.0,
                          _imp->renderer->adapterName().c_str() );
            std::fflush(stderr);
        }
        if ( !_imp->renderer->draw(data->pos.data(), prm, W, H, rout) ) {
            setPersistentMessage( eMessageTypeError, ( "SplatRender: " + _imp->renderer->lastError() ).c_str() );
            return eStatusFailed;
        }
    }
    if (nSplatNodes > 1) {
        setPersistentMessage(eMessageTypeWarning,
                             "SplatRender: the scene holds more than one ReadSplats node; only the first is "
                             "rendered. Render each with its own SplatRender and merge them with their Depth.");
    } else {
        clearPersistentMessage(false);
    }

    // ---- colours: the core blends in the splats' own (display) colourspace, as
    // the scan was trained. Convert the finished pixel to linear, through its
    // unpremultiplied value.
    if ( splats->getPreviewLinearize() ) {
        static const SrgbToLinear toLinear;
        const std::size_t n = (std::size_t)W * (std::size_t)H;
        float* p = rout.rgba.data();
        for (std::size_t i = 0; i < n; ++i, p += 4) {
            const float a = p[3];
            if (a <= 1e-6f) {
                p[0] = p[1] = p[2] = 0.f;
                continue;
            }
            const float inv = 1.f / a;
            p[0] = toLinear(p[0] * inv) * a;
            p[1] = toLinear(p[1] * inv) * a;
            p[2] = toLinear(p[2] * inv) * a;
        }
    }

    // ---- write planes (core row 0 = image top; Natron y1 = image bottom)
    for (auto& planePair : args.outputPlanes) {
        const ImagePlaneDesc& desc = planePair.first;
        ImagePtr img = planePair.second;
        if (!img) continue;
        const RectI bounds = img->getBounds();
        const int nc = desc.getNumComponents();
        const bool isDepth = desc.getPlaneID() == "Depth";

        Image::WriteAccess wa( img.get() );
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            const int srcY = (H - 1) - (y - args.roi.y1);
            if (srcY < 0 || srcY >= H) continue;
            for (int x = bounds.x1; x < bounds.x2; ++x) {
                const int srcX = x - args.roi.x1;
                if (srcX < 0 || srcX >= W) continue;
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;
                const std::size_t si = (std::size_t)srcY * W + srcX;
                if (isDepth) {
                    for (int c = 0; c < nc; ++c) dst[c] = rout.depth[si];
                } else {
                    const float* s = &rout.rgba[si * 4];
                    for (int c = 0; c < nc; ++c) dst[c] = c < 4 ? s[c] : s[3];
                    if (nc == 1) dst[0] = s[3];
                }
            }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SplatRender.cpp"
