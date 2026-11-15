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

#include "ReadSplats.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"
#include "SceneGraph.h"

NATRON_NAMESPACE_ENTER

struct ReadSplatsPrivate
{
    KnobFileWPtr filePath;
    KnobButtonWPtr reloadBtn;
    KnobChoiceWPtr shDegree;
    KnobStringWPtr info;

    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr uniformScale;
    KnobBoolWPtr yDown;

    KnobDoubleWPtr pointScale;
    KnobDoubleWPtr sizeClamp;
    KnobDoubleWPtr opacityThreshold;
    KnobIntWPtr viewportMaxPoints;
    KnobIntWPtr renderMaxPoints;
    KnobBoolWPtr linearize;

    KnobChoiceWPtr cropMode;
    KnobBoolWPtr cropInvert;
    KnobDoubleWPtr cropCenter[3], cropSize[3], cropRadius;
    KnobButtonWPtr cropFit;
    KnobDoubleWPtr minOpacity, maxSize;

    // Guards everything below. Held across the file read on purpose: a second
    // caller (viewport paint vs. render thread) waits for the one read instead of
    // starting its own.
    mutable std::mutex mutex;
    SplatDataPtr raw;            // the file as read
    SplatDataPtr filtered;       // after crop / cleanup; built only while a filter is active
    SplatFilter filter;          // what `filtered` was built with
    bool filteredValid = false;
    std::string loadedPath;
    int loadedShDegree = -1;
    bool loadAttempted = false;
    std::string lastError;
    // The file's own convention is Y up (.spz): the Y Down knob then does nothing.
    // Atomic because the scene graph asks for the transform on every rebuild and
    // must not wait on a file read.
    std::atomic<bool> fileYUp{false};
};

ReadSplats::ReadSplats(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadSplatsPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadSplats::~ReadSplats()
{
}

std::string
ReadSplats::getPluginDescription() const
{
    return tr("Read a 3D Gaussian splat scene (3DGS): the binary .ply written by the "
              "standard trainers (position, opacity, scale, rotation and spherical-harmonic "
              "colour per splat). Plain binary point-cloud .ply files load too.\n\n"
              "Connect it to a Scene3D, or straight to ScanlineRender's obj/scn input. "
              "The 3D viewport and ScanlineRender show a point preview: one opaque round "
              "point per splat, coloured with the splat's base colour and sized from its "
              "scale. That is enough to line a scan up against a plate, a camera or other "
              "geometry; it is not the final splat render.\n\n"
              "Scans come in the orientation of whatever made them, so use the Transform "
              "tab to stand the scene upright and scale it.").toStdString();
}

void
ReadSplats::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
}

