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

#include "ReadVDB.h"

#include <cmath>
#include <algorithm>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <fstream>
#include <memory>

#include <iostream>

#include <QtCore/QDir>          // sequence detection — see detectSequence
#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtCore/QStringList>

#ifdef NATRON_HAVE_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/tools/Dense.h>
#include <openvdb/tools/Interpolation.h>  // BoxSampler (trilinear) for viewport preview
#endif

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../NodeMetadata.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct ReadVDBPrivate
{
    // Guards all cached state below (gridNames, cachedData, bounds cache):
    // loadVDBFile runs on the GUI thread while getVolumeData/getVDBBounds are
    // called from render workers and the 3D-viewport paint. Held across the
    // whole load/query so two threads can't double-load or copy a vector
    // another thread is refilling.
    std::mutex vdbMutex;

    KnobFileWPtr filePath;
    KnobChoiceWPtr gridName;
    KnobIntWPtr maxResolution;
    KnobDoubleWPtr density;
    KnobDoubleWPtr colorR, colorG, colorB;
    KnobDoubleWPtr absorptionR, absorptionG, absorptionB;
    KnobIntWPtr frameOffset;
    KnobIntWPtr viewportDisplayRes;   // 3D-viewport splat preview resolution (preview only)

    // Render settings
    KnobDoubleWPtr stepSize;
    KnobIntWPtr volumeBounces;
    KnobDoubleWPtr anisotropy;
    KnobDoubleWPtr blackbodyIntensity;
    KnobDoubleWPtr blackbodyTintR, blackbodyTintG, blackbodyTintB;
    KnobDoubleWPtr temperatureScale;

    // Remap curves
    KnobParametricWPtr densityRemap;
    KnobParametricWPtr temperatureRemap;

    // Grid bindings
    KnobStringWPtr bindDensity, bindTemperature, bindFlame, bindColor, bindVelocity;

    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Cached data
    std::string lastLoadedPath;
    std::string lastLoadedFramePath;
    std::vector<std::string> gridNames;
    ReadVDB::VDBVolumeData cachedData;
    bool hasCachedData;

    // Cached bbox (keyed by resolved frame path)
    std::string lastBoundsPath;
    float bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ;
    bool hasCachedBounds;

    // Is the chosen file part of a real, on-disk sequence? Decided once per file
    // pick (loadVDBFile). false = single frame: never substitute a frame number
    // into the name, however many digits it ends in. A one-off `smoke_0000.vdb`
    // used to be read as `smoke_<currentFrame>.vdb` and fail everywhere but the
    // viewport preview (which silently falls back to the un-substituted name).
    bool isSequence;

    // Why the last read failed (resolved path + OpenVDB exception). Surfaced to
    // the user through getLastLoadError(); empty after a successful read.
    std::string lastError;

    ReadVDBPrivate()
    : hasCachedData(false)
    , bMinX(-1), bMinY(-1), bMinZ(-1), bMaxX(1), bMaxY(1), bMaxZ(1)
    , hasCachedBounds(false)
    , isSequence(false)
    {}
};

// Locate the frame-number digit group in a VDB filename: the last run of digits
// before the .vdb extension. Returns false when there is none (e.g. "smoke.vdb").
static bool
findFrameDigits(const std::string& path, size_t& numStart, size_t& numEnd)
{
    size_t dotVdb = path.rfind(".vdb");
    if (dotVdb == std::string::npos) dotVdb = path.rfind(".VDB");
    if (dotVdb == std::string::npos) return false;

    // Search backwards from the extension for digits, stepping over a separating
    // dot first (e.g. "smoke.0050.vdb" → "0050").
    numEnd = dotVdb;
    while (numEnd > 0 && path[numEnd - 1] == '.') --numEnd;
    numStart = numEnd;
    while (numStart > 0 && path[numStart - 1] >= '0' && path[numStart - 1] <= '9') --numStart;

    return numStart != numEnd;
}

