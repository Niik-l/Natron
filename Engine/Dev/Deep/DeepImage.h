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

#ifndef NATRON_ENGINE_DEEPIMAGE_H
#define NATRON_ENGINE_DEEPIMAGE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <QReadWriteLock>

#include "../../RectI.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Stores deep image data with variable samples per pixel.
 *
 * Each pixel can have zero or more depth samples. Each sample contains
 * values for all channels (typically R, G, B, A, Z, ZBack).
 *
 * Storage layout:
 *   _sampleCounts[pixelIndex] = number of samples at that pixel
 *   _sampleOffsets[pixelIndex] = starting index in _sampleData for that pixel's data
 *   _sampleData = flat array of all samples, each sample = _numChannels floats
 *
 * Samples within a pixel are expected to be sorted front-to-back by Z.
 */
class DeepImage
{
public:

    DeepImage();

    DeepImage(const RectI& dataWindow,
              int numChannels,
              const std::vector<std::string>& channelNames);

    ~DeepImage();

    // -- Setup --

    /**
     * @brief Set the number of samples for a given pixel.
     * Must be called for all pixels before allocateFromSampleCounts().
     */
    void setSampleCount(int x, int y, int count);

    /**
     * @brief After all sample counts are set, allocate the contiguous data buffer
     * and compute the offset table.
     */
    void allocateFromSampleCounts();

    // -- Accessors --

    const RectI& getDataWindow() const { return _dataWindow; }

    int getNumChannels() const { return _numChannels; }

    const std::vector<std::string>& getChannelNames() const { return _channelNames; }

    int getSampleCount(int x, int y) const;

    /**
     * @brief Returns a pointer to the first float of the first sample
     * at pixel (x,y). The data for this pixel is laid out as:
     *   [sample0_ch0, sample0_ch1, ..., sample0_chN,
     *    sample1_ch0, sample1_ch1, ..., sample1_chN, ...]
     * Returns nullptr if no samples at this pixel.
     */
    float* getSampleData(int x, int y);
    const float* getSampleData(int x, int y) const;

    std::size_t totalSamples() const { return _totalSamples; }

    std::size_t sizeInBytes() const;

    // -- Channel index helpers --

    /**
     * @brief Find the index of a named channel, or -1 if not found.
     */
    int findChannelIndex(const std::string& name) const;

    // -- Flattening --

    /**
     * @brief Flatten the deep image into a flat RGBA image by front-to-back
     * compositing of all samples. Writes into the provided output buffer.
     *
     * @param roi Region to flatten (must be within dataWindow)
     * @param outputRGBA Pointer to float buffer with stride (roi.width() * 4) per row
     * @param outputRowBytes Number of bytes per output row
     */
    void flattenToRGBA(const RectI& roi,
                       float* outputRGBA,
                       int outputRowBytes) const;

    /**
     * @brief Flatten the deep image and copy to a Natron Image, handling Y-flip.
     *
     * Deep images from OIIO are stored top-down (y=0 at top).
     * Natron's Image class is bottom-up (y=0 at bottom).
     * This method handles the coordinate transformation automatically.
     *
     * @param outImg The Natron Image to write the flattened result into
     */
    void flattenToImage(Image* outImg) const;

    // -- Thread safety --

    QReadWriteLock& getLock() { return _lock; }

private:

    int pixelIndex(int x, int y) const
    {
        return (y - _dataWindow.y1) * _dataWindow.width() + (x - _dataWindow.x1);
    }

    RectI _dataWindow;
    int _numChannels;
    std::vector<std::string> _channelNames;

    // Per-pixel sample counts and offsets into _sampleData
    std::vector<int> _sampleCounts;
    std::vector<std::size_t> _sampleOffsets;

    // All sample data, contiguous: pixel0_samples | pixel1_samples | ...
    std::vector<float> _sampleData;
    std::size_t _totalSamples;

    mutable QReadWriteLock _lock;
};

typedef std::shared_ptr<DeepImage> DeepImagePtr;
typedef std::shared_ptr<const DeepImage> DeepImageConstPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEEPIMAGE_H