void
ReadSplats::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadSplats::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadSplats::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("Splats") );

    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>( this, tr("File") );
        k->setName("filePath");
        k->setHintToolTip( tr("A 3D Gaussian splat scene. Formats: the binary .ply a 3DGS trainer writes "
                              "(usually point_cloud.ply); .splat (base colour only); .spz up to version 3. "
                              "A plain binary point-cloud .ply also loads, as points without sizes.") );
        page->addKnob(k);
        _imp->filePath = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>( this, tr("Reload") );
        k->setName("reload");
        k->setHintToolTip( tr("Read the file again, for when it changed on disk.") );
        page->addKnob(k);
        _imp->reloadBtn = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("SH Degree") );
        k->setName("shDegree");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("0", "0 (base colour)", "Keep one colour per splat. 56 bytes per splat in memory.") );
        entries.push_back( ChoiceOption("1", "1", "Keep the first band of view-dependent colour. 92 bytes per splat.") );
        entries.push_back( ChoiceOption("2", "2", "Keep two bands of view-dependent colour. 152 bytes per splat.") );
        entries.push_back( ChoiceOption("3", "3 (full)", "Keep all of the file's view-dependent colour. 236 bytes per splat.") );
        k->populateChoices(entries);
        k->setDefaultValue(3);
        k->setHintToolTip( tr("How much of the splats' view-dependent colour (spherical harmonics) to keep "
                              "in memory and on the graphics card. SplatRender uses it for highlights and "
                              "reflections that shift with the camera. The point preview in the 3D viewport "
                              "and ScanlineRender only uses the base colour, so for layout work, or when a "
                              "large scan does not fit in memory, 0 keeps it four times smaller. "
                              "Changing this reads the file again.") );
        page->addKnob(k);
        _imp->shDegree = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>( this, tr("Info") );
        k->setName("info");
        k->setAsLabel();
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);
        k->setIsPersistent(false);
        k->setDefaultValue("Set File to a 3DGS .ply file.");
        page->addKnob(k);
        _imp->info = k;
    }

    // ---- Transform ----
    KnobPagePtr xfPage = AppManager::createKnob<KnobPage>( this, tr("Transform") );
    struct Axis { const char* name; const char* label; KnobDoubleWPtr* dst; double def; bool newLine; };
    const Axis axes[] = {
        {"translateX", "Translate X", &_imp->translateX, 0., true},
        {"translateY", "Y",           &_imp->translateY, 0., false},
        {"translateZ", "Z",           &_imp->translateZ, 0., false},
        {"rotateX",    "Rotate X",    &_imp->rotateX,    0., true},
        {"rotateY",    "Y",           &_imp->rotateY,    0., false},
        {"rotateZ",    "Z",           &_imp->rotateZ,    0., false},
    };
    for (std::size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr(axes[i].label) );
        k->setName(axes[i].name);
        k->setDefaultValue(axes[i].def);
        k->setAnimationEnabled(true);
        const bool isRot = (i >= 3);
        k->setDisplayMinimum(isRot ? -180. : -10.);
        k->setDisplayMaximum(isRot ? 180. : 10.);
        // Three per row: the next knob stays on this row unless it starts a new group.
        const bool nextStartsRow = (i + 1 >= sizeof(axes) / sizeof(axes[0])) || axes[i + 1].newLine;
        k->setAddNewLine(nextStartsRow);
        xfPage->addKnob(k);
        *axes[i].dst = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Uniform Scale") );
        k->setName("uniformScale");
        k->setDefaultValue(1.);
        k->setMinimum(1e-6);
        k->setDisplayMinimum(0.01);
        k->setDisplayMaximum(10.);
        k->setAnimationEnabled(true);
        k->setHintToolTip( tr("Scale of the whole scene. A trained scan has no real-world unit; scale it "
                              "until it matches the rest of the 3D scene.") );
        xfPage->addKnob(k);
        _imp->uniformScale = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Y Down (COLMAP)") );
        k->setName("yDown");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Scenes trained from COLMAP camera data have Y pointing down and Z pointing "
                              "away from the camera. On: turn the scene half a turn around X so it is the "
                              "right way up here (Y up). Turn it off if the scene comes in upside down. "
                              "Applied before Rotate. Ignored for .spz files, which are stored Y up.") );
        xfPage->addKnob(k);
        _imp->yDown = k;
    }

    // ---- Preview ----
    KnobPagePtr pvPage = AppManager::createKnob<KnobPage>( this, tr("Preview") );
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Point Scale") );
        k->setName("pointScale");
        k->setDefaultValue(2.0);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.1);
        k->setDisplayMaximum(5.);
        k->setAnimationEnabled(true);
        k->setHintToolTip( tr("Size of the preview points, as a multiple of each splat's own size. "
                              "Raise it to close the gaps between points, lower it for a crisper, "
                              "sparser cloud.") );
        pvPage->addKnob(k);
        _imp->pointScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Size Limit") );
        k->setName("sizeClamp");
        k->setDefaultValue(4.);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(20.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("No point is drawn larger than this many times the typical (median) splat. "
                              "A real scan has a few enormous, faint splats for sky and far background; "
                              "drawn as solid points they would cover everything. 0 = no limit.") );
        pvPage->addKnob(k);
        _imp->sizeClamp = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Opacity Threshold") );
        k->setName("opacityThreshold");
        k->setDefaultValue(0.1);
        k->setMinimum(0.);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Splats more transparent than this are left out of the preview. The points "
                              "are drawn solid, so faint splats (haze, floaters) would otherwise look like "
                              "solid clutter.") );
        pvPage->addKnob(k);
        _imp->opacityThreshold = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Viewport Max Points") );
        k->setName("viewportMaxPoints");
        k->setDefaultValue(4000000);
        k->setMinimum(0);
        k->setDisplayMinimum(0);
        k->setDisplayMaximum(10000000);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Most points the 3D viewport draws for this node. A larger scene is thinned "
                              "evenly. 20 bytes of graphics memory per point. 0 = draw every splat.") );
        pvPage->addKnob(k);
        _imp->viewportMaxPoints = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Render Max Points") );
        k->setName("renderMaxPoints");
        k->setDefaultValue(0);
        k->setMinimum(0);
        k->setDisplayMinimum(0);
        k->setDisplayMaximum(10000000);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Most points ScanlineRender draws for this node. A larger scene is thinned "
                              "evenly. 0 = draw every splat.") );
        pvPage->addKnob(k);
        _imp->renderMaxPoints = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("sRGB to Linear") );
        k->setName("linearize");
        k->setDefaultValue(true);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Splat colours are those of the photographs the scene was trained on, "
                              "normally sRGB. On: ScanlineRender converts them to linear, like every other "
                              "image in the comp. Turn it off for a scene trained on linear images. The 3D "
                              "viewport always shows the colours as stored.") );
        pvPage->addKnob(k);
        _imp->linearize = k;
    }

    // ---- Crop / cleanup: applies to everything downstream (viewport, ScanlineRender, SplatRender)
    KnobPagePtr cropPage = AppManager::createKnob<KnobPage>( this, tr("Crop") );
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Crop") );
        k->setName("cropMode");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back( ChoiceOption("Off", "Off", "Keep the whole scene.") );
        entries.push_back( ChoiceOption("Box", "Box", "Keep the splats whose centre is inside the box.") );
        entries.push_back( ChoiceOption("Sphere", "Sphere", "Keep the splats whose centre is inside the sphere.") );
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setHintToolTip( tr("Cut the scene down to a box or a sphere: a real scan comes with floor, "
                              "walls and floating specks around the subject. The shape is drawn in the "
                              "3D viewport. It is defined in the scene's own upright space, so it moves "
                              "with the scene when the Transform tab changes. Splats are kept or dropped "
                              "whole, by their centre.") );
        cropPage->addKnob(k);
        _imp->cropMode = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Invert") );
        k->setName("cropInvert");
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Keep what is outside the shape instead: cut a hole in the scene.") );
        cropPage->addKnob(k);
        _imp->cropInvert = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>( this, tr("Fit to Scene") );
        k->setName("cropFit");
        k->setHintToolTip( tr("Set the box (and the sphere) around the bulk of the scene, leaving out "
                              "the far-away 2% on each side, and turn the crop on if it was off. A good "
                              "starting point to then tighten by hand.") );
        cropPage->addKnob(k);
        _imp->cropFit = k;
    }
    struct CropAxis { const char* name; const char* label; KnobDoubleWPtr* dst; double def; bool newLine; };
    const CropAxis cropAxes[] = {
        {"cropCenterX", "Center X", &_imp->cropCenter[0], 0., true},
        {"cropCenterY", "Y",        &_imp->cropCenter[1], 0., false},
        {"cropCenterZ", "Z",        &_imp->cropCenter[2], 0., false},
        {"cropSizeX",   "Size X",   &_imp->cropSize[0],   2., true},
        {"cropSizeY",   "Y",        &_imp->cropSize[1],   2., false},
        {"cropSizeZ",   "Z",        &_imp->cropSize[2],   2., false},
    };
    for (std::size_t i = 0; i < sizeof(cropAxes) / sizeof(cropAxes[0]); ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr(cropAxes[i].label) );
        k->setName(cropAxes[i].name);
        k->setDefaultValue(cropAxes[i].def);
        k->setAnimationEnabled(false);
        const bool isSize = (i >= 3);
        if (isSize) k->setMinimum(0.);
        k->setDisplayMinimum(isSize ? 0. : -10.);
        k->setDisplayMaximum(10.);
        const bool nextStartsRow = (i + 1 >= sizeof(cropAxes) / sizeof(cropAxes[0])) || cropAxes[i + 1].newLine;
        k->setAddNewLine(nextStartsRow);
        if (i == 0) k->setHintToolTip( tr("Middle of the box or sphere, in the scene's upright space (before the Transform tab).") );
        if (i == 3) k->setHintToolTip( tr("Full size of the box along each axis.") );
        cropPage->addKnob(k);
        *cropAxes[i].dst = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Radius") );
        k->setName("cropRadius");
        k->setDefaultValue(1.);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(10.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Radius of the sphere (Crop = Sphere).") );
        cropPage->addKnob(k);
        _imp->cropRadius = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Min Opacity") );
        k->setName("minOpacity");
        k->setDefaultValue(0.);
        k->setMinimum(0.);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(1.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Remove splats more transparent than this from the scene altogether. A "
                              "small value (0.02 to 0.1) clears much of the haze and specks a scan "
                              "carries. Unlike Opacity Threshold on the Preview tab, this also changes "
                              "what SplatRender draws.") );
        cropPage->addKnob(k);
        _imp->minOpacity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Max Size") );
        k->setName("maxSize");
        k->setDefaultValue(0.);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(100.);
        k->setAnimationEnabled(false);
        k->setHintToolTip( tr("Remove splats larger than this many times the typical (median) splat. "
                              "The few very large splats of a scan are usually sky, far background or "
                              "errors that show as smears. 0 = keep all; try 20 to 50.") );
        cropPage->addKnob(k);
        _imp->maxSize = k;
    }
}