// Does this file look like one frame of an on-disk sequence? Counts siblings
// matching <prefix><same number of digits><suffix>; two or more = a sequence.
//
// Why this exists: the frame substitution below is destructive, so applying it
// to a file that ISN'T a sequence breaks single-frame VDBs — a one-off
// "smoke_0000.vdb" got read as "smoke_<currentFrame>.vdb", and a versioned
// "fire_v003.vdb" as "fire_v001.vdb". Deciding sequence-ness from what is
// actually on disk fixes that at the root, rather than papering over the missing
// file with a fallback (which would silently render the wrong frame of a real
// sequence — see getVDBBounds, which does exactly that for preview purposes).
//
// Compared on FILE NAMES, never full paths: the knob may hold backslashes while
// Qt hands back forward slashes, and a prefix compare would then never match.
static bool
detectSequence(const std::string& path)
{
    const QFileInfo fi( QString::fromUtf8( path.c_str() ) );
    const std::string fileName = fi.fileName().toStdString();

    size_t numStart = 0, numEnd = 0;
    if ( !findFrameDigits(fileName, numStart, numEnd) ) {
        return false;   // no digits at all -> definitively a single frame
    }

    const std::string prefix = fileName.substr(0, numStart);
    const std::string suffix = fileName.substr(numEnd);
    const int padding = (int)(numEnd - numStart);

    QDir dir = fi.absoluteDir();
    if ( !dir.exists() ) return false;

    const QString qPrefix = QString::fromUtf8( prefix.c_str() );
    const QString qSuffix = QString::fromUtf8( suffix.c_str() );
    QStringList filter;
    filter << (qPrefix + QString::fromUtf8("*") + qSuffix);

    int matches = 0;
    const QStringList entries = dir.entryList(filter, QDir::Files);
    for (const QString& name : entries) {
        const int digitsLen = name.size() - qPrefix.size() - qSuffix.size();
        if (digitsLen != padding) continue;          // different padding = different sequence
        bool allDigits = true;
        for (int i = 0; i < padding; ++i) {
            if ( !name.at(qPrefix.size() + i).isDigit() ) { allDigits = false; break; }
        }
        if (allDigits && ++matches > 1) return true; // two frames is enough
    }
    return false;
}

// Resolve frame number in a VDB filename
// e.g. "smoke.0050.vdb" at frame 10 with offset 0 → "smoke.0010.vdb"
static std::string
resolveFramePath(const std::string& templatePath, int frame)
{
    std::string path = templatePath;
    size_t numStart = 0, numEnd = 0;
    if ( !findFrameDigits(path, numStart, numEnd) ) return path;

    int padding = (int)(numEnd - numStart);
    if (padding < 1) padding = 4;

    // Build the frame number string with padding
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(padding) << frame;

    return path.substr(0, numStart) + ss.str() + path.substr(numEnd);
}

std::string
ReadVDB::resolvePathAtTime(double time) const
{
    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (!fileKnob) return std::string();
    const std::string templatePath = fileKnob->getValue();
    if ( templatePath.empty() ) return templatePath;

    // Single frame — the name is the name, whatever digits it ends in.
    if (!_imp->isSequence) return templatePath;

    KnobIntPtr offKnob = _imp->frameOffset.lock();
    int frame = (int)time + (offKnob ? offKnob->getValueAtTime(time) : 0);
    if (frame < 0) frame = 0;
    return resolveFramePath(templatePath, frame);
}

std::string
ReadVDB::getLastLoadError() const
{
    std::lock_guard<std::mutex> lk(_imp->vdbMutex);
    return _imp->lastError;
}


