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

#include "TimeFlow.h"

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
#include "Engine/NodeMetadata.h"

#include "FlowCore.h"

NATRON_NAMESPACE_ENTER

namespace {
enum TimingEnum
{
    eTimingSpeed = 0,
    eTimingSourceFrame = 1,
};

enum MethodEnum
{
    eMethodMotion = 0,
    eMethodBlend = 1,
    eMethodNearest = 2,
};

enum VectorKindEnum
{
    eVectorKindBackward = 0,
    eVectorKindVelocity = 1,
};

// a source time this close to a whole frame is that frame
const double kOnFrame = 1e-4;
}

struct TimeFlowPrivate
{
    KnobChoiceWPtr timing;
    KnobDoubleWPtr speed;
    KnobDoubleWPtr startFrame;
    KnobDoubleWPtr sourceFrame;
    KnobChoiceWPtr method;
    KnobDoubleWPtr vectorDetail;
    KnobDoubleWPtr smoothness;
    KnobBoolWPtr flickerCompensation;
    KnobDoubleWPtr temporalSmoothing;
    KnobChoiceWPtr vectorKind;
    KnobDoubleWPtr weightR, weightG, weightB;

    // The vector fields of the last few frame pairs. A slowed clip builds several output frames
    // from the same pair of source frames, so most renders find their two fields here.
    struct CachedField
    {
        U64 hash;
        double time, otherTime, scale;
        RectI bounds;
        bool layered;
        std::vector<float> flow, background;
    };
    std::list<CachedField> fields;
    QMutex fieldsMutex;
};

TimeFlow::TimeFlow(NodePtr node)
    : EffectInstance(node)
    , _imp( new TimeFlowPrivate() )
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

TimeFlow::~TimeFlow()
{
}

std::string
TimeFlow::getPluginDescription() const
{
    return "Retimes a clip: slow motion, speed-ups and speed ramps.\n\n"
           "Where the new timing lands between two source frames, the picture is built by moving both frames along their "
           "motion and mixing them, rather than dissolving one into the other, so moving things stay sharp instead of "
           "doubling. The motion is estimated the same way as in VectorGenerator.\n\n"
           "Timing is either a constant Speed (0.5 = half speed) or a Source Frame curve: animate it to say which source "
           "frame shows at each output frame.\n\n"
           "Where something is covered or uncovered between the two frames there is no motion to follow; there the node "
           "uses whichever frame can see the area, and falls back to a dissolve when neither can be trusted.\n\n"
           "The optional Vectors input replaces the node's own estimate with supplied motion vectors, for example the "
           "velocity pass of a 3D render.\n\n"
           "The optional Matte input takes the alpha of a foreground object (a roto shape or a key). The object and the "
           "background behind it are then followed and rebuilt separately and put back together, which removes the faint "
           "double outline a moving object otherwise gets at its edge. The matte must cover the object on every source frame.";
}

void
TimeFlow::addAcceptedComponents(int inputNb,
                                std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    if (inputNb == 2) {
        comps->push_back( ImagePlaneDesc::getAlphaComponents() );
    } else {
        comps->push_back( ImagePlaneDesc::getRGBComponents() );
    }
}

