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
#include <sstream>

#include <iostream>

#ifdef NATRON_HAVE_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/tools/Dense.h>
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
    KnobFileWPtr filePath;
    KnobChoiceWPtr gridName;
    KnobIntWPtr maxResolution;
    KnobDoubleWPtr density;
    KnobDoubleWPtr colorR, colorG, colorB;
    KnobIntWPtr frameOffset;

    // Transform
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;

    // Cached data
    std::string lastLoadedPath;
    std::string lastLoadedFramePath;
    std::vector<std::string> gridNames;
    ReadVDB::VDBVolumeData cachedData;
    bool hasCachedData;

    ReadVDBPrivate() : hasCachedData(false) {}
};

// Resolve frame number in a VDB filename
// e.g. "smoke.0050.vdb" at frame 10 with offset 0 → "smoke.0010.vdb"
static std::string
resolveFramePath(const std::string& templatePath, int frame)
{
    // Find the last group of digits before .vdb
    std::string path = templatePath;
    size_t dotVdb = path.rfind(".vdb");
    if (dotVdb == std::string::npos) dotVdb = path.rfind(".VDB");
    if (dotVdb == std::string::npos) return path;

    // Find the digit group before .vdb
    size_t digitEnd = dotVdb;
    // Skip the dot before digits (e.g. ".0050.vdb" → find "0050")
    if (digitEnd > 0 && path[digitEnd - 1] == '.') {
        // No digits between dots
    }

    // Search backwards from dotVdb for digits
    size_t numEnd = dotVdb;
    while (numEnd > 0 && path[numEnd - 1] == '.') --numEnd; // skip dot
    size_t numStart = numEnd;
    while (numStart > 0 && path[numStart - 1] >= '0' && path[numStart - 1] <= '9') --numStart;

    if (numStart == numEnd) return path; // no digits found

    int padding = (int)(numEnd - numStart);
    if (padding < 1) padding = 4;

    // Build the frame number string with padding
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(padding) << frame;

    return path.substr(0, numStart) + ss.str() + path.substr(numEnd);
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
        k->setName("density"); k->setDefaultValue(10.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(50.0);
        mainPage->addKnob(k);
        _imp->density = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Frame Offset"));
        k->setName("frameOffset"); k->setDefaultValue(0);
        k->setDisplayMinimum(-100); k->setDisplayMaximum(100);
        mainPage->addKnob(k);
        _imp->frameOffset = k;
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
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color R"));
        k->setName("colorR"); k->setDefaultValue(0.8);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorR = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color G"));
        k->setName("colorG"); k->setDefaultValue(0.8);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorG = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Color B"));
        k->setName("colorB"); k->setDefaultValue(0.9);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        colorPage->addKnob(k); _imp->colorB = k;
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
ReadVDB::loadVDBFile(const std::string& path)
{
#ifdef NATRON_HAVE_OPENVDB
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
    } catch (...) {
        // VDB load failed
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
ReadVDB::getVolumeData(double time, VDBVolumeData& outData)
{
#ifdef NATRON_HAVE_OPENVDB
    KnobFilePtr fileKnob = _imp->filePath.lock();
    std::string templatePath = fileKnob->getValue();
    if (templatePath.empty()) return false;

    // Resolve frame number in the filename
    int frame = (int)time + _imp->frameOffset.lock()->getValueAtTime(time);
    if (frame < 0) frame = 0;
    std::string path = resolveFramePath(templatePath, frame);

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

        // Determine output resolution (fit to maxRes cube)
        int maxDim = std::max({bSize.x(), bSize.y(), bSize.z()});
        int res = std::min(maxRes, maxDim);
        if (res < 8) res = 8;

        std::cerr << "[ReadVDB] Index bbox: (" << bMin.x() << "," << bMin.y() << "," << bMin.z()
                  << ") to (" << bMax.x() << "," << bMax.y() << "," << bMax.z() << ")"
                  << " size: " << bSize.x() << "x" << bSize.y() << "x" << bSize.z()
                  << " maxDim=" << maxDim << " res=" << res << std::endl;

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
        outData.resolution = res;

        // Sample the VDB grid into a dense 3D array
        outData.densityData.resize(res * res * res, 0.0f);

        openvdb::FloatGrid::ConstAccessor accessor = floatGrid->getConstAccessor();

        for (int z = 0; z < res; ++z) {
            for (int y = 0; y < res; ++y) {
                for (int x = 0; x < res; ++x) {
                    // Map dense [0,res) to VDB index space
                    float fx = bMin.x() + (float)x / (float)res * bSize.x();
                    float fy = bMin.y() + (float)y / (float)res * bSize.y();
                    float fz = bMin.z() + (float)z / (float)res * bSize.z();

                    openvdb::Coord ijk((int)fx, (int)fy, (int)fz);
                    float val = accessor.getValue(ijk);

                    outData.densityData[z * res * res + y * res + x] = std::max(0.0f, val);
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