ReadVDB::ReadVDB(NodePtr node)
    : EffectInstance(node)
    , _imp(new ReadVDBPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ReadVDB::~ReadVDB()
{
}

std::string
ReadVDB::getPluginDescription() const
{
    return tr("Read OpenVDB (.vdb) files for volumetric rendering.\n\n"
              "Loads a density grid from a .vdb file and converts it to a 3D texture "
              "for GPU ray marching in ScanlineRender.\n\n"
              "Supports grids: density, temperature, or any scalar float grid.\n\n"
              "Connect to ScanlineRender's obj/scn input with a Camera3D.").toStdString();
}

void
ReadVDB::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
ReadVDB::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ReadVDB::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ReadVDB::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("VDB"));

    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("File"));
        k->setName("filePath");
        mainPage->addKnob(k);
        _imp->filePath = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Grid"));
        k->setName("gridName");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("density", "density", "Density grid"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        mainPage->addKnob(k);
        _imp->gridName = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Max Resolution"));
        k->setName("maxResolution"); k->setDefaultValue(64);
        k->setMinimum(8); k->setDisplayMinimum(16); k->setDisplayMaximum(256);
        mainPage->addKnob(k);
        _imp->maxResolution = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Density"));
        k->setName("density"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        mainPage->addKnob(k);
        _imp->density = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Offset"));
        k->setName("frameOffset"); k->setDefaultValue(0);
        // Wide enough to reach a real shot offset by dragging: a timeline
        // starting at 1001 over a sequence numbered from 0000 needs -1000, which
        // the old +/-100 slider range couldn't express (typing worked, dragging
        // didn't, so the knob read as broken).
        k->setDisplayMinimum(-2000); k->setDisplayMaximum(2000);
        k->setHintToolTip(tr("Offset applied to the current frame when reading a VDB "
                             "SEQUENCE: file frame = current frame + offset. A timeline "
                             "starting at 1001 reading a sequence numbered from 0000 needs "
                             "-1000. Ignored for single-frame VDBs — those are read exactly "
                             "as named, whatever digits the filename ends in."));
        mainPage->addKnob(k);
        _imp->frameOffset = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Viewport Display Res"));
        k->setName("viewportDisplayRes"); k->setDefaultValue(40);
        k->setMinimum(8); k->setMaximum(96);
        k->setDisplayMinimum(8); k->setDisplayMaximum(96);
        k->setHintToolTip(tr("Resolution of the 3D-viewport splat preview (samples along "
                             "the longest axis). Higher = finer/denser preview but more "
                             "splats and a slightly longer re-sample when scrubbing. Does "
                             "NOT affect the render — preview only."));
        k->setEvaluateOnChange(false);   // preview-only: don't trigger a re-render
        mainPage->addKnob(k);
        _imp->viewportDisplayRes = k;
    }

    // Transform
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate X"));
        k->setName("translateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Y"));
        k->setName("translateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Translate Z"));
        k->setName("translateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        xformPage->addKnob(k); _imp->translateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate X"));
        k->setName("rotateX"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Y"));
        k->setName("rotateY"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Rotate Z"));
        k->setName("rotateZ"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setDisplayMinimum(-180.0); k->setDisplayMaximum(180.0);
        xformPage->addKnob(k); _imp->rotateZ = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale X"));
        k->setName("scaleX"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleX = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Y"));
        k->setName("scaleY"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleY = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scale Z"));
        k->setName("scaleZ"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        xformPage->addKnob(k); _imp->scaleZ = k;
    }

    KnobPagePtr colorPage = AppManager::createKnob<KnobPage>(this, tr("Color"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scatter Color R"));
        k->setName("colorR"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Smoke scatter color. Low values = dark smoke."));
        colorPage->addKnob(k); _imp->colorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scatter Color G"));
        k->setName("colorG"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Scatter Color B"));
        k->setName("colorB"); k->setDefaultValue(0.1); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorB = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Absorption Color R"));
        k->setName("absorptionR"); k->setDefaultValue(0.02); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Light absorption color. Low values = smoke absorbs most light (dark)."));
        colorPage->addKnob(k); _imp->absorptionR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Absorption Color G"));
        k->setName("absorptionG"); k->setDefaultValue(0.02); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->absorptionG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Absorption Color B"));
        k->setName("absorptionB"); k->setDefaultValue(0.025); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->absorptionB = k;
    }
    // Density remap curve
    {
        KnobParametricPtr k = AppManager::createKnob<KnobParametric>(this, tr("Density Remap"), 1, false);
        k->setName("densityRemap");
        k->setParametricRange(0.0, 1.0);
        k->setCurveColor(0, 0.8, 0.8, 0.8);
        k->setHintToolTip(tr("Remap density values. X = input density (0-1), Y = output density. "
                              "Default is linear (no change). Use to sharpen edges or clamp low values."));
        // Default: linear identity curve (0,0) → (1,1)
        k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 0.0, 0.0, eKeyframeTypeLinear);
        k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 1.0, 1.0, eKeyframeTypeLinear);
        k->setDefaultCurvesFromCurves();
        colorPage->addKnob(k);
        _imp->densityRemap = k;
    }

    // Fire / Blackbody
    KnobPagePtr firePage = AppManager::createKnob<KnobPage>(this, tr("Fire"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blackbody Intensity"));
        k->setName("blackbodyIntensity"); k->setDefaultValue(0.005); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(0.1);
        k->setHintToolTip(tr("Fire rendering intensity. 0 = smoke only. Very small values (0.001-0.01) for EmberGen/Houdini VDBs."));
        firePage->addKnob(k); _imp->blackbodyIntensity = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Temperature Scale"));
        k->setName("temperatureScale"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Temperature multiplier for blackbody color. EmberGen VDBs have high values, use low scale (0.1-10)."));
        firePage->addKnob(k); _imp->temperatureScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blackbody Tint R"));
        k->setName("blackbodyTintR"); k->setDefaultValue(1.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        firePage->addKnob(k); _imp->blackbodyTintR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blackbody Tint G"));
        k->setName("blackbodyTintG"); k->setDefaultValue(0.7);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        firePage->addKnob(k); _imp->blackbodyTintG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Blackbody Tint B"));
        k->setName("blackbodyTintB"); k->setDefaultValue(0.4);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        firePage->addKnob(k); _imp->blackbodyTintB = k;
    }

    // Temperature remap curve
    {
        KnobParametricPtr k = AppManager::createKnob<KnobParametric>(this, tr("Temperature Remap"), 1, false);
        k->setName("temperatureRemap");
        k->setParametricRange(0.0, 1.0);
        k->setCurveColor(0, 1.0, 0.4, 0.1);  // orange for fire
        k->setHintToolTip(tr("Remap temperature values before blackbody conversion. X = input temp (0-1), Y = output. "
                              "Use to control fire shape and color transitions."));
        k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 0.0, 0.0, eKeyframeTypeLinear);
        k->addControlPoint(eValueChangedReasonNatronInternalEdited, 0, 1.0, 1.0, eKeyframeTypeLinear);
        k->setDefaultCurvesFromCurves();
        firePage->addKnob(k);
        _imp->temperatureRemap = k;
    }

    // Grid Bindings — user maps VDB grid names to Cycles attributes
    KnobPagePtr bindPage = AppManager::createKnob<KnobPage>(this, tr("Bindings"));
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Density Grid"));
        k->setName("bindDensity"); k->setDefaultValue("density");
        k->setHintToolTip(tr("VDB grid name for smoke density."));
        bindPage->addKnob(k); _imp->bindDensity = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Temperature Grid"));
        k->setName("bindTemperature"); k->setDefaultValue("temperature");
        k->setHintToolTip(tr("VDB grid name for fire temperature (drives blackbody color)."));
        bindPage->addKnob(k); _imp->bindTemperature = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Flame Grid"));
        k->setName("bindFlame"); k->setDefaultValue("flames");
        k->setHintToolTip(tr("VDB grid name for flame intensity."));
        bindPage->addKnob(k); _imp->bindFlame = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Color Grid"));
        k->setName("bindColor"); k->setDefaultValue("Cd");
        k->setHintToolTip(tr("VDB grid name for per-voxel color."));
        bindPage->addKnob(k); _imp->bindColor = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Velocity Grid"));
        k->setName("bindVelocity"); k->setDefaultValue("vel");
        k->setHintToolTip(tr("VDB grid name for velocity (motion blur)."));
        bindPage->addKnob(k); _imp->bindVelocity = k;
    }

    // Render settings
    KnobPagePtr renderPage = AppManager::createKnob<KnobPage>(this, tr("Render"));
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Step Size"));
        k->setName("stepSize"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Ray marching step size. 0 = auto from voxel size."));
        renderPage->addKnob(k); _imp->stepSize = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Volume Bounces"));
        k->setName("volumeBounces"); k->setDefaultValue(6);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(8);
        k->setHintToolTip(tr("Max light bounces inside volume."));
        renderPage->addKnob(k); _imp->volumeBounces = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Anisotropy"));
        k->setName("anisotropy"); k->setDefaultValue(0.3); k->setAnimationEnabled(true);
        k->setMinimum(-1.0); k->setMaximum(1.0);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Scattering direction. 0=isotropic, +1=forward, -1=backward."));
        renderPage->addKnob(k); _imp->anisotropy = k;
    }
}

bool
ReadVDB::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                     double /*time*/, bool /*originatedFromMainThread*/)
{
    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (k == fileKnob.get()) {
        std::string path = fileKnob->getValue();
        if (!path.empty()) {
            loadVDBFile(path);
        }
        return true;
    }
    return false;
}

void
ReadVDB::onKnobsLoaded()
{
    // After a project load the file-path knob is restored but no knobChanged
    // fires, so the grid list was never enumerated and the node served nothing
    // until the user re-picked the file. Same pattern as ReadGeo /
    // ReadAlembicArchive / ReadAlembicCamera / ReadAlembicTransform.
    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (fileKnob) {
        const std::string path = fileKnob->getValue();
        if (!path.empty()) {
            loadVDBFile(path);
        }
    }
}

void
ReadVDB::loadVDBFile(const std::string& path)
{
#ifdef NATRON_HAVE_OPENVDB
    std::lock_guard<std::mutex> lk(_imp->vdbMutex);

    // Decide ONCE per file pick whether this is a sequence. Every later read
    // consults this instead of blindly substituting a frame number into the
    // name (which broke single-frame and versioned filenames).
    _imp->isSequence = detectSequence(path);
    std::cerr << "[ReadVDB] " << (_imp->isSequence ? "sequence" : "single frame")
              << ": " << path << std::endl;

    try {
        openvdb::initialize();
        std::cerr << "[ReadVDB] Opening file: " << path << std::endl;
        openvdb::io::File file(path);
        file.open();

        // Enumerate grids
        _imp->gridNames.clear();
        std::vector<ChoiceOption> entries;

        for (openvdb::io::File::NameIterator it = file.beginName(); it != file.endName(); ++it) {
            std::string name = it.gridName();
            std::cerr << "[ReadVDB] Found grid: " << name << std::endl;
            _imp->gridNames.push_back(name);
            entries.push_back(ChoiceOption(name, name, name));
        }

        file.close();

        if (!entries.empty()) {
            KnobChoicePtr gridKnob = _imp->gridName.lock();
            gridKnob->populateChoices(entries);
        }

        _imp->lastLoadedPath = path;
        _imp->hasCachedData = false;
        _imp->hasCachedBounds = false;
        _imp->lastError.clear();
    } catch (const std::exception& e) {
        _imp->lastError = "could not open " + path + " (" + e.what() + ")";
        std::cerr << "[ReadVDB] " << _imp->lastError << std::endl;
    } catch (...) {
        _imp->lastError = "could not open " + path + " (unknown error)";
        std::cerr << "[ReadVDB] " << _imp->lastError << std::endl;
    }
#else
    (void)path;
#endif
}

void
ReadVDB::getTransform(double time,
                      double& tx, double& ty, double& tz,
                      double& sx, double& sy, double& sz) const
{
    tx = _imp->translateX.lock()->getValueAtTime(time);
    ty = _imp->translateY.lock()->getValueAtTime(time);
    tz = _imp->translateZ.lock()->getValueAtTime(time);
    sx = _imp->scaleX.lock()->getValueAtTime(time);
    sy = _imp->scaleY.lock()->getValueAtTime(time);
    sz = _imp->scaleZ.lock()->getValueAtTime(time);
}

StatusEnum
ReadVDB::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setIsFrameVarying(true);
    return eStatusOK;
}

bool
ReadVDB::getVDBBounds(double time,
                      float& outMinX, float& outMinY, float& outMinZ,
                      float& outMaxX, float& outMaxY, float& outMaxZ)
{
#ifdef NATRON_HAVE_OPENVDB
    std::lock_guard<std::mutex> lk(_imp->vdbMutex);
    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (!fileKnob) return false;
    std::string templatePath = fileKnob->getValue();
    if (templatePath.empty()) return false;

    std::string path = resolvePathAtTime(time);

    // Cache hit — return stored bounds
    if (_imp->hasCachedBounds && _imp->lastBoundsPath == path) {
        outMinX = _imp->bMinX; outMinY = _imp->bMinY; outMinZ = _imp->bMinZ;
        outMaxX = _imp->bMaxX; outMaxY = _imp->bMaxY; outMaxZ = _imp->bMaxZ;
        return true;
    }

    // If resolved frame file doesn't exist, fall back to the template path
    // (sim domain is stable across frames, so any frame's bounds work for display)
    {
        std::ifstream testFile(path.c_str());
        if (!testFile.good()) {
            path = templatePath;
        }
    }

    try {
        openvdb::initialize();
        openvdb::io::File file(path);
        file.open();

        // Read full grid (cached), prefer density, fall back to first available
        openvdb::GridBase::Ptr grid;
        std::string preferName = "density";
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            if (*it == preferName) { grid = file.readGrid(preferName); break; }
        }
        if (!grid) {
            for (auto it = file.beginName(); it != file.endName(); ++it) {
                grid = file.readGrid(*it);
                if (grid) break;
            }
        }
        file.close();

        if (!grid) return false;

        openvdb::CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
        if (bbox.empty()) return false;

        openvdb::Vec3d wsMin = grid->transform().indexToWorld(bbox.min().asVec3d());
        openvdb::Vec3d wsMax = grid->transform().indexToWorld(bbox.max().asVec3d());

        _imp->bMinX = outMinX = (float)wsMin.x();
        _imp->bMinY = outMinY = (float)wsMin.y();
        _imp->bMinZ = outMinZ = (float)wsMin.z();
        _imp->bMaxX = outMaxX = (float)wsMax.x();
        _imp->bMaxY = outMaxY = (float)wsMax.y();
        _imp->bMaxZ = outMaxZ = (float)wsMax.z();
        _imp->lastBoundsPath = path;
        _imp->hasCachedBounds = true;
        return true;
    } catch (...) {
        return false;
    }
#else
    (void)time;
    (void)outMinX; (void)outMinY; (void)outMinZ;
    (void)outMaxX; (void)outMaxY; (void)outMaxZ;
    return false;
#endif
}

bool
ReadVDB::getViewportDensitySamples(double time, int targetN,
                                   std::vector<float>& outDensity,
                                   std::vector<float>& outFire,
                                   int& outNx, int& outNy, int& outNz)
{
    outDensity.clear();
    outFire.clear();
    outNx = outNy = outNz = 0;
#ifdef NATRON_HAVE_OPENVDB
    if (targetN < 2) targetN = 2;
    if (targetN > 96) targetN = 96;

    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (!fileKnob) return false;
    std::string templatePath = fileKnob->getValue();
    if (templatePath.empty()) return false;

    std::string path = resolvePathAtTime(time);
    {
        std::ifstream testFile(path.c_str());
        if (!testFile.good()) path = templatePath;
    }

    try {
        openvdb::initialize();
        openvdb::io::File file(path);
        file.open();

        // List grid names once, then read the density and (optional) fire grids.
        std::vector<std::string> names;
        for (auto it = file.beginName(); it != file.endName(); ++it) names.push_back(*it);

        openvdb::GridBase::Ptr densBase;
        for (const std::string& n : names) {
            if (n == "density") { densBase = file.readGrid(n); break; }
        }
        if (!densBase && !names.empty()) densBase = file.readGrid(names[0]);

        // Fire / heat grid (Houdini pyro: shares density's transform & resolution).
        openvdb::GridBase::Ptr fireBase;
        const char* fireNames[] = { "flames", "flame", "temperature", "heat", "fuel" };
        for (const char* fn : fireNames) {
            for (const std::string& n : names) {
                if (n == fn) { fireBase = file.readGrid(n); break; }
            }
            if (fireBase) break;
        }
        file.close();

        openvdb::FloatGrid::Ptr grid = openvdb::gridPtrCast<openvdb::FloatGrid>(densBase);
        if (!grid) return false;
        openvdb::FloatGrid::Ptr fireGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(fireBase);

        openvdb::CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
        if (bbox.empty()) return false;
        const openvdb::Coord dim = bbox.dim();   // voxels per axis (>= 1)
        const int mx = std::max(dim.x(), std::max(dim.y(), dim.z()));
        if (mx < 1) return false;

        const int nx = std::max(2, (int)std::lround((double)targetN * dim.x() / mx));
        const int ny = std::max(2, (int)std::lround((double)targetN * dim.y() / mx));
        const int nz = std::max(2, (int)std::lround((double)targetN * dim.z() / mx));

        std::vector<float> dens((size_t)nx * ny * nz, 0.f);
        std::vector<float> fire;
        if (fireGrid) fire.assign((size_t)nx * ny * nz, 0.f);

        openvdb::FloatGrid::ConstAccessor acc = grid->getConstAccessor();
        std::unique_ptr<openvdb::FloatGrid::ConstAccessor> fireAcc;
        if (fireGrid) fireAcc.reset(new openvdb::FloatGrid::ConstAccessor(fireGrid->getConstAccessor()));
        const openvdb::Coord bmin = bbox.min();
        float maxVal = 0.f, fireMax = 0.f;
        // Trilinear (BoxSampler) at fractional index coords → smoother values than
        // nearest-voxel, with less blockiness/flicker on a coarse lattice. The 8×
        // corner lookups only run on a re-sample (cache miss), not per viewport frame.
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const double fx = (double)bmin.x() + (double)i / (nx - 1) * (dim.x() - 1);
                    const double fy = (double)bmin.y() + (double)j / (ny - 1) * (dim.y() - 1);
                    const double fz = (double)bmin.z() + (double)k / (nz - 1) * (dim.z() - 1);
                    const openvdb::Vec3R p(fx, fy, fz);
                    float d = openvdb::tools::BoxSampler::sample(acc, p);
                    if (d < 0.f) d = 0.f;
                    const size_t idx = ((size_t)k * ny + j) * nx + i;
                    dens[idx] = d;
                    if (d > maxVal) maxVal = d;
                    if (fireAcc) {
                        float f = openvdb::tools::BoxSampler::sample(*fireAcc, p);
                        if (f < 0.f) f = 0.f;
                        fire[idx] = f;
                        if (f > fireMax) fireMax = f;
                    }
                }
            }
        }
        if (maxVal <= 1e-8f) return false;   // empty / all-zero grid
        const float inv = 1.0f / maxVal;
        for (size_t s = 0; s < dens.size(); ++s) dens[s] *= inv;   // normalize to [0,1]
        if (fireGrid && fireMax > 1e-8f) {
            const float finv = 1.0f / fireMax;
            for (size_t s = 0; s < fire.size(); ++s) fire[s] *= finv;
            outFire.swap(fire);                                    // else leave empty
        }

        outDensity.swap(dens);
        outNx = nx; outNy = ny; outNz = nz;
        return true;
    } catch (...) {
        return false;
    }
#else
    (void)time; (void)targetN;
    return false;
#endif
}