void
TimeFlow::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
TimeFlow::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
TimeFlow::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("Controls") );

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Timing") );
        k->setName("timing");
        std::vector<ChoiceOption> opts;
        opts.push_back( ChoiceOption("speed", "Speed", "A constant speed, counted from Start Frame.") );
        opts.push_back( ChoiceOption("sourceFrame", "Source Frame", "The Source Frame curve says which source frame shows at each output frame.") );
        k->populateChoices(opts);
        k->setDefaultValue(eTimingSpeed);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("How the new timing is given. Use Speed for a constant change and Source Frame for ramps and freezes.") );
        page->addKnob(k);
        _imp->timing = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Speed") );
        k->setName("speed");
        k->setDefaultValue(0.5);
        k->setMinimum(0.001);
        k->setDisplayMinimum(0.1);
        k->setDisplayMaximum(4.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Playback speed when Timing is Speed. 1 is unchanged, 0.5 is half speed (twice as long), 2 is double "
                              "speed. For a speed that changes over the shot, use Source Frame instead.") );
        page->addKnob(k);
        _imp->speed = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Start Frame") );
        k->setName("startFrame");
        k->setDefaultValue(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(200.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("The frame that stays where it is when Timing is Speed: at this frame the output shows the same source "
                              "frame, and the speed change is counted from here. Normally the first frame of the clip.") );
        page->addKnob(k);
        _imp->startFrame = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Source Frame") );
        k->setName("sourceFrame");
        k->setDefaultValue(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(200.);
        k->setAnimationEnabled(true);
        k->setHintToolTip( tr("The source frame shown at the current frame when Timing is Source Frame. Set keys on it: for example "
                              "1 at frame 1 and 50 at frame 100 plays frames 1 to 50 at half speed. Fractions are allowed.") );
        page->addKnob(k);
        _imp->sourceFrame = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Method") );
        k->setName("method");
        std::vector<ChoiceOption> opts;
        opts.push_back( ChoiceOption("motion", "Motion", "Move the two nearest frames along their motion and mix them.") );
        opts.push_back( ChoiceOption("blend", "Blend", "Dissolve between the two nearest frames. Fast; moving things double up.") );
        opts.push_back( ChoiceOption("nearest", "Nearest", "Show the nearest source frame unchanged. Fastest; motion stutters.") );
        k->populateChoices(opts);
        k->setDefaultValue(eMethodMotion);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("How in-between frames are made. Blend and Nearest are there for comparison and for shots where "
                              "the motion cannot be followed.") );
        page->addKnob(k);
        _imp->method = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>( this, tr("Motion") );
        sep->setName("motionSep");
        page->addKnob(sep);
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
        k->setHintToolTip( tr("Resolution of the motion estimate relative to the image. 1 follows every pixel; 0.5 works at half "
                              "resolution, which is about four times faster. Raise it when small or thin objects smear.") );
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
        k->setHintToolTip( tr("How strongly neighbouring motion vectors are evened out. Low values follow local detail but can tear; "
                              "high values hold objects together but may miss small motions.") );
        page->addKnob(k);
        _imp->smoothness = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Flicker Compensation") );
        k->setName("flickerCompensation");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Ignore brightness changes between the two frames (flicker, exposure ramps) so they are not mistaken "
                              "for motion.") );
        page->addKnob(k);
        _imp->flickerCompensation = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Temporal Smoothing") );
        k->setName("temporalSmoothing");
        k->setDefaultValue(0.);
        k->setMinimum(0.);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Steadies the motion estimate using the source frames before and after the pair being mixed. Try it "
                              "when flat areas shimmer. It needs four more motion solves per pair of source frames, so it is off "
                              "by default.") );
        page->addKnob(k);
        _imp->temporalSmoothing = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Vectors Input") );
        k->setName("vectorKind");
        std::vector<ChoiceOption> opts;
        opts.push_back( ChoiceOption("backward", "Backward Vectors", "Where each pixel was in the previous frame: VectorGenerator set to Backward.") );
        opts.push_back( ChoiceOption("velocity", "Velocity Pass", "How far each pixel moved since the previous frame: ScanlineRender's Velocity layer.") );
        k->populateChoices(opts);
        k->setDefaultValue(eVectorKindBackward);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("What the optional Vectors input holds. Only used when it is connected. Red is the horizontal and "
                              "green the vertical motion, in pixels per frame, belonging to the same frame as the picture.") );
        page->addKnob(k);
        _imp->vectorKind = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>( this, tr("") );
        sep->setName("weightSep");
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

double
TimeFlow::sourceTime(double time) const
{
    if (_imp->timing.lock()->getValue() == eTimingSourceFrame) {
        return _imp->sourceFrame.lock()->getValueAtTime(time);
    }
    const double start = _imp->startFrame.lock()->getValue();

    return start + (time - start) * _imp->speed.lock()->getValue();
}

void
TimeFlow::splitTime(double src,
                    double* before,
                    double* position)
{
    double b = std::floor(src);
    double p = src - b;

    if (p < kOnFrame) {
        p = 0.;
    } else if (p > 1. - kOnFrame) {
        b += 1.;
        p = 0.;
    }
    *before = b;
    *position = p;
}

bool
TimeFlow::isIdentity(double time,
                     const RenderScale & /*scale*/,
                     const RectI & /*roi*/,
                     ViewIdx view,
                     double* inputTime,
                     ViewIdx* inputView,
                     int* inputNb)
{
    double before, position;

    splitTime(sourceTime(time), &before, &position);
    if (_imp->method.lock()->getValue() == eMethodNearest) {
        *inputTime = position < 0.5 ? before : before + 1.;
    } else if (position == 0.) {
        *inputTime = before;
    } else {
        return false;
    }
    *inputView = view;
    *inputNb = 0;

    return true;
}