SplatDataPtr
ReadSplats::getSplatData()
{
    KnobFilePtr fileKnob = _imp->filePath.lock();
    KnobChoicePtr degKnob = _imp->shDegree.lock();
    // Read the knobs before taking the mutex: knob access has its own locks.
    const std::string path = fileKnob ? fileKnob->getValue() : std::string();
    const int degree = degKnob ? degKnob->getValue() : 3;
    SplatFilter want;
    {
        KnobChoicePtr cm = _imp->cropMode.lock();
        want.cropMode = cm ? cm->getValue() : 0;
        KnobBoolPtr inv = _imp->cropInvert.lock();
        want.invert = inv && inv->getValue();
        for (int k = 0; k < 3; ++k) {
            KnobDoublePtr c = _imp->cropCenter[k].lock(), z = _imp->cropSize[k].lock();
            want.center[k] = c ? (float)c->getValue() : 0.f;
            want.size[k] = z ? (float)z->getValue() : 2.f;
        }
        KnobDoublePtr r = _imp->cropRadius.lock(), mo = _imp->minOpacity.lock(), ms = _imp->maxSize.lock();
        want.radius = r ? (float)r->getValue() : 1.f;
        want.minOpacity = mo ? (float)mo->getValue() : 0.f;
        want.maxSize = ms ? (float)ms->getValue() : 0.f;
    }
    KnobBoolPtr yd = _imp->yDown.lock();
    const bool yDownKnob = yd && yd->getValue();

    std::lock_guard<std::mutex> lk(_imp->mutex);
    if (!_imp->loadAttempted || path != _imp->loadedPath || degree != _imp->loadedShDegree) {
        _imp->loadAttempted = true;
        _imp->loadedPath = path;
        _imp->loadedShDegree = degree;
        _imp->raw.reset();        // free the old scene before reading the new one
        _imp->filtered.reset();
        _imp->filteredValid = false;
        _imp->lastError.clear();
        _imp->fileYUp = false;
        if ( !path.empty() ) {
            _imp->raw = readSplatFile(path, degree, &_imp->lastError);
            if (_imp->raw) {
                _imp->fileYUp = _imp->raw->yUp;
            } else {
                std::fprintf( stderr, "[ReadSplats] %s: %s\n",
                              getNode() ? getNode()->getScriptName_mt_safe().c_str() : "?", _imp->lastError.c_str() );
                std::fflush(stderr);
            }
        }
    }
    if (!_imp->raw) {
        return SplatDataPtr();
    }
    want.flipYZ = yDownKnob && !_imp->raw->yUp;
    if ( !want.active() ) {
        _imp->filtered.reset();
        _imp->filteredValid = false;
        _imp->lastError.clear();
        return _imp->raw;
    }
    if ( !_imp->filteredValid || !(want == _imp->filter) ) {
        _imp->filtered = filterSplats(*_imp->raw, want);
        _imp->filter = want;
        _imp->filteredValid = true;
        _imp->lastError = _imp->filtered ? std::string()
                                         : std::string("The Crop tab leaves no splats: widen the crop, or lower Min Opacity / raise Max Size.");
    }
    return _imp->filtered;
}