#ifdef NATRON_HAVE_OPENVDB
bool
ReadVDB::getVDBDirect(double time, VDBDirectData& outData)
{
    // Direct VDB grid access for Cycles — no dense conversion.
    // Reads the OpenVDB grid directly from the file and returns it.
    // REVERT: if issues, switch CyclesRenderer back to getVolumeData() (dense path).
    std::lock_guard<std::mutex> lk(_imp->vdbMutex);
    KnobFilePtr fileKnob = _imp->filePath.lock();
    std::string templatePath = fileKnob->getValue();
    if (templatePath.empty()) return false;

    std::string path = resolvePathAtTime(time);

    try {
        openvdb::initialize();
        openvdb::io::File file(path);
        file.open();

        int gridIdx = _imp->gridName.lock()->getValue();
        std::string selectedGrid;
        if (gridIdx >= 0 && gridIdx < (int)_imp->gridNames.size()) {
            selectedGrid = _imp->gridNames[gridIdx];
        } else if (!_imp->gridNames.empty()) {
            selectedGrid = _imp->gridNames[0];
        }

        // Load ALL grids from the file
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            openvdb::GridBase::Ptr grid = file.readGrid(*it);
            if (grid) {
                VDBGridInfo gi;
                gi.grid = grid;
                gi.name = *it;
                outData.grids.push_back(gi);
            }
        }
        file.close();

        if (outData.grids.empty()) return false;

        outData.density = (float)_imp->density.lock()->getValueAtTime(time);
        outData.colorR = (float)_imp->colorR.lock()->getValueAtTime(time);
        outData.colorG = (float)_imp->colorG.lock()->getValueAtTime(time);
        outData.colorB = (float)_imp->colorB.lock()->getValueAtTime(time);
        outData.absorptionR = (float)_imp->absorptionR.lock()->getValueAtTime(time);
        outData.absorptionG = (float)_imp->absorptionG.lock()->getValueAtTime(time);
        outData.absorptionB = (float)_imp->absorptionB.lock()->getValueAtTime(time);
        outData.stepSize = (float)_imp->stepSize.lock()->getValueAtTime(time);
        outData.volumeBounces = _imp->volumeBounces.lock()->getValueAtTime(time);
        outData.anisotropy = (float)_imp->anisotropy.lock()->getValueAtTime(time);
        outData.blackbodyIntensity = (float)_imp->blackbodyIntensity.lock()->getValueAtTime(time);
        outData.blackbodyTintR = (float)_imp->blackbodyTintR.lock()->getValueAtTime(time);
        outData.blackbodyTintG = (float)_imp->blackbodyTintG.lock()->getValueAtTime(time);
        outData.blackbodyTintB = (float)_imp->blackbodyTintB.lock()->getValueAtTime(time);
        outData.temperatureScale = (float)_imp->temperatureScale.lock()->getValueAtTime(time);
        outData.bindDensity = _imp->bindDensity.lock()->getValue();
        outData.bindTemperature = _imp->bindTemperature.lock()->getValue();
        outData.bindFlame = _imp->bindFlame.lock()->getValue();
        outData.bindColor = _imp->bindColor.lock()->getValue();
        outData.bindVelocity = _imp->bindVelocity.lock()->getValue();

        // Sample remap curves at REMAP_SAMPLES points
        const int N = VDBDirectData::REMAP_SAMPLES;
        outData.densityRemap.resize(N);
        outData.temperatureRemap.resize(N);
        {
            KnobParametricPtr densRemap = _imp->densityRemap.lock();
            KnobParametricPtr tempRemap = _imp->temperatureRemap.lock();
            for (int i = 0; i < N; ++i) {
                double t = (double)i / (double)(N - 1);
                double dVal = t, tVal = t;  // default: identity
                if (densRemap) densRemap->getValue(0, t, &dVal);
                if (tempRemap) tempRemap->getValue(0, t, &tVal);
                outData.densityRemap[i] = (float)dVal;
                outData.temperatureRemap[i] = (float)tVal;
            }
        }

        _imp->lastError.clear();
        return true;
    } catch (const std::exception& e) {
        // Record WHY, with the path actually opened — the callers (FastVolume,
        // Cycles) put this in their persistent message. stderr alone is
        // invisible in a GUI launch, which is what made a missing frame read as
        // an unexplained "failed to load VDB grids".
        _imp->lastError = "could not open " + path + " (" + e.what() + ")";
        std::cerr << "[ReadVDB] " << _imp->lastError << std::endl;
        return false;
    }
}
#endif