void
TimeFlow::getFrameRange(double *first,
                        double *last)
{
    EffectInstance::getFrameRange(first, last);
    if (_imp->timing.lock()->getValue() != eTimingSpeed) {
        return;
    }
    // where the first and the last source frame land on the output
    const double start = _imp->startFrame.lock()->getValue();
    const double speed = std::max( 0.001, _imp->speed.lock()->getValue() );
    if ( (*first > -1e8) && (*last < 1e8) ) {
        *first = std::floor(start + (*first - start) / speed + 0.5);
        *last = std::floor(start + (*last - start) / speed + 0.5);
    }
}

StatusEnum
TimeFlow::getPreferredMetadata(NodeMetadata& metadata)
{
    // the output changes from frame to frame even when no parameter is animated
    metadata.setIsFrameVarying(true);

    return eStatusOK;
}

FramesNeededMap
TimeFlow::getFramesNeeded(double time,
                          ViewIdx view)
{
    FramesNeededMap ret;
    double before, position;

    splitTime(sourceTime(time), &before, &position);
    const int method = _imp->method.lock()->getValue();
    RangeD range;
    range.min = before;
    range.max = position == 0. ? before : before + 1.;
    const bool ownVectors = !getInput(1);
    if ( (method == eMethodMotion) && (position != 0.) && ownVectors && (_imp->temporalSmoothing.lock()->getValue() > 0.) ) {
        range.min -= 1.;
        range.max += 1.;
    }
    {
        std::vector<RangeD> ranges;
        ranges.push_back(range);
        FrameRangesMap views;
        views.insert( std::make_pair(view, ranges) );
        if ( getInput(0) ) {
            ret.insert( std::make_pair(0, views) );
        }
    }
    if ( !ownVectors && (method == eMethodMotion) && (position != 0.) ) {
        // the vectors of the later frame describe the motion between the two
        RangeD vr;
        vr.min = vr.max = before + 1.;
        std::vector<RangeD> ranges;
        ranges.push_back(vr);
        FrameRangesMap views;
        views.insert( std::make_pair(view, ranges) );
        ret.insert( std::make_pair(1, views) );
    }
    if ( ownVectors && getInput(2) && (method == eMethodMotion) && (position != 0.) ) {
        // the matte of both source frames
        RangeD mr;
        mr.min = before;
        mr.max = before + 1.;
        std::vector<RangeD> ranges;
        ranges.push_back(mr);
        FrameRangesMap views;
        views.insert( std::make_pair(view, ranges) );
        ret.insert( std::make_pair(2, views) );
    }

    return ret;
}

StatusEnum
TimeFlow::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusFailed;
    }
    double before, position;
    splitTime(sourceTime(time), &before, &position);
    bool isProject;
    StatusEnum st = input->getRegionOfDefinition_public(input->getHash(), before, scale, view, rod, &isProject);
    if ( (st != eStatusOK) && (st != eStatusReplyDefault) ) {
        return st;
    }
    if (position != 0.) {
        RectD other;
        st = input->getRegionOfDefinition_public(input->getHash(), before + 1., scale, view, &other, &isProject);
        if ( (st == eStatusOK) || (st == eStatusReplyDefault) ) {
            rod->merge(other);
        }
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

// The first `nOut` channels of `img` over `bounds` as one block of floats (zero where the image
// has no data or fewer channels).
void
toBuffer(const ImagePtr& img,
         const RectI& bounds,
         int nOut,
         std::vector<float>* out)
{
    const int W = bounds.width(), H = bounds.height();

    out->assign( (std::size_t)W * H * nOut, 0.f );
    if (!img) {
        return;
    }
    Image::ReadAccess ra( img.get() );
    const RectI ib = img->getBounds();
    const int nComp = img->getComponents().getNumComponents();
    const int n = std::min(nComp, nOut);
    for (int y = std::max(bounds.y1, ib.y1); y < std::min(bounds.y2, ib.y2); ++y) {
        float* o = &(*out)[(std::size_t)(y - bounds.y1) * W * nOut];
        for (int x = std::max(bounds.x1, ib.x1); x < std::min(bounds.x2, ib.x2); ++x) {
            const float* p = (const float*)ra.pixelAt(x, y);
            if (!p) {
                continue;
            }
            float* d = o + (std::size_t)(x - bounds.x1) * nOut;
            for (int c = 0; c < n; ++c) {
                d[c] = p[c];
            }
        }
    }
}
} // namespace