void
ReadSplats::getCropShape(int* mode, float center[3], float size[3], float* radius, bool* flipYZ) const
{
    KnobChoicePtr cm = _imp->cropMode.lock();
    *mode = cm ? cm->getValue() : 0;
    for (int k = 0; k < 3; ++k) {
        KnobDoublePtr c = _imp->cropCenter[k].lock(), z = _imp->cropSize[k].lock();
        center[k] = c ? (float)c->getValue() : 0.f;
        size[k] = z ? (float)z->getValue() : 2.f;
    }
    KnobDoublePtr r = _imp->cropRadius.lock();
    *radius = r ? (float)r->getValue() : 1.f;
    KnobBoolPtr yd = _imp->yDown.lock();
    *flipYZ = yd && yd->getValue() && !_imp->fileYUp.load();
}

std::string
ReadSplats::getLastLoadError() const
{
    std::lock_guard<std::mutex> lk(_imp->mutex);
    return _imp->lastError;
}

void
ReadSplats::refreshInfo()
{
    KnobStringPtr info = _imp->info.lock();
    if (!info) return;
    SplatDataPtr d = getSplatData();
    std::ostringstream os;
    if (!d) {
        const std::string err = getLastLoadError();
        os << ( err.empty() ? std::string("Set File to a 3DGS .ply file.") : err );
    } else {
        std::size_t total = d->count;
        {
            std::lock_guard<std::mutex> lk(_imp->mutex);
            if (_imp->raw) total = _imp->raw->count;
        }
        os << d->count;
        if (total != d->count) os << " of " << total;
        os << (d->hasGaussians ? " splats" : " points (plain point cloud, no splat data)");
        if (total != d->count) os << " kept by the Crop tab";
        if (d->hasGaussians) {
            os << ", SH degree " << d->fileShDegree << " in the file, " << d->shDegree << " kept";
        }
        os << ", " << ( ( d->bytesInMemory() + (1u << 19) ) >> 20 ) << " MB";
        if (d->droppedNonFinite) os << ", " << d->droppedNonFinite << " invalid skipped";
    }
    info->setValue( os.str() );
}

