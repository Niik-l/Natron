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

#include "DeepRead.h"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <sstream>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include "../../AppInstance.h"
#include "DeepImage.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"
#include "../../NodeMetadata.h"

#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/deepdata.h>
#endif

NATRON_NAMESPACE_ENTER

// Substitute a '####' run in a sequence pattern with the zero-padded frame
// number. A path without '#' is returned unchanged (single-file behavior).
static std::string
resolveSeqPath(const std::string& pattern, int frame)
{
    const std::size_t h = pattern.find('#');
    if (h == std::string::npos) return pattern;
    std::size_t e = h;
    while (e < pattern.size() && pattern[e] == '#') ++e;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*d", (int)(e - h), frame);
    return pattern.substr(0, h) + buf + pattern.substr(e);
}

// Scan the pattern's directory for existing frames; returns false if the path
// has no '#' or no frames exist on disk.
static bool
seqFrameRangeOnDisk(const std::string& pattern, int* first, int* last)
{
    const std::size_t h = pattern.find('#');
    if (h == std::string::npos) return false;

    const QFileInfo fi(QString::fromStdString(pattern));
    const QString fname = fi.fileName();
    const int fh = fname.indexOf(QLatin1Char('#'));
    if (fh < 0) return false;  // '#' only in the directory part — not a sequence
    int fe = fh;
    while (fe < fname.size() && fname[fe] == QLatin1Char('#')) ++fe;
    const QString prefix = fname.left(fh);
    const QString suffix = fname.mid(fe);

    const QStringList entries = fi.dir().entryList(
        QStringList() << (prefix + QString::fromUtf8("*") + suffix), QDir::Files);
    int mn = 0, mx = 0;
    bool any = false;
    for (const QString& en : entries) {
        const QString mid = en.mid(prefix.size(), en.size() - prefix.size() - suffix.size());
        bool ok = false;
        const int f = mid.toInt(&ok);
        if (!ok) continue;
        if (!any || f < mn) mn = f;
        if (!any || f > mx) mx = f;
        any = true;
    }
    if (!any) return false;
    if (first) *first = mn;
    if (last) *last = mx;
    return true;
}

// Resolve the pattern for rendering at a frame, the way the beauty Read does
// by default (Before/After = Hold, Missing Frame = Load nearest): the frame's
// own file when it exists, else the sequence clamped to the frames on disk,
// else the nearest existing frame. Without this the Deep Merge tree showed
// the held beauty but the DeepRead errored "Cannot open file" whenever the
// timeline sat outside the rendered range (e.g. project at 1001, pass
// rendered 1..N) — reported as "the deep didn't load".
static std::string
resolveRenderPath(const std::string& pattern, int frame)
{
    const std::string exact = resolveSeqPath(pattern, frame);
    if (pattern.find('#') == std::string::npos || QFileInfo::exists(QString::fromStdString(exact))) {
        return exact;
    }
    int first = 0, last = 0;
    if (!seqFrameRangeOnDisk(pattern, &first, &last)) {
        return exact;
    }
    int f = std::max(first, std::min(last, frame));   // hold before / after
    for (int d = 0; d <= last - first; ++d) {         // nearest within a gap
        const int lo = f - d, hi = f + d;
        if (lo >= first && QFileInfo::exists(QString::fromStdString(resolveSeqPath(pattern, lo)))) return resolveSeqPath(pattern, lo);
        if (hi <= last && QFileInfo::exists(QString::fromStdString(resolveSeqPath(pattern, hi)))) return resolveSeqPath(pattern, hi);
    }
    return exact;
}

// Resolve the pattern for probing (info display, metadata): current path if it
// is a plain file, else the first frame that exists on disk.
static std::string
resolveProbePath(const std::string& pattern)
{
    int first = 0, last = 0;
    if (seqFrameRangeOnDisk(pattern, &first, &last))
        return resolveSeqPath(pattern, first);
    return resolveSeqPath(pattern, 1);
}

struct DeepReadPrivate
{
    KnobFileWPtr filePath;
    KnobStringWPtr fileInfo;

    DeepImagePtr deepImage;
    mutable QMutex deepImageMutex;

    DeepReadPrivate()
        : filePath()
        , fileInfo()
        , deepImage()
        , deepImageMutex()
    {
    }
};