bool
ReadVDB::getVolumeData(double time, VDBVolumeData& outData)
{
#ifdef NATRON_HAVE_OPENVDB
    std::lock_guard<std::mutex> lk(_imp->vdbMutex);
    KnobFilePtr fileKnob = _imp->filePath.lock();
    std::string templatePath = fileKnob->getValue();
    if (templatePath.empty()) return false;

    std::string path = resolvePathAtTime(time);

    int maxRes = _imp->maxResolution.lock()->getValueAtTime(time);

    // Check cache — must match both path AND frame
    if (_imp->hasCachedData && _imp->lastLoadedFramePath == path) {
        outData = _imp->cachedData;
        outData.density = (float)_imp->density.lock()->getValueAtTime(time);
        outData.colorR = (float)_imp->colorR.lock()->getValueAtTime(time);
        outData.colorG = (float)_imp->colorG.lock()->getValueAtTime(time);
        outData.colorB = (float)_imp->colorB.lock()->getValueAtTime(time);
        return true;
    }

    try {
        openvdb::initialize();
        openvdb::io::File file(path);
        file.open();

        // Get selected grid name
        int gridIdx = _imp->gridName.lock()->getValue();
        std::string selectedGrid;
        if (gridIdx >= 0 && gridIdx < (int)_imp->gridNames.size()) {
            selectedGrid = _imp->gridNames[gridIdx];
        } else if (!_imp->gridNames.empty()) {
            selectedGrid = _imp->gridNames[0];
        }

        std::cerr << "[ReadVDB] Reading grid: " << selectedGrid << std::endl;
        openvdb::GridBase::Ptr baseGrid = file.readGrid(selectedGrid);
        file.close();

        if (!baseGrid) {
            std::cerr << "[ReadVDB] ERROR: baseGrid is null" << std::endl;
            return false;
        }

        std::cerr << "[ReadVDB] Grid type: " << baseGrid->valueType() << std::endl;

        // Try to cast to FloatGrid
        openvdb::FloatGrid::Ptr floatGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);
        if (!floatGrid) {
            std::cerr << "[ReadVDB] ERROR: not a FloatGrid, trying to read as any scalar type" << std::endl;
            return false;
        }

        // Get the active bounding box
        openvdb::CoordBBox bbox = floatGrid->evalActiveVoxelBoundingBox();
        if (bbox.empty()) {
            std::cerr << "[ReadVDB] ERROR: bounding box is empty" << std::endl;
            return false;
        }

        openvdb::Coord bMin = bbox.min();
        openvdb::Coord bMax = bbox.max();
        openvdb::Coord bSize = bMax - bMin + openvdb::Coord(1, 1, 1);

        // Determine output resolution — preserve the VDB's voxel aspect ratio.
        // Map the largest dimension to maxRes, scale the others proportionally
        // and floor to at least 8. Without this we'd squash non-cubic VDBs into
        // a uniform cube, which made smoke look like a soft blob filling the
        // bounding box instead of matching the Cycles render.
        int maxDim = std::max({bSize.x(), bSize.y(), bSize.z()});
        if (maxDim < 1) maxDim = 1;
        const double scale = (double)std::min(maxRes, maxDim) / (double)maxDim;
        int resX = std::max(8, (int)std::round((double)bSize.x() * scale));
        int resY = std::max(8, (int)std::round((double)bSize.y() * scale));
        int resZ = std::max(8, (int)std::round((double)bSize.z() * scale));

        std::cerr << "[ReadVDB] Index bbox: (" << bMin.x() << "," << bMin.y() << "," << bMin.z()
                  << ") to (" << bMax.x() << "," << bMax.y() << "," << bMax.z() << ")"
                  << " size: " << bSize.x() << "x" << bSize.y() << "x" << bSize.z()
                  << " maxDim=" << maxDim
                  << " sampled at " << resX << "x" << resY << "x" << resZ << std::endl;

        // World-space bounds from VDB transform
        openvdb::Vec3d worldMin = floatGrid->transform().indexToWorld(bMin.asVec3d());
        openvdb::Vec3d worldMax = floatGrid->transform().indexToWorld(bMax.asVec3d());

        std::cerr << "[ReadVDB] World bbox: (" << worldMin.x() << "," << worldMin.y() << "," << worldMin.z()
                  << ") to (" << worldMax.x() << "," << worldMax.y() << "," << worldMax.z() << ")" << std::endl;

        outData.bboxMinX = (float)worldMin.x();
        outData.bboxMinY = (float)worldMin.y();
        outData.bboxMinZ = (float)worldMin.z();
        outData.bboxMaxX = (float)worldMax.x();
        outData.bboxMaxY = (float)worldMax.y();
        outData.bboxMaxZ = (float)worldMax.z();
        outData.resX = resX;
        outData.resY = resY;
        outData.resZ = resZ;

        // Sample the VDB grid into a dense 3D array sized to match the VDB's
        // aspect ratio. Storage order (X-major): index = z*resY*resX + y*resX + x.
        outData.densityData.assign((size_t)resX * (size_t)resY * (size_t)resZ, 0.0f);

        openvdb::FloatGrid::ConstAccessor accessor = floatGrid->getConstAccessor();

        for (int z = 0; z < resZ; ++z) {
            for (int y = 0; y < resY; ++y) {
                for (int x = 0; x < resX; ++x) {
                    // Map dense [0,resN) to VDB index space along each axis.
                    float fx = bMin.x() + ((float)x + 0.5f) / (float)resX * bSize.x();
                    float fy = bMin.y() + ((float)y + 0.5f) / (float)resY * bSize.y();
                    float fz = bMin.z() + ((float)z + 0.5f) / (float)resZ * bSize.z();

                    openvdb::Coord ijk((int)fx, (int)fy, (int)fz);
                    float val = accessor.getValue(ijk);

                    outData.densityData[(size_t)z * resY * resX + (size_t)y * resX + (size_t)x]
                        = std::max(0.0f, val);
                }
            }
        }

        outData.density = (float)_imp->density.lock()->getValueAtTime(time);
        outData.colorR = (float)_imp->colorR.lock()->getValueAtTime(time);
        outData.colorG = (float)_imp->colorG.lock()->getValueAtTime(time);
        outData.colorB = (float)_imp->colorB.lock()->getValueAtTime(time);

        // Check density statistics
        float minVal = 1e10f, maxVal = -1e10f, sum = 0;
        int nonZero = 0;
        for (size_t i = 0; i < outData.densityData.size(); ++i) {
            float v = outData.densityData[i];
            if (v > 0) ++nonZero;
            if (v < minVal) minVal = v;
            if (v > maxVal) maxVal = v;
            sum += v;
        }
        std::cerr << "[ReadVDB] Density stats: min=" << minVal << " max=" << maxVal
                  << " avg=" << (sum / outData.densityData.size())
                  << " nonZero=" << nonZero << "/" << outData.densityData.size() << std::endl;

        // Cache
        _imp->cachedData = outData;
        _imp->hasCachedData = true;
        _imp->lastLoadedFramePath = path;

        return true;
    } catch (...) {
        return false;
    }
#else
    (void)time;
    (void)outData;
    return false;
#endif
}

StatusEnum
ReadVDB::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                               ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
ReadVDB::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;

    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ReadVDB.cpp"
