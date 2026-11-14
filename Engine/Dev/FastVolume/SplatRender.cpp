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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <utility>
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
    // What the renderer holds now: the scenes, in order, by address and version.
    std::vector<std::pair<const SplatData*, unsigned long long> > uploaded;

    KnobDoubleWPtr splatScale, opacity;
    KnobChoiceWPtr shDegree;
    KnobBoolWPtr antialias, aovDepth;

    KnobChoiceWPtr sizeMode;
    KnobIntWPtr outputWidth, outputHeight;

    KnobIntWPtr samples;
    KnobBoolWPtr motionBlur, dof;
    KnobDoubleWPtr shutter, apertureScale;
    KnobChoiceWPtr shutterOffset;
    // Depth of field: where the focus and the f-stop come from, and the iris shape
    // (same names and meaning as LensDefocus' knobs).
    KnobChoiceWPtr focusMode;
    KnobDoubleWPtr focusDistance, fStop;
    KnobBoolWPtr fStopFromCamera;
    KnobIntWPtr blades;
    KnobDoubleWPtr curvature, rotation, squeeze;

    // Output rectangle: the project format, or the node's own size.
    void outputSize(EffectInstance* self, int* w, int* h, int* x1, int* y1) const
    {
        KnobChoicePtr m = sizeMode.lock();
        if (m && m->getValue() == 1) {
            KnobIntPtr kw = outputWidth.lock(), kh = outputHeight.lock();
            *w = std::max(1, kw ? kw->getValue() : 1920);
            *h = std::max(1, kh ? kh->getValue() : 1080);
            *x1 = *y1 = 0;
            return;
        }
        Format f;
        self->getApp()->getProject()->getProjectDefaultFormat(&f);
        *w = std::max(1, (int)(f.x2 - f.x1));
        *h = std::max(1, (int)(f.y2 - f.y1));
        *x1 = f.x1;
        *y1 = f.y1;
    }
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
           "Group3D that contains up to eight of them) into the Scene input and a Camera3D into "
           "the Camera input. Each splat is drawn as a soft, semi-transparent ellipse, blended far to "
           "near, with the view-dependent colour stored in the scan: the same picture the "
           "standard 3DGS viewers give. Outputs linear premultiplied RGBA plus a Depth plane to "
           "composite against ScanlineRender or CyclesRender. The Blur tab adds motion blur "
           "and depth of field. "
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

    // ---- Output size
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Output Size") );
        k->setName("sizeMode");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("Project", "Project Format", "Render at the project's format.") );
        entries.push_back( ChoiceOption("Custom", "Custom", "Render at the Width and Height below.") );
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip( tr("Size of the rendered picture. The camera's gate is fitted to it whatever its "
                              "shape, as in ScanlineRender, so keep the camera's apertures in the same "
                              "proportion as the picture.") );
        page->addKnob(k);
        _imp->sizeMode = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Width") );
        k->setName("outputWidth");
        k->setDefaultValue(1920);
        k->setMinimum(1);
        k->setDisplayMinimum(320);
        k->setDisplayMaximum(4096);
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->outputWidth = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Height") );
        k->setName("outputHeight");
        k->setDefaultValue(1080);
        k->setMinimum(1);
        k->setDisplayMinimum(240);
        k->setDisplayMaximum(4096);
        k->setAnimationEnabled(false);
        page->addKnob(k);
        _imp->outputHeight = k;
    }

    // ---- Motion blur / depth of field: several draws averaged
    KnobPagePtr blurPage = AppManager::createKnob<KnobPage>( this, tr("Blur") );
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Samples") );
        k->setName("samples");
        k->setDefaultValue(16);
        k->setMinimum(1);
        k->setMaximum(256);
        k->setDisplayMinimum(1);
        k->setDisplayMaximum(64);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Number of draws averaged when Motion Blur or Depth of Field is on (it does "
                              "nothing otherwise). Each draw takes as long as a frame without blur. Too "
                              "few show as separate ghost images; 16 is a fair start, 32 to 64 for wide "
                              "blurs.") );
        blurPage->addKnob(k);
        _imp->samples = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Motion Blur") );
        k->setName("motionBlur");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Blur from the movement of the camera and of the scene's transforms over the "
                              "shutter time. The splats themselves do not move.") );
        blurPage->addKnob(k);
        _imp->motionBlur = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Shutter") );
        k->setName("shutter");
        k->setDefaultValue(0.5);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(2.);
        k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("How long the shutter stays open, in frames. 0.5 is a 180 degree shutter.") );
        blurPage->addKnob(k);
        _imp->shutter = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Shutter Offset") );
        k->setName("shutterOffset");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("Centered", "Centered", "Open half before and half after the frame.") );
        entries.push_back( ChoiceOption("Start", "Start", "Opens at the frame.") );
        entries.push_back( ChoiceOption("End", "End", "Closes at the frame.") );
        k->populateChoices(entries);
        k->setDefaultValue(0);
        blurPage->addKnob(k);
        _imp->shutterOffset = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Depth of Field") );
        k->setName("dof");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Blur what is nearer or farther than the focus distance, as a real lens does. "
                              "This is a true depth of field: the scene is drawn Samples times from points "
                              "spread over the lens opening and averaged, so it is right through "
                              "semi-transparent splats and thin edges, where a blur made afterwards from a "
                              "depth pass has to guess. For lens character beyond the iris shape (cat's eye, "
                              "swirl, chromatic aberration, real lenses), leave this off and put a "
                              "LensDefocus after this node: it reads the Depth plane.") );
        blurPage->addKnob(k);
        _imp->dof = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Focus") );
        k->setName("focusMode");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("camera", "Camera", "The Focus Distance of the camera on the Camera input, so the camera "
                                                            "drives this node, CyclesRender and LensDefocus alike.") );
        entries.push_back( ChoiceOption("distance", "Distance", "Focus at Focus Distance below.") );
        k->populateChoices(entries);
        k->setDefaultValue(0);
        blurPage->addKnob(k);
        _imp->focusMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Focus Distance") );
        k->setName("focusDistance");
        k->setDefaultValue(5.0);
        k->setMinimum(1e-6);
        k->setDisplayMinimum(0.1);
        k->setDisplayMaximum(50.);
        k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("Distance from the camera, along its view axis, that is sharp (Focus = Distance), "
                              "in scene units. The Depth plane of this node gives that distance for any "
                              "pixel: view it and read the value under the cursor.") );
        blurPage->addKnob(k);
        _imp->focusDistance = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("F-Stop From Camera") );
        k->setName("fStopFromCamera");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Take the f-stop from the camera on the Camera input. Off: use F-Stop below.") );
        blurPage->addKnob(k);
        _imp->fStopFromCamera = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("F-Stop") );
        k->setName("fStop");
        k->setDefaultValue(2.8);
        k->setMinimum(0.1);
        k->setDisplayMinimum(0.7);
        k->setDisplayMaximum(22.);
        k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("Smaller = wider opening = more blur. Used when F-Stop From Camera is off.") );
        blurPage->addKnob(k);
        _imp->fStop = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Aperture Scale") );
        k->setName("apertureScale");
        k->setDefaultValue(1.0);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(100.);
        k->setAnimationEnabled(true);
        k->setHintToolTip( tr("Multiplies the size of the lens opening. The opening is worked out as if one "
                              "scene unit were one metre (focal length / (2 x F-Stop)); a scan has no real "
                              "unit, so raise this until the blur looks right for the scale of the scene. "
                              "To match a LensDefocus on the same camera: 1 = its Scene Units set to Metres, "
                              "100 = Centimetres, or Custom with Millimetres per Unit = 1000 / this value. "
                              "The two agree when the focus distance is much longer than the focal length; "
                              "LensDefocus also models the extra blur of close-up (macro) focus, this node, "
                              "like CyclesRender, does not.") );
        blurPage->addKnob(k);
        _imp->apertureScale = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Blades") );
        k->setName("blades");
        k->setDefaultValue(0);
        k->setMinimum(0);
        k->setMaximum(16);
        k->setDisplayMinimum(0);
        k->setDisplayMaximum(16);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Iris blades: the shape of out-of-focus highlights. 0 = circular opening; "
                              "3 to 16 = polygon. Shapes need more Samples than a plain disc to look clean.") );
        blurPage->addKnob(k);
        _imp->blades = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Blade Curvature") );
        k->setName("curvature");
        k->setDefaultValue(0.);
        k->setMinimum(0.);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("0 = straight blades, 1 = round. Real irises are rounder wide open.") );
        blurPage->addKnob(k);
        _imp->curvature = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Rotation") );
        k->setName("rotation");
        k->setDefaultValue(0.);
        k->setDisplayMinimum(-180.);
        k->setDisplayMaximum(180.);
        k->setAnimationEnabled(true);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("Iris rotation in degrees.") );
        blurPage->addKnob(k);
        _imp->rotation = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Anamorphic Squeeze") );
        k->setName("squeeze");
        k->setDefaultValue(1.);
        k->setMinimum(0.1);
        k->setDisplayMinimum(0.5);
        k->setDisplayMaximum(2.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Oval bokeh: above 1 makes out-of-focus highlights taller (anamorphic look), "
                              "below 1 wider. The area of the opening is kept.") );
        blurPage->addKnob(k);
        _imp->squeeze = k;
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

    int w, h, x1, y1;
    _imp->outputSize(this, &w, &h, &x1, &y1);
    RectI fmt;
    fmt.x1 = x1; fmt.y1 = y1;
    fmt.x2 = x1 + w; fmt.y2 = y1 + h;
    metadata.setOutputFormat(fmt);
    return eStatusOK;
}

