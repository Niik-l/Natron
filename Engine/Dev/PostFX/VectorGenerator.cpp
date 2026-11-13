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
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "VectorGenerator.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "FlowCore.h"

NATRON_NAMESPACE_ENTER

namespace {
enum OutputEnum
{
    eOutputForward = 0,
    eOutputBackward = 1,
};
}

struct VectorGeneratorPrivate
{
    KnobChoiceWPtr output;
    KnobDoubleWPtr vectorDetail;
    KnobDoubleWPtr smoothness;
    KnobBoolWPtr flickerCompensation;
    KnobDoubleWPtr temporalSmoothing;
    KnobDoubleWPtr weightR, weightG, weightB;

    // The vector fields of the last few frame pairs, before temporal smoothing. Smoothing frame N
    // needs the fields of N-1, N and N+1, so playing through a shot reuses two of every three.
    struct CachedField
    {
        U64 hash;
        double time, otherTime, scale;
        RectI bounds;
        std::vector<float> flow, conf;
    };
    std::list<CachedField> fields;
    QMutex fieldsMutex;
};

VectorGenerator::VectorGenerator(NodePtr node)
    : EffectInstance(node)
    , _imp( new VectorGeneratorPrivate() )
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VectorGenerator::~VectorGenerator()
{
}

std::string
VectorGenerator::getPluginDescription() const
{
    return "Motion vectors from footage, by optical flow.\n\n"
           "For every pixel the node works out where it goes in the next frame (Forward) or where it was in the previous "
           "frame (Backward). The output is a vector image: red is the horizontal motion and green the vertical motion, "
           "in pixels per frame, and alpha is a confidence between 0 and 1.\n\n"
           "Connect the output to the Vectors input of VectorBlur to add motion blur to footage shot without it. "
           "TimeFlow uses the same motion estimator for retiming.\n\n"
           "Vector Detail sets the resolution of the vector field: lower values are faster and smoother, higher values "
           "follow small objects. Smoothness evens the field out inside objects without blurring it across their edges.";
}

void
VectorGenerator::addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
}

void
VectorGenerator::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
VectorGenerator::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
VectorGenerator::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("Controls") );

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Output") );
        k->setName("output");
        std::vector<ChoiceOption> opts;
        opts.push_back( ChoiceOption("forward", "Forward", "Where each pixel of this frame goes in the next frame.") );
        opts.push_back( ChoiceOption("backward", "Backward", "Where each pixel of this frame was in the previous frame.") );
        k->populateChoices(opts);
        k->setDefaultValue(eOutputForward);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Which neighbouring frame the motion is measured against. Forward is what VectorBlur expects.") );
        page->addKnob(k);
        _imp->output = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Vector Detail") );
        k->setName("vectorDetail");
        k->setDefaultValue(0.5);
        k->setMinimum(0.05);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.05);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Resolution of the vector field relative to the image. 1 computes a vector for every pixel; "
                              "0.5 computes at half resolution, which is about four times faster and less noisy. "
                              "Raise it when small or thin objects are not followed.") );
        page->addKnob(k);
        _imp->vectorDetail = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Smoothness") );
        k->setName("smoothness");
        k->setDefaultValue(0.5);
        k->setMinimum(0.);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("How strongly neighbouring vectors are evened out. Low values keep local detail but can leave "
                              "stray vectors; high values give a cleaner field that may miss small motions. "
                              "The smoothing does not cross edges in the image.") );
        page->addKnob(k);
        _imp->smoothness = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Flicker Compensation") );
        k->setName("flickerCompensation");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Ignore brightness changes between the two frames (flicker, exposure ramps, a light switching on) "
                              "so they are not mistaken for motion.") );
        page->addKnob(k);
        _imp->flickerCompensation = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Temporal Smoothing") );
        k->setName("temporalSmoothing");
        k->setDefaultValue(1.);
        k->setMinimum(0.);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Steadies the vectors over time. Each frame is otherwise solved on its own, so in flat areas the "
                              "vectors land somewhere slightly different every frame, which shows as shimmer in a blur or a "
                              "retime. This follows each pixel into the previous and the next frame and averages the vectors "
                              "that agree. 0 turns it off. It needs the neighbouring frames, so a single frame takes about "
                              "three times as long; playing through a shot costs little extra.") );
        page->addKnob(k);
        _imp->temporalSmoothing = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>( this, tr("") );
        page->addKnob(sep);
    }
    const char* names[3] = { "weightRed", "weightGreen", "weightBlue" };
    const char* labels[3] = { "Weight Red", "Weight Green", "Weight Blue" };
    const double defaults[3] = { 0.3, 0.6, 0.1 };
    KnobDoubleWPtr* targets[3] = { &_imp->weightR, &_imp->weightG, &_imp->weightB };
    for (int i = 0; i < 3; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr(labels[i]) );
        k->setName(names[i]);
        k->setDefaultValue(defaults[i]);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("How much this channel counts when looking for motion. Lower the weight of a noisy channel "
                              "(often blue), or raise one to follow something only visible in that channel.") );
        page->addKnob(k);
        *targets[i] = k;
    }
} // initializeKnobs