bool
ReadSplats::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                        double /*time*/, bool /*originatedFromMainThread*/)
{
    if ( k == _imp->filePath.lock().get() || k == _imp->shDegree.lock().get() ) {
        refreshInfo();    // getSplatData notices the new path / degree and reads the file
        if ( getApp() ) getApp()->redrawAllViewers();
        return true;
    }
    if ( k == _imp->cropFit.lock().get() ) {
        SplatDataPtr raw;
        {
            // make sure the file is read, then take the unfiltered scene
            (void)getSplatData();
            std::lock_guard<std::mutex> lk(_imp->mutex);
            raw = _imp->raw;
        }
        if (raw) {
            KnobBoolPtr yd = _imp->yDown.lock();
            const bool flip = yd && yd->getValue() && !raw->yUp;
            float c[3], z[3];
            robustSplatBounds(*raw, flip, c, z);
            beginChanges();
            for (int a = 0; a < 3; ++a) {
                // 4 significant digits: the box is a starting point, not a measurement
                const double mag = std::pow( 10., std::floor( std::log10( std::max(1e-9f, z[a]) ) ) - 3. );
                if ( KnobDoublePtr kc = _imp->cropCenter[a].lock() ) kc->setValue(std::round(c[a] / mag) * mag);
                if ( KnobDoublePtr kz = _imp->cropSize[a].lock() ) kz->setValue(std::round(z[a] / mag) * mag);
            }
            if ( KnobDoublePtr kr = _imp->cropRadius.lock() ) {
                const double rad = 0.5 * std::max( z[0], std::max(z[1], z[2]) );
                const double mag = std::pow( 10., std::floor( std::log10( std::max(1e-9, rad) ) ) - 3. );
                kr->setValue(std::round(rad / mag) * mag);
            }
            KnobChoicePtr cm = _imp->cropMode.lock();
            if (cm && cm->getValue() == 0) cm->setValue(1);
            endChanges();
        }
        refreshInfo();
        if ( getApp() ) getApp()->redrawAllViewers();
        return true;
    }
    {
        bool isCrop = k == _imp->cropMode.lock().get() || k == _imp->cropInvert.lock().get()
                      || k == _imp->cropRadius.lock().get() || k == _imp->minOpacity.lock().get()
                      || k == _imp->maxSize.lock().get() || k == _imp->yDown.lock().get();
        for (int a = 0; a < 3 && !isCrop; ++a) {
            isCrop = k == _imp->cropCenter[a].lock().get() || k == _imp->cropSize[a].lock().get();
        }
        if (isCrop) {
            refreshInfo();
            if ( getApp() ) getApp()->redrawAllViewers();
            return false;      // still evaluates normally
        }
    }
    if ( k == _imp->reloadBtn.lock().get() ) {
        {
            std::lock_guard<std::mutex> lk(_imp->mutex);
            _imp->loadAttempted = false;
        }
        refreshInfo();
        if ( getApp() ) getApp()->redrawAllViewers();
        return true;
    }
    return false;
}