DeepRead::DeepRead(NodePtr node)
    : EffectInstance(node)
    , _imp(new DeepReadPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

DeepRead::~DeepRead()
{
}

std::string
DeepRead::getPluginDescription() const
{
    return tr("Read deep EXR image files using OpenImageIO. "
              "Deep images contain multiple samples per pixel, each with color and depth information. "
              "This node reads the deep data and produces both a deep image (accessible by downstream deep nodes "
              "such as DeepFlatten and DeepMerge) and a flattened RGBA preview for display in the Viewer. "
              "Supported formats: Deep OpenEXR (.exr).").toStdString();
}

void
DeepRead::addAcceptedComponents(int /*inputNb*/,
                                std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
DeepRead::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DeepRead::isHostChannelSelectorSupported(bool* /*defaultR*/,
                                         bool* /*defaultG*/,
                                         bool* /*defaultB*/,
                                         bool* /*defaultA*/) const
{
    return false;
}

DeepImagePtr
DeepRead::getDeepImage() const
{
    QMutexLocker lock(&_imp->deepImageMutex);
    return _imp->deepImage;
}

void
DeepRead::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Controls"));

    KnobFilePtr filePath = AppManager::createKnob<KnobFile>(this, tr("File"));
    filePath->setName("filename");
    filePath->setHintToolTip(tr("Path to the deep EXR file to read. Use #### for a frame-number "
                                "sequence (e.g. beauty_deep.####.exr) — the frame range is scanned "
                                "from disk."));
    filePath->setAnimationEnabled(false);
    page->addKnob(filePath);
    _imp->filePath = filePath;

    KnobStringPtr fileInfo = AppManager::createKnob<KnobString>(this, tr("File Info"));
    fileInfo->setName("fileInfo");
    fileInfo->setAnimationEnabled(false);
    fileInfo->setAsMultiLine();
    fileInfo->setEvaluateOnChange(false);
    fileInfo->setIsPersistent(false);
    fileInfo->setDefaultValue("No file loaded");
    page->addKnob(fileInfo);
    _imp->fileInfo = fileInfo;
}

bool
DeepRead::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    if (_imp->filePath.lock().get() == k) {
        // File path changed — update info display
        std::string rawPath = _imp->filePath.lock()->getValue();
        if (rawPath.empty()) {
            _imp->fileInfo.lock()->setValue("No file loaded");
            return true;
        }
        std::string path = resolveProbePath(rawPath);

#ifdef NATRON_HAVE_OPENIMAGEIO
        // Probe the file
        auto input = OIIO::ImageInput::open(path);
        if (!input) {
            std::string err = "Cannot open file: " + path;
            _imp->fileInfo.lock()->setValue(err);
            return true;
        }

        const OIIO::ImageSpec& spec = input->spec();
        std::ostringstream info;
        info << "Resolution: " << spec.full_width << " x " << spec.full_height
             << " (data: " << spec.width << " x " << spec.height << ")\n";
        info << "Channels: " << spec.nchannels << "\n";
        info << "Deep: " << (spec.deep ? "Yes" : "No") << "\n";
        {
            int f1 = 0, f2 = 0;
            if (seqFrameRangeOnDisk(rawPath, &f1, &f2))
                info << "Frames on disk: " << f1 << " - " << f2 << "\n";
        }
        if (spec.deep) {
            info << "Channel names: ";
            for (int i = 0; i < spec.nchannels; ++i) {
                if (i > 0) info << ", ";
                info << spec.channelnames[i];
            }
            info << "\n";
        }
        _imp->fileInfo.lock()->setValue(info.str());
        input->close();
#else
        _imp->fileInfo.lock()->setValue("OpenImageIO not available — cannot probe file.");
#endif

        return true;
    }

    return false;
}

StatusEnum
DeepRead::getPreferredMetadata(NodeMetadata& metadata)
{
#ifdef NATRON_HAVE_OPENIMAGEIO
    KnobFilePtr fileKnob = _imp->filePath.lock();
    if (!fileKnob) return eStatusOK;
    std::string path = fileKnob->getValue();
    if (path.empty()) return eStatusOK;
    path = resolveProbePath(path);

    auto input = OIIO::ImageInput::open(path);
    if (!input) return eStatusOK;

    const OIIO::ImageSpec& spec = input->spec();

    // Read PAR from EXR file (defaults to 1.0 if not present)
    double par = spec.get_float_attribute("PixelAspectRatio", 1.0f);
    metadata.setPixelAspectRatio(-1, par);

    // Set output format from display window
    RectI format;
    format.x1 = spec.full_x;
    format.y1 = spec.full_y;
    format.x2 = spec.full_x + spec.full_width;
    format.y2 = spec.full_y + spec.full_height;
    metadata.setOutputFormat(format);

    input->close();
#else
    Q_UNUSED(metadata);
#endif
    return eStatusOK;
}

StatusEnum
DeepRead::getRegionOfDefinition(U64 /*hash*/,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
#ifdef NATRON_HAVE_OPENIMAGEIO
    std::string path = _imp->filePath.lock()->getValue();
    if (path.empty()) {
        return eStatusFailed;
    }
    path = resolveRenderPath(path, (int)std::floor(time + 0.5));

    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        return eStatusFailed;
    }

    const OIIO::ImageSpec& spec = input->spec();
    if (!spec.deep) {
        input->close();
        return eStatusFailed;
    }

    // Use the display window (full canvas) for the RoD, not the data window.
    // The data window may be cropped smaller than the full image.
    rod->x1 = spec.full_x;
    rod->y1 = spec.full_y;
    rod->x2 = spec.full_x + spec.full_width;
    rod->y2 = spec.full_y + spec.full_height;

    input->close();
    return eStatusOK;