// The vector field from the frame at `time` to the frame at `otherTime`, over `bounds`. Returns false
// when either frame is missing or the render was aborted.
bool
TimeFlow::fetchMatte(double time,
                     const RenderActionArgs& args,
                     const RectI& bounds,
                     std::vector<float>* matte)
{
    RectI roi;
    ImagePtr img = getImage(2, time, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roi);

    if (!img) {
        return false;
    }
    const int W = bounds.width(), H = bounds.height();
    matte->assign( (std::size_t)W * H, 0.f );
    Image::ReadAccess ra( img.get() );
    const RectI ib = img->getBounds();
    const int nComp = img->getComponents().getNumComponents();
    // the alpha of an RGBA picture, otherwise the first channel
    const int c = nComp >= 4 ? 3 : 0;
    for (int y = std::max(bounds.y1, ib.y1); y < std::min(bounds.y2, ib.y2); ++y) {
        float* o = &(*matte)[(std::size_t)(y - bounds.y1) * W];
        for (int x = std::max(bounds.x1, ib.x1); x < std::min(bounds.x2, ib.x2); ++x) {
            const float* p = (const float*)ra.pixelAt(x, y);
            if (p) {
                o[x - bounds.x1] = p[c];
            }
        }
    }

    return true;
}

bool
TimeFlow::computeField(double time,
                       double otherTime,
                       const RenderActionArgs& args,
                       const RectI& bounds,
                       std::vector<float>* flow,
                       std::vector<float>* background)
{
    const U64 hash = getHash();
    const bool layered = background != 0;
    const double scale = args.mappedScale.toOfxPointD().x;
    {
        QMutexLocker l(&_imp->fieldsMutex);
        for (std::list<TimeFlowPrivate::CachedField>::iterator it = _imp->fields.begin(); it != _imp->fields.end(); ++it) {
            if ( (it->hash == hash) && (it->time == time) && (it->otherTime == otherTime) && (it->scale == scale)
                 && (it->bounds.x1 == bounds.x1) && (it->bounds.y1 == bounds.y1) && (it->bounds.x2 == bounds.x2) && (it->bounds.y2 == bounds.y2)
                 && (it->layered == layered) ) {
                *flow = it->flow;
                if (layered) {
                    *background = it->background;
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
    std::vector<float> a, b;
    toMatchImage(imgA, bounds, weights, &a);
    toMatchImage(imgB, bounds, weights, &b);
    imgA.reset();
    imgB.reset();
    FlowCore::Params p;
    p.vectorDetail = _imp->vectorDetail.lock()->getValue();
    p.smoothness = _imp->smoothness.lock()->getValue();
    p.meanNormalize = _imp->flickerCompensation.lock()->getValue();
    if (layered) {
        std::vector<float> ma, mb;
        if ( !fetchMatte(time, args, bounds, &ma) || !fetchMatte(otherTime, args, bounds, &mb) || aborted() ) {
            return false;
        }
        if ( !FlowCore::computeFlowLayered(&a[0], &b[0], &ma[0], &mb[0], W, H, p, flow, background) || aborted() ) {
            return false;
        }
    } else if ( !FlowCore::computeFlow(&a[0], &b[0], W, H, p, flow) || aborted() ) {
        return false;
    }
    {
        QMutexLocker l(&_imp->fieldsMutex);
        _imp->fields.push_front( TimeFlowPrivate::CachedField() );
        TimeFlowPrivate::CachedField& e = _imp->fields.front();
        e.hash = hash;
        e.time = time;
        e.otherTime = otherTime;
        e.scale = scale;
        e.bounds = bounds;
        e.layered = layered;
        e.flow = *flow;
        if (layered) {
            e.background = *background;
        }
        // two fields per pair of source frames, six with temporal smoothing; room for a pair and its neighbour
        while (_imp->fields.size() > 8) {
            _imp->fields.pop_back();
        }
    }

    return true;
} // computeField

StatusEnum
TimeFlow::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) {
        return eStatusFailed;
    }
    const RectI outBounds = outImg->getBounds();
    const int W = outBounds.width(), H = outBounds.height();
    const int outNComp = outImg->getComponents().getNumComponents();
    const std::size_t N = (std::size_t)W * H;
    double before, position;

    splitTime(sourceTime(args.time), &before, &position);
    int method = _imp->method.lock()->getValue();
    if (method == eMethodNearest) {
        // normally answered by isIdentity; kept for a render that arrives anyway
        before = position < 0.5 ? before : before + 1.;
        position = 0.;
    }
    const double after = before + 1.;

    RectI roiA, roiB;
    ImagePtr imgA = getImage(0, before, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roiA);
    ImagePtr imgB;
    if (position != 0.) {
        imgB = getImage(0, after, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roiB);
    }
    if ( aborted() ) {
        return eStatusFailed;
    }
    std::vector<float> a, b, out(N * outNComp, 0.f);
    toBuffer(imgA, outBounds, outNComp, &a);
    imgA.reset();
    if (position == 0.) {
        out.swap(a);
    } else {
        toBuffer(imgB, outBounds, outNComp, &b);
        imgB.reset();
        bool moved = false;
        if ( (method == eMethodMotion) && (W >= 8) && (H >= 8) ) {
            std::vector<float> ab, ba;
            bool haveFields = false;
            if ( getInput(1) ) {
                // supplied vectors: those of the later frame point back to the earlier one
                RectI roiV;
                ImagePtr vec = getImage(1, after, args.mappedScale, args.view, NULL, NULL, false, false, eStorageModeRAM, 0, &roiV);
                if (vec) {
                    std::vector<float> v2;
                    toBuffer(vec, outBounds, 2, &v2);
                    vec.reset();
                    // the vectors are in full-size pixels; a velocity pass points the other way
                    const float k = (float)args.mappedScale.toOfxPointD().x
                                    * (_imp->vectorKind.lock()->getValue() == eVectorKindVelocity ? -1.f : 1.f);
                    for (std::size_t i = 0; i < v2.size(); ++i) {
                        v2[i] *= k;
                    }
                    ba.swap(v2);
                    FlowCore::invertFlow(ba, W, H, &ab);
                    haveFields = true;
                }
            } else if ( getInput(2) ) {
                // a matte: the foreground and the background each get their own pair of fields
                std::vector<float> bgAB, bgBA, ma, mb;
                if ( computeField(before, after, args, outBounds, &ab, &bgAB) && computeField(after, before, args, outBounds, &ba, &bgBA)
                     && fetchMatte(before, args, outBounds, &ma) && fetchMatte(after, args, outBounds, &mb) && !aborted() ) {
                    FlowCore::interpolateFrameLayered(&a[0], &b[0], outNComp, W, H, ab, ba, bgAB, bgBA, &ma[0], &mb[0], position, &out[0]);
                    moved = true;
                }
            } else {
                haveFields = computeField(before, after, args, outBounds, &ab) && computeField(after, before, args, outBounds, &ba);
                const double temporal = _imp->temporalSmoothing.lock()->getValue();
                if ( haveFields && (temporal > 0.) && !aborted() ) {
                    // the same kind of field one frame further along each motion, and one frame back
                    std::vector<float> ahead, behind;
                    bool haveAhead = computeField(after, after + 1., args, outBounds, &ahead);
                    bool haveBehind = computeField(before - 1., before, args, outBounds, &behind);
                    FlowCore::temporalFilter(ab, haveAhead ? &ahead : NULL, haveBehind ? &behind : NULL, W, H, temporal, &ab);
                    haveAhead = computeField(before, before - 1., args, outBounds, &ahead);
                    haveBehind = computeField(after + 1., after, args, outBounds, &behind);
                    FlowCore::temporalFilter(ba, haveAhead ? &ahead : NULL, haveBehind ? &behind : NULL, W, H, temporal, &ba);
                }
            }
            if ( aborted() ) {
                return eStatusFailed;
            }
            if (haveFields) {
                std::vector<float> confA, confB;
                FlowCore::flowConsistency(ab, ba, W, H, &confA);
                FlowCore::flowConsistency(ba, ab, W, H, &confB);
                FlowCore::interpolateFrame(&a[0], &b[0], outNComp, W, H, ab, ba, confA, confB, position, &out[0]);
                moved = true;
            }
        }
        if (!moved) {
            // Blend, or the motion could not be worked out
            const float s = (float)position;
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = a[i] + (b[i] - a[i]) * s;
            }
        }
    }
    if ( aborted() ) {
        return eStatusFailed;
    }

    Image::WriteAccess wa( outImg.get() );
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        const float* src = &out[(std::size_t)(y - outBounds.y1) * W * outNComp];
        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) {
                continue;
            }
            const float* s = src + (std::size_t)(x - outBounds.x1) * outNComp;
            for (int c = 0; c < outNComp; ++c) {
                dst[c] = s[c];
            }
        }
    }

    return eStatusOK;
} // render

NATRON_NAMESPACE_EXIT
