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

#include "DeepImage.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#include "../../Image.h"

NATRON_NAMESPACE_ENTER

DeepImage::DeepImage()
    : _dataWindow()
    , _numChannels(0)
    , _channelNames()
    , _sampleCounts()
    , _sampleOffsets()
    , _sampleData()
    , _totalSamples(0)
    , _lock()
{
}

DeepImage::DeepImage(const RectI& dataWindow,
                     int numChannels,
                     const std::vector<std::string>& channelNames)
    : _dataWindow(dataWindow)
    , _numChannels(numChannels)
    , _channelNames(channelNames)
    , _sampleCounts()
    , _sampleOffsets()
    , _sampleData()
    , _totalSamples(0)
    , _lock()
{
    assert(numChannels > 0);
    assert((int)channelNames.size() == numChannels);

    int numPixels = _dataWindow.area();
    _sampleCounts.resize(numPixels, 0);
    _sampleOffsets.resize(numPixels, 0);
}

DeepImage::~DeepImage()
{
}

void
DeepImage::setSampleCount(int x, int y, int count)
{
    assert(_dataWindow.contains(x, y));
    _sampleCounts[pixelIndex(x, y)] = count;
}

void
DeepImage::allocateFromSampleCounts()
{
    int numPixels = (int)_sampleCounts.size();
    _totalSamples = 0;

    for (int i = 0; i < numPixels; ++i) {
        _sampleOffsets[i] = _totalSamples * _numChannels;
        _totalSamples += _sampleCounts[i];
    }

    _sampleData.resize(_totalSamples * _numChannels, 0.0f);
}

int
DeepImage::getSampleCount(int x, int y) const
{
    if (!_dataWindow.contains(x, y)) {
        return 0;
    }
    return _sampleCounts[pixelIndex(x, y)];
}

float*
DeepImage::getSampleData(int x, int y)
{
    if (!_dataWindow.contains(x, y)) {
        return nullptr;
    }
    int idx = pixelIndex(x, y);
    if (_sampleCounts[idx] == 0) {
        return nullptr;
    }
    return &_sampleData[_sampleOffsets[idx]];
}

const float*
DeepImage::getSampleData(int x, int y) const
{
    if (!_dataWindow.contains(x, y)) {
        return nullptr;
    }
    int idx = pixelIndex(x, y);
    if (_sampleCounts[idx] == 0) {
        return nullptr;
    }
    return &_sampleData[_sampleOffsets[idx]];
}

std::size_t
DeepImage::sizeInBytes() const
{
    return _sampleData.size() * sizeof(float)
           + _sampleCounts.size() * sizeof(int)
           + _sampleOffsets.size() * sizeof(std::size_t);
}

int
DeepImage::findChannelIndex(const std::string& name) const
{
    for (int i = 0; i < (int)_channelNames.size(); ++i) {
        if (_channelNames[i] == name) {
            return i;
        }
    }
    return -1;
}

void
DeepImage::flattenToRGBA(const RectI& roi,
                          float* outputRGBA,
                          int outputRowBytes) const
{
    // Find channel indices for R, G, B, A
    int rIdx = findChannelIndex("R");
    int gIdx = findChannelIndex("G");
    int bIdx = findChannelIndex("B");
    int aIdx = findChannelIndex("A");

    // Also check for Z — samples should be sorted by Z (front to back)
    // but we just composite in order as stored

    RectI clipped = roi.intersect(_dataWindow);
    if (clipped.isNull()) {
        return;
    }

    for (int y = clipped.y1; y < clipped.y2; ++y) {
        float* outRow = (float*)((char*)outputRGBA + (y - roi.y1) * outputRowBytes);

        for (int x = clipped.x1; x < clipped.x2; ++x) {
            int outIdx = (x - roi.x1) * 4;
            int pIdx = pixelIndex(x, y);
            int nSamples = _sampleCounts[pIdx];

            // Initialize to transparent black
            float accR = 0.0f;
            float accG = 0.0f;
            float accB = 0.0f;
            float accA = 0.0f;

            if (nSamples > 0) {
                const float* data = &_sampleData[_sampleOffsets[pIdx]];

                // Front-to-back "under" compositing
                for (int s = 0; s < nSamples; ++s) {
                    const float* sample = data + s * _numChannels;

                    float sR = (rIdx >= 0) ? sample[rIdx] : 0.0f;
                    float sG = (gIdx >= 0) ? sample[gIdx] : 0.0f;
                    float sB = (bIdx >= 0) ? sample[bIdx] : 0.0f;
                    float sA = (aIdx >= 0) ? sample[aIdx] : 1.0f;

                    // Standard "under" operator: dst = dst + src * (1 - dst.a)
                    float oneMinusAccA = 1.0f - accA;
                    accR += sR * oneMinusAccA;
                    accG += sG * oneMinusAccA;
                    accB += sB * oneMinusAccA;
                    accA += sA * oneMinusAccA;

                    // Early out if fully opaque
                    if (accA >= 1.0f) {
                        accA = 1.0f;
                        break;
                    }
                }
            }

            outRow[outIdx + 0] = accR;
            outRow[outIdx + 1] = accG;
            outRow[outIdx + 2] = accB;
            outRow[outIdx + 3] = accA;
        }
    }
}

void
DeepImage::flattenToImage(Image* outImg) const
{
    if (!outImg) return;

    // Flatten into deep image's own data window
    int deepW = _dataWindow.width();
    int deepH = _dataWindow.height();

    if (deepW <= 0 || deepH <= 0) return;

    int rowBytes = deepW * 4 * sizeof(float);
    std::vector<float> flatBuf(deepW * deepH * 4, 0.0f);
    flattenToRGBA(_dataWindow, flatBuf.data(), rowBytes);

    // Copy to output Image with Y-flip
    // Deep images from OIIO are top-down (row 0 = top of image)
    // Natron's Image class is bottom-up (row y1 = bottom of image)
    RectI outBounds = outImg->getBounds();
    int outW = outBounds.width();
    int outH = outBounds.height();

    Image::WriteAccess wa(outImg);
    for (int y = outBounds.y1; y < outBounds.y2; ++y) {
        // Map output Y (bottom-up) to deep buffer Y (top-down)
        float normY = (float)(y - outBounds.y1) / std::max(1, outH - 1);
        int deepRow = (int)((1.0f - normY) * (deepH - 1) + 0.5f);
        deepRow = std::max(0, std::min(deepH - 1, deepRow));

        const float* srcRow = &flatBuf[deepRow * deepW * 4];

        for (int x = outBounds.x1; x < outBounds.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) continue;

            // Map output X to deep buffer X (handles resolution difference)
            float normX = (float)(x - outBounds.x1) / std::max(1, outW - 1);
            int deepCol = (int)(normX * (deepW - 1) + 0.5f);
            deepCol = std::max(0, std::min(deepW - 1, deepCol));

            int srcOff = deepCol * 4;
            dst[0] = srcRow[srcOff + 0];
            dst[1] = srcRow[srcOff + 1];
            dst[2] = srcRow[srcOff + 2];
            dst[3] = srcRow[srcOff + 3];
        }
    }
}

NATRON_NAMESPACE_EXIT