#else
    Q_UNUSED(rod);
    return eStatusFailed;
#endif
}

void
DeepRead::getFrameRange(double* first,
                        double* last)
{
    // Sequence: scan the frames on disk. Single file: 1-1.
    KnobFilePtr fileKnob = _imp->filePath.lock();
    const std::string path = fileKnob ? fileKnob->getValue() : std::string();
    int f1 = 0, f2 = 0;
    if (!path.empty() && seqFrameRangeOnDisk(path, &f1, &f2)) {
        *first = f1;
        *last = f2;
        return;
    }
    *first = 1;
    *last = 1;
}

StatusEnum
DeepRead::render(const RenderActionArgs& args)
{
#ifdef NATRON_HAVE_OPENIMAGEIO
    std::string path = _imp->filePath.lock()->getValue();
    if (path.empty()) {
        return eStatusFailed;
    }
    path = resolveRenderPath(path, (int)std::floor(args.time + 0.5));

    // Open the deep EXR file
    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        setPersistentMessage(eMessageTypeError,
                             "Cannot open file: " + path
                             + " (no frame of the sequence exists on disk)");
        return eStatusFailed;
    }

    const OIIO::ImageSpec& spec = input->spec();
    if (!spec.deep) {
        input->close();
        setPersistentMessage(eMessageTypeError,
                             "File is not a deep image: " + path);
        return eStatusFailed;
    }

    // Read the deep data
    OIIO::DeepData deepdata;
    bool ok = input->read_native_deep_image(0, 0, deepdata);
    input->close();

    if (!ok) {
        setPersistentMessage(eMessageTypeError,
                             "Failed to read deep image data from: " + path);
        return eStatusFailed;
    }

    // Build our DeepImage from the OIIO DeepData
    RectI dataWindow;
    dataWindow.x1 = spec.x;
    dataWindow.y1 = spec.y;
    dataWindow.x2 = spec.x + spec.width;
    dataWindow.y2 = spec.y + spec.height;

    std::vector<std::string> channelNames(spec.channelnames.begin(),
                                          spec.channelnames.end());

    DeepImagePtr deepImg = std::make_shared<DeepImage>(dataWindow,
                                                       spec.nchannels,
                                                       channelNames);

    // Set sample counts
    for (int y = dataWindow.y1; y < dataWindow.y2; ++y) {
        for (int x = dataWindow.x1; x < dataWindow.x2; ++x) {
            int pixIdx = (y - dataWindow.y1) * spec.width + (x - dataWindow.x1);
            int nSamples = deepdata.samples(pixIdx);
            deepImg->setSampleCount(x, y, nSamples);
        }
    }

    // Allocate the contiguous buffer
    deepImg->allocateFromSampleCounts();

    // Copy sample data from OIIO DeepData into our DeepImage
    for (int y = dataWindow.y1; y < dataWindow.y2; ++y) {
        for (int x = dataWindow.x1; x < dataWindow.x2; ++x) {
            int pixIdx = (y - dataWindow.y1) * spec.width + (x - dataWindow.x1);
            int nSamples = deepdata.samples(pixIdx);
            if (nSamples == 0) {
                continue;
            }

            float* dest = deepImg->getSampleData(x, y);
            for (int s = 0; s < nSamples; ++s) {
                for (int c = 0; c < spec.nchannels; ++c) {
                    // OIIO deep channels can be float/half or uint32 (e.g. object IDs)
                    if (spec.channelformat(c) == OIIO::TypeDesc::UINT32) {
                        dest[s * spec.nchannels + c] = static_cast<float>(deepdata.deep_value_uint(pixIdx, c, s));
                    } else {
                        dest[s * spec.nchannels + c] = deepdata.deep_value(pixIdx, c, s);
                    }
                }
            }
        }
    }

    // Store the deep image for downstream nodes
    {
        QMutexLocker lock(&_imp->deepImageMutex);
        _imp->deepImage = deepImg;
    }

    // Also produce a flattened RGBA preview for the Viewer
    assert(!args.outputPlanes.empty());
    const std::pair<ImagePlaneDesc, ImagePtr>& output = args.outputPlanes.front();
    ImagePtr outImg = output.second;

    if (outImg) {
        deepImg->flattenToImage(outImg.get());
    }

    clearPersistentMessage(false);
    return eStatusOK;
#else
    Q_UNUSED(args);
    setPersistentMessage(eMessageTypeError,
                         "DeepRead requires OpenImageIO which is not available in this build.");
    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepRead.cpp"