FramesNeededMap
VectorGenerator::getFramesNeeded(double time,
                                 ViewIdx view)
{
    FramesNeededMap ret;
    const bool backward = _imp->output.lock()->getValue() == eOutputBackward;
    RangeD range;

    range.min = backward ? time - 1. : time;
    range.max = backward ? time : time + 1.;
    if (_imp->temporalSmoothing.lock()->getValue() > 0.) {
        // the fields of the previous and the next frame as well
        range.min -= 1.;
        range.max += 1.;
    }
    std::vector<RangeD> ranges;
    ranges.push_back(range);
    FrameRangesMap views;
    views.insert( std::make_pair(view, ranges) );
    if ( getInput(0) ) {
        ret.insert( std::make_pair(0, views) );
    }

    return ret;
}

StatusEnum
VectorGenerator::getRegionOfDefinition(U64 /*hash*/,
                                       double time,
                                       const RenderScale& scale,
                                       ViewIdx view,
                                       RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusFailed;
    }
    bool isProject;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
    if ( (st != eStatusOK) && (st != eStatusReplyDefault) ) {
        return st;
    }

    return eStatusOK;
}

namespace {
// The weighted, tone-compressed brightness of `img` over `bounds` (zero where the image has no data).
void
toMatchImage(const ImagePtr& img,
             const RectI& bounds,
             const float weights[3],
             std::vector<float>* out)
{
    const int W = bounds.width(), H = bounds.height();

    out->assign( (std::size_t)W * H, 0.f );
    if (!img) {
        return;
    }
    Image::ReadAccess ra( img.get() );
    const RectI ib = img->getBounds();
    const int nComp = img->getComponents().getNumComponents();
    const float wsum = std::max(1e-6f, weights[0] + weights[1] + weights[2]);
    for (int y = std::max(bounds.y1, ib.y1); y < std::min(bounds.y2, ib.y2); ++y) {
        float* o = &(*out)[(std::size_t)(y - bounds.y1) * W];
        for (int x = std::max(bounds.x1, ib.x1); x < std::min(bounds.x2, ib.x2); ++x) {
            const float* p = (const float*)ra.pixelAt(x, y);
            if (!p) {
                continue;
            }
            float v;
            if (nComp >= 3) {
                v = (weights[0] * p[0] + weights[1] * p[1] + weights[2] * p[2]) / wsum;
            } else {
                v = p[0];
            }
            o[x - bounds.x1] = FlowCore::matchEncode(v);
        }
    }
}
} // namespace