void
ReadSplats::onKnobsLoaded()
{
    // A project load restores the file knob without firing knobChanged.
    refreshInfo();
}

void
ReadSplats::getSplatTransform(double time, float out[16]) const
{
    auto val = [time](const KnobDoubleWPtr& w, double def) -> float {
        KnobDoublePtr k = w.lock();
        return k ? (float)k->getValueAtTime(time) : (float)def;
    };
    const float s = val(_imp->uniformScale, 1.);
    float trs[16];
    SceneGraph::buildTRS(val(_imp->translateX, 0.), val(_imp->translateY, 0.), val(_imp->translateZ, 0.),
                         val(_imp->rotateX, 0.), val(_imp->rotateY, 0.), val(_imp->rotateZ, 0.),
                         s, s, s, trs);
    KnobBoolPtr yd = _imp->yDown.lock();
    if ( yd && yd->getValue() && !_imp->fileYUp.load() ) {
        // out = TRS * flip, flip = half a turn around X = diag(1, -1, -1): negate columns 1 and 2.
        for (int r = 0; r < 4; ++r) {
            trs[4 + r] = -trs[4 + r];
            trs[8 + r] = -trs[8 + r];
        }
    }
    std::memcpy( out, trs, 16 * sizeof(float) );
}

float
ReadSplats::getSplatWorldScale(double time) const
{
    KnobDoublePtr k = _imp->uniformScale.lock();
    return k ? (float)k->getValueAtTime(time) : 1.f;
}

float
ReadSplats::getPreviewPointScale(double time) const
{
    KnobDoublePtr k = _imp->pointScale.lock();
    return k ? (float)k->getValueAtTime(time) : 2.f;
}

float
ReadSplats::getPreviewSizeClamp() const
{
    KnobDoublePtr k = _imp->sizeClamp.lock();
    return k ? (float)k->getValue() : 4.f;
}

float
ReadSplats::getPreviewOpacityThreshold() const
{
    KnobDoublePtr k = _imp->opacityThreshold.lock();
    return k ? (float)k->getValue() : 0.1f;
}

std::size_t
ReadSplats::getViewportMaxPoints() const
{
    KnobIntPtr k = _imp->viewportMaxPoints.lock();
    return k ? (std::size_t)std::max( 0, k->getValue() ) : 0;
}

std::size_t
ReadSplats::getRenderMaxPoints() const
{
    KnobIntPtr k = _imp->renderMaxPoints.lock();
    return k ? (std::size_t)std::max( 0, k->getValue() ) : 0;
}

bool
ReadSplats::getPreviewLinearize() const
{
    KnobBoolPtr k = _imp->linearize.lock();
    return k ? k->getValue() : true;
}

StatusEnum
ReadSplats::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                                  ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadSplats::render(const RenderActionArgs& args)
{
    // A 3D source has no picture of its own: consumers call getSplatData().
    if ( !args.outputPlanes.empty() ) {
        ImagePtr outImg = args.outputPlanes.front().second;
        if (outImg) outImg->fillZero(args.roi);
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadSplats.cpp"