StatusEnum
SplatRender::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                   ViewIdx /*view*/, RectD* rod)
{
    // Canonical (full-resolution) coordinates, not scaled by the render scale.
    int w, h, x1, y1;
    _imp->outputSize(this, &w, &h, &x1, &y1);
    rod->x1 = x1;
    rod->y1 = y1;
    rod->x2 = x1 + w;
    rod->y2 = y1 + h;
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

namespace {

struct SceneRef
{
    ReadSplats* node = NULL;
    SplatDataPtr data;
    float model[16];
};

struct CameraState
{
    float view[16];
    float projX = 1.f, projY = 1.f, nearClip = 0.1f;
    double focal = 50., fStop = 2.8, focusDistance = 10.;
};

// World matrices at `time` of the given ReadSplats nodes (their own transform under
// any parent Group3D), through the scene graph.
void splatMatricesAt(EffectInstance* self, double time, std::vector<SceneRef>& scenes)
{
    SceneGraph scene;
    NodesList allNodes;
    self->getApp()->getProject()->getNodes_recursive(allNodes, true);
    scene.rebuild(allNodes, time);
    for (const SceneNode& sn : scene.nodes()) {
        if (sn.type != eSceneNodeSplats) continue;
        NodePtr n = sn.sourceNode.lock();
        if (!n) continue;
        EffectInstance* e = n->getEffectInstance().get();
        for (SceneRef& r : scenes) {
            if ( (EffectInstance*)r.node == e ) std::memcpy(r.model, sn.worldMatrix, sizeof r.model);
        }
    }
}

// Run f(begin, end) over the pixel range [0, n) on several threads: the per-sample
// colour conversion and averaging of a blurred frame cost more than the draw itself
// when done on one.
template <typename F>
void parallelPixels(std::size_t n, F f)
{
    const unsigned hc = std::thread::hardware_concurrency();
    const std::size_t T = std::max<std::size_t>( 1, std::min<std::size_t>(16, hc ? hc : 4) );
    if (T == 1 || n < 65536) {
        f( (std::size_t)0, n );
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(T - 1);
    for (std::size_t t = 1; t < T; ++t) {
        const std::size_t b = n * t / T, e = n * (t + 1) / T;
        pool.emplace_back([&f, b, e]() { f(b, e); });
    }
    f( (std::size_t)0, n / T );
    for (std::thread& th : pool) th.join();
}

// Even spread of `n` points over the lens opening, in units of its radius: a disc, or
// an iris of `blades` blades whose sides bulge towards a circle with `curvature`,
// turned by `rotationDeg` and squeezed (area kept). Points come from a
// low-discrepancy sequence over the square, keeping those inside the shape, so the
// density is uniform over the opening whatever its shape.
double halton(int i, int b);
void lensPoints(int n, int blades, double curvature, double rotationDeg, double squeeze,
                std::vector<std::pair<double, double> >& out)
{
    out.clear();
    const double pi = 3.14159265358979323846;
    const double rot = rotationDeg * pi / 180.;
    const double sq = std::sqrt( std::max(0.01, squeeze) );
    const double seg = blades >= 3 ? 2. * pi / blades : 0.;
    for (int i = 0; (int)out.size() < n && i < n * 64 + 1024; ++i) {
        const double x = 2. * halton(i, 2) - 1., y = 2. * halton(i, 3) - 1.;
        const double r = std::sqrt(x * x + y * y);
        if (r > 1.) continue;
        if (blades >= 3) {
            // Distance to the polygon's edge along this direction (corners on the unit circle).
            double a = std::fmod(std::atan2(y, x) - rot, seg);
            if (a < 0.) a += seg;
            const double edge = std::cos(0.5 * seg) / std::cos(a - 0.5 * seg);
            if (r > edge + (1. - edge) * std::min( 1., std::max(0., curvature) ) ) continue;
        }
        out.push_back( std::make_pair(x / sq, y * sq) );
    }
    if ( out.empty() ) out.push_back( std::make_pair(0., 0.) );
    // Centre the set on the lens axis: a finite set of points is never exactly balanced, and
    // any offset would slide the whole out-of-focus picture sideways by a pixel or two.
    double mx = 0., my = 0.;
    for (std::size_t i = 0; i < out.size(); ++i) { mx += out[i].first; my += out[i].second; }
    mx /= (double)out.size();
    my /= (double)out.size();
    for (std::size_t i = 0; i < out.size(); ++i) { out[i].first -= mx; out[i].second -= my; }
}

// Point i of the Halton sequence in base b, in [0, 1).
double halton(int i, int b)
{
    double f = 1., r = 0.;
    for (int k = i + 1; k > 0; k /= b) {
        f /= b;
        r += f * (k % b);
    }
    return r;
}

} // namespace

StatusEnum
SplatRender::render(const RenderActionArgs& args)
{
    if ( !getInput(0) ) {
        setPersistentMessage(eMessageTypeError,
                             "SplatRender: connect a ReadSplats node (or a Scene3D / Group3D that contains "
                             "some) to the Scene input.");
        return eStatusFailed;
    }

    // ---- find the ReadSplats nodes wired upstream of the Scene input, with their
    // world matrices, through the scene graph.
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
    std::vector<SceneRef> scenes;
    int nSplatNodes = 0;
    std::string loadError;
    {
        SceneGraph scene;
        NodesList allNodes;
        getApp()->getProject()->getNodes_recursive(allNodes, true);
        scene.rebuild(allNodes, args.time);
        for (const SceneNode& sn : scene.nodes()) {
            if (sn.type != eSceneNodeSplats || !sn.visible) continue;
            NodePtr n = sn.sourceNode.lock();
            if ( !n || !upstream.count( n.get() ) || n->isNodeDisabled() ) continue;
            ReadSplats* rs = dynamic_cast<ReadSplats*>( n->getEffectInstance().get() );
            if (!rs) continue;
            ++nSplatNodes;
            if ( (int)scenes.size() >= SplatCore::kMaxScenes ) continue;
            SceneRef r;
            r.node = rs;
            r.data = rs->getSplatData();
            if (!r.data) {
                if ( loadError.empty() ) loadError = rs->getLastLoadError();
                continue;          // a node with no file, a failed read or an empty crop: skip it
            }
            std::memcpy(r.model, sn.worldMatrix, sizeof r.model);
            scenes.push_back(r);
        }
    }
    if (nSplatNodes == 0) {
        setPersistentMessage(eMessageTypeError, "SplatRender: no ReadSplats node in the connected scene.");
        return eStatusFailed;
    }
    if ( scenes.empty() ) {
        setPersistentMessage( eMessageTypeError,
                              ( "SplatRender: the ReadSplats node has no scene. " +
                                ( loadError.empty() ? std::string("Set its File.") : loadError ) ).c_str() );
        return eStatusFailed;
    }
    const int nScenes = (int)scenes.size();

    // ---- knobs
    SplatCore::DrawParams prm;
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
    EffectInstancePtr camEffect = skipDots( getInput(1) );
    CameraProvider* cp = camEffect ? dynamic_cast<CameraProvider*>( camEffect.get() ) : NULL;

    bool wantMB = _imp->motionBlur.lock() && _imp->motionBlur.lock()->getValue();
    // Without a camera the framing camera's 50 mm and the node's own Focus Distance / F-Stop are used.
    bool wantDof = _imp->dof.lock() && _imp->dof.lock()->getValue();
    const double shutter = _imp->shutter.lock() ? _imp->shutter.lock()->getValueAtTime(args.time) : 0.5;
    const int shutterMode = _imp->shutterOffset.lock() ? _imp->shutterOffset.lock()->getValue() : 0;
    const double apertureScale = _imp->apertureScale.lock() ? _imp->apertureScale.lock()->getValueAtTime(args.time) : 1.;
    const int focusMode = _imp->focusMode.lock() ? _imp->focusMode.lock()->getValue() : 0;
    const bool fStopFromCamera = !_imp->fStopFromCamera.lock() || _imp->fStopFromCamera.lock()->getValue();
    if (shutter <= 0.) wantMB = false;
    int nSamples = 1;
    if (wantMB || wantDof) {
        nSamples = _imp->samples.lock() ? std::max(1, std::min(256, _imp->samples.lock()->getValue())) : 16;
        if (args.draftMode) nSamples = std::max(1, nSamples / 4);     // scrubbing, dragging a knob
    }

    // Camera at a given time. No camera: stand back along +Z from the middle of the first scene's box.
    auto cameraAt = [&](double t, CameraState& cs) {
        double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
        double hAp = 24.576, vAp = 18.672;
        float farClip = 10000.f;
        if (cp) {
            cp->getCameraPosition(t, tx, ty, tz, rx, ry, rz);
            cs.focal = cp->getCameraFocalLength(t);
            hAp = cp->getCameraHAperture(t);
            vAp = cp->getCameraVAperture(t);
            cs.nearClip = (float)cp->getCameraNear(t);
            farClip = (float)cp->getCameraFar(t);
            cs.fStop = std::max( 0.1, cp->getCameraFStop(t) );
            cs.focusDistance = std::max( 1e-6, cp->getCameraFocusDistance(t) );
        } else {
            const SplatData& d = *scenes[0].data;
            const float* model = scenes[0].model;
            float c[3], half[3];
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (d.bboxMin[k] + d.bboxMax[k]);
                half[k] = 0.5f * (d.bboxMax[k] - d.bboxMin[k]);
            }
            const float wc[3] = {model[0] * c[0] + model[4] * c[1] + model[8] * c[2] + model[12],
                                 model[1] * c[0] + model[5] * c[1] + model[9] * c[2] + model[13],
                                 model[2] * c[0] + model[6] * c[1] + model[10] * c[2] + model[14]};
            const float sc = std::cbrt( std::fabs( model[0] * (model[5] * model[10] - model[9] * model[6])
                                                   - model[4] * (model[1] * model[10] - model[9] * model[2])
                                                   + model[8] * (model[1] * model[6] - model[5] * model[2]) ) );
            const float radius = sc * std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
            tx = wc[0]; ty = wc[1]; tz = wc[2] + 1.5 * radius;
            cs.nearClip = std::max(1e-4f, radius * 1e-3f);
            // keep the picture's own shape: there is no camera gate to respect
            const double aspect = (double)std::max( 1, args.roi.height() ) / (double)std::max( 1, args.roi.width() );
            vAp = hAp * aspect;
        }
        buildView(tx, ty, tz, rx, ry, rz, cs.view);
        float proj[16];
        CameraMath::composeProjectionMatrix(cs.focal, hAp, vAp, cs.nearClip, farClip, proj);
        cs.projX = proj[0];
        cs.projY = proj[5];
    };

    // ---- render: upload only when the scenes changed; draw every time.
    const int W = args.roi.width();
    const int H = args.roi.height();
    const bool linearize = scenes[0].node->getPreviewLinearize();
    static const SrgbToLinear toLinear;
    // The core blends in the splats' own (display) colourspace, as the scan was
    // trained. Convert a finished frame to linear through its unpremultiplied value.
    auto frameToLinear = [&](std::vector<float>& rgba) {
        float* base = rgba.data();
        parallelPixels( (std::size_t)W * (std::size_t)H, [base](std::size_t b, std::size_t e) {
            float* p = base + b * 4;
            for (std::size_t i = b; i < e; ++i, p += 4) {
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
        } );
    };

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
        std::vector<std::pair<const SplatData*, unsigned long long> > key;
        for (const SceneRef& r : scenes) key.push_back( std::make_pair(r.data.get(), r.data->version) );
        if ( key != _imp->uploaded || !_imp->renderer->hasScene() ) {
            std::vector<SplatCore::SceneDesc> sds( (std::size_t)nScenes );
            std::size_t total = 0, bytes = 0;
            int maxDeg = 0;
            for (int si = 0; si < nScenes; ++si) {
                const SplatData& d = *scenes[si].data;
                SplatCore::SceneDesc& sd = sds[(std::size_t)si];
                sd.count = d.count;
                sd.pos = d.pos.data();
                sd.scale = d.scale.data();
                sd.rot = d.rot.data();
                sd.opacity = d.opacity.data();
                sd.dc = d.dc.data();
                sd.shRest = d.shRest.empty() ? NULL : d.shRest.data();
                sd.shDegree = d.shDegree;
                sd.id = d.version;        // lets SplatRender nodes on the same scan share one GPU copy
                total += d.count;
                maxDeg = std::max(maxDeg, d.shRest.empty() ? 0 : d.shDegree);
            }
            bytes = SplatCore::Renderer::gpuBytesFor(total, maxDeg);
            if ( !_imp->renderer->upload(sds.data(), nScenes) ) {
                _imp->uploaded.clear();
                setPersistentMessage( eMessageTypeError, ( "SplatRender: " + _imp->renderer->lastError() ).c_str() );
                return eStatusFailed;
            }
            _imp->uploaded = key;
            std::fprintf( stderr, "[SplatRender] uploaded %llu splats in %d scene(s), SH degree %d, %.0f MB on %s\n",
                          (unsigned long long)total, nScenes, maxDeg, (double)bytes / 1048576.0,
                          _imp->renderer->adapterName().c_str() );
            std::fflush(stderr);
        }
        std::vector<const float*> positions( (std::size_t)nScenes );
        for (int si = 0; si < nScenes; ++si) positions[(std::size_t)si] = scenes[(std::size_t)si].data->pos.data();

        // NATRON_SPLAT_TIMING=1 prints where a frame's time went (stderr).
        static const bool timing = std::getenv("NATRON_SPLAT_TIMING") != NULL;
        typedef std::chrono::steady_clock Clock;
        auto msSince = [](const Clock::time_point& t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
        const Clock::time_point tFrame = Clock::now();
        double msSetup = 0., msSort = 0., msGpu = 0., msWait = 0., msDraw = 0., msPost = 0.;

        std::vector<std::pair<double, double> > lens;
        if (wantDof) {
            lensPoints(nSamples,
                       _imp->blades.lock() ? _imp->blades.lock()->getValue() : 0,
                       _imp->curvature.lock() ? _imp->curvature.lock()->getValue() : 0.,
                       _imp->rotation.lock() ? _imp->rotation.lock()->getValueAtTime(args.time) : 0.,
                       _imp->squeeze.lock() ? _imp->squeeze.lock()->getValue() : 1.,
                       lens);
        }

        std::vector<float> accRgba, accDepth, accAlpha;
        if (nSamples > 1) {
            accRgba.assign( (std::size_t)W * H * 4, 0.f );
            accDepth.assign( (std::size_t)W * H, 0.f );
            accAlpha.assign( (std::size_t)W * H, 0.f );
        }
        for (int smp = 0; smp < nSamples; ++smp) {
            if ( nSamples > 1 && aborted() ) {
                return eStatusFailed;
            }
            const Clock::time_point tS = Clock::now();
            // Time of this draw inside the shutter.
            double t = args.time;
            if (wantMB) {
                const double t01 = ( (double)smp + 0.5 ) / (double)nSamples;
                if (shutterMode == 1) t += t01 * shutter;
                else if (shutterMode == 2) t += (t01 - 1.) * shutter;
                else t += (t01 - 0.5) * shutter;
                splatMatricesAt(this, t, scenes);
            }
            CameraState cs;
            cameraAt(t, cs);
            std::memcpy(prm.view, cs.view, sizeof prm.view);
            for (int si = 0; si < nScenes; ++si) std::memcpy(prm.model[si], scenes[(std::size_t)si].model, 16 * sizeof(float));
            prm.projX = cs.projX;
            prm.projY = cs.projY;
            prm.nearClip = cs.nearClip;
            prm.ndcOffsetX = prm.ndcOffsetY = 0.f;
            if (wantDof) {
                // A point on the lens (even spread over a disc). Move the camera there, in its
                // own plane, and shift the picture back so that what lies at the focus distance
                // stays put: everything else smears, more the farther it is from that distance.
                double fStop = cs.fStop, focusDist = cs.focusDistance;
                if (!fStopFromCamera && _imp->fStop.lock()) fStop = std::max( 0.1, _imp->fStop.lock()->getValueAtTime(t) );
                if (focusMode == 1 && _imp->focusDistance.lock()) focusDist = std::max( 1e-6, _imp->focusDistance.lock()->getValueAtTime(t) );
                const double lensRadius = cs.focal / (2. * fStop) / 1000. * apertureScale;   // 1 unit = 1 m
                const std::pair<double, double>& lp = lens[(std::size_t)smp % lens.size()];
                const double lx = lensRadius * lp.first, ly = lensRadius * lp.second;
                prm.view[12] -= (float)lx;
                prm.view[13] -= (float)ly;
                prm.ndcOffsetX = (float)(cs.projX * lx / focusDist);
                prm.ndcOffsetY = (float)(cs.projY * ly / focusDist);
            }
            // Lens samples of one instant see almost the same order: sort once. With motion
            // blur the view really changes, so every draw sorts.
            prm.reuseOrder = (smp > 0 && !wantMB);
            msSetup += msSince(tS);
            const Clock::time_point tD = Clock::now();
            if ( !_imp->renderer->draw(positions.data(), prm, W, H, rout) ) {
                setPersistentMessage( eMessageTypeError, ( "SplatRender: " + _imp->renderer->lastError() ).c_str() );
                return eStatusFailed;
            }
            msDraw += msSince(tD);
            msSort += rout.sortMs;
            msGpu += rout.gpuMs;
            msWait += rout.waitMs;
            const Clock::time_point tP = Clock::now();
            if (linearize) frameToLinear(rout.rgba);      // average light, not display values
            if (nSamples > 1) {
                parallelPixels( (std::size_t)W * (std::size_t)H, [&](std::size_t b, std::size_t e) {
                    for (std::size_t i = b * 4; i < e * 4; ++i) accRgba[i] += rout.rgba[i];
                    for (std::size_t i = b; i < e; ++i) {
                        const float a = rout.rgba[i * 4 + 3];
                        accDepth[i] += rout.depth[i] * a;
                        accAlpha[i] += a;
                    }
                } );
            }
            msPost += msSince(tP);
        }
        if (nSamples > 1) {
            const std::size_t n = (std::size_t)W * (std::size_t)H;
            const float inv = 1.f / (float)nSamples;
            for (std::size_t i = 0; i < n * 4; ++i) rout.rgba[i] = accRgba[i] * inv;
            for (std::size_t i = 0; i < n; ++i) rout.depth[i] = accAlpha[i] > 1e-6f ? accDepth[i] / accAlpha[i] : 0.f;
        }
        if (timing) {
            std::fprintf(stderr, "[SplatRender] %dx%d, %d sample(s), %llu splats drawn: %.0f ms (per sample: setup %.1f, draw %.1f "
                                 "of which sort %.1f, GPU %.1f, copy back %.1f; colour + average %.1f)\n",
                         W, H, nSamples, (unsigned long long)rout.drawn, msSince(tFrame), msSetup / nSamples, msDraw / nSamples,
                         msSort / nSamples, msWait / nSamples, (msGpu - msWait) / nSamples, msPost / nSamples);
            std::fflush(stderr);
        }
    }
    if (nSplatNodes > SplatCore::kMaxScenes) {
        setPersistentMessage(eMessageTypeWarning,
                             "SplatRender: the scene holds more than 8 ReadSplats nodes; only the first 8 are rendered.");
    } else {
        clearPersistentMessage(false);
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
                    const float* sp = &rout.rgba[si * 4];
                    for (int c = 0; c < nc; ++c) dst[c] = c < 4 ? sp[c] : sp[3];
                    if (nc == 1) dst[0] = sp[3];
                }
            }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SplatRender.cpp"