// The vector field from the frame at `time` to the frame at `otherTime`, over `bounds`. Returns false
// when either frame is missing or the render was aborted.
bool
VectorGenerator::computeField(double time,
                              double otherTime,
                              const RenderActionArgs& args,
                              const RectI& bounds,
                              std::vector<float>* flow,
                              std::vector<float>* conf)
{
    const U64 hash = getHash();
    {
        QMutexLocker l(&_imp->fieldsMutex);
        for (std::list<VectorGeneratorPrivate::CachedField>::iterator it = _imp->fields.begin(); it != _imp->fields.end(); ++it) {
            if ( (it->hash == hash) && (it->time == time) && (it->otherTime == otherTime) && (it->scale == args.mappedScale.toOfxPointD().x)
                 && (it->bounds.x1 == bounds.x1) && (it->bounds.y1 == bounds.y1) && (it->bounds.x2 == bounds.x2) && (it->bounds.y2 == bounds.y2) ) {
                *flow = it->flow;
                if (conf) {
                    *conf = it->conf;
                }
                _imp->fields.splice(_imp->fields.begin(), _imp->fields, it); // most recently used first

                return true;
            }
        }
    }
    const int W = bounds.width(), H = bounds.height();
    if ( (W < 8) || (H < 8) ) {
        return false;
    }
    RectI roiA, roiB;
    ImagePtr imgA = getImage(0, time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roiA);
    ImagePtr imgB = getImage(0, otherTime, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roiB);
    if ( !imgA || !imgB || aborted() ) {
        return false;
    }
    const float weights[3] = {
        (float)_imp->weightR.lock()->getValue(), (float)_imp->weightG.lock()->getValue(), (float)_imp->weightB.lock()->getValue()
    };
    std::vector<float> a, b, c;
    toMatchImage(imgA, bounds, weights, &a);
    toMatchImage(imgB, bounds, weights, &b);
    imgA.reset();
    imgB.reset();
    FlowCore::Params p;
    p.vectorDetail = _imp->vectorDetail.lock()->getValue();
    p.smoothness = _imp->smoothness.lock()->getValue();
    p.meanNormalize = _imp->flickerCompensation.lock()->getValue();
    if ( !FlowCore::computeFlow(&a[0], &b[0], W, H, p, flow, &c) || aborted() ) {
        return false;
    }
    {
        QMutexLocker l(&_imp->fieldsMutex);
        _imp->fields.push_front( VectorGeneratorPrivate::CachedField() );
        VectorGeneratorPrivate::CachedField& e = _imp->fields.front();
        e.hash = hash;
        e.time = time;
        e.otherTime = otherTime;
        e.scale = args.mappedScale.toOfxPointD().x;
        e.bounds = bounds;
        e.flow = *flow;
        e.conf = c;
        while (_imp->fields.size() > 4) {
            _imp->fields.pop_back();
        }
    }
    if (conf) {
        conf->swap(c);
    }

    return true;
} // computeField

StatusEnum
VectorGenerator::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) {
        return eStatusFailed;
    }
    const bool backward = _imp->output.lock()->getValue() == eOutputBackward;
    const double step = backward ? -1. : 1.;
    const double temporal = _imp->temporalSmoothing.lock()->getValue();
    const RectI outBounds = outImg->getBounds();
    const int W = outBounds.width(), H = outBounds.height();
    const int outNComp = outImg->getComponents().getNumComponents();
    std::vector<float> flow, conf;
    const bool haveFlow = computeField(args.time, args.time + step, args, outBounds, &flow, &conf);
    if ( aborted() ) {
        return eStatusFailed;
    }
    if ( haveFlow && (temporal > 0.) ) {
        // the same kind of field one frame further along the motion, and one frame back
        std::vector<float> ahead, behind;
        const bool haveAhead = computeField(args.time + step, args.time + 2. * step, args, outBounds, &ahead, NULL);
        const bool haveBehind = computeField(args.time - step, args.time, args, outBounds, &behind, NULL);
        if ( aborted() ) {
            return eStatusFailed;
        }
        FlowCore::temporalFilter(flow, haveAhead ? &ahead : NULL, haveBehind ? &behind : NULL, W, H, temporal, &flow);
    }

    Image::WriteAccess wa( outImg.get() );
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) {
                continue;
            }
            const std::size_t k = (std::size_t)(y - outBounds.y1) * W + (x - outBounds.x1);
            const float v[4] = { haveFlow ? flow[2 * k] : 0.f, haveFlow ? flow[2 * k + 1] : 0.f, 0.f, haveFlow ? conf[k] : 0.f };
            for (int c = 0; c < outNComp; ++c) {
                dst[c] = v[c < 4 ? c : 3];
            }
        }
    }

    return eStatusOK;
} // render

NATRON_NAMESPACE_EXIT
