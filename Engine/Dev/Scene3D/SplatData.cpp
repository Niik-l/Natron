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

#include "SplatData.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <sstream>

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QString>

NATRON_NAMESPACE_ENTER

namespace {

const float kSH_C0 = 0.28209479177387814f;

enum PlyType
{
    ePlyInt8, ePlyUInt8, ePlyInt16, ePlyUInt16, ePlyInt32, ePlyUInt32, ePlyFloat32, ePlyFloat64, ePlyUnknown
};

struct PlyProp
{
    PlyType type = ePlyUnknown;
    std::size_t offset = 0;
};

PlyType
parsePlyType(const std::string& s, std::size_t* size)
{
    if (s == "char"   || s == "int8")    { *size = 1; return ePlyInt8; }
    if (s == "uchar"  || s == "uint8")   { *size = 1; return ePlyUInt8; }
    if (s == "short"  || s == "int16")   { *size = 2; return ePlyInt16; }
    if (s == "ushort" || s == "uint16")  { *size = 2; return ePlyUInt16; }
    if (s == "int"    || s == "int32")   { *size = 4; return ePlyInt32; }
    if (s == "uint"   || s == "uint32")  { *size = 4; return ePlyUInt32; }
    if (s == "float"  || s == "float32") { *size = 4; return ePlyFloat32; }
    if (s == "double" || s == "float64") { *size = 8; return ePlyFloat64; }
    *size = 0;
    return ePlyUnknown;
}

// Read one property of a row as a float. Files are little-endian, as is every
// platform this builds for.
inline float
readProp(const char* row, const PlyProp& p)
{
    const char* at = row + p.offset;
    switch (p.type) {
        case ePlyFloat32: { float v;    std::memcpy(&v, at, 4); return v; }
        case ePlyFloat64: { double v;   std::memcpy(&v, at, 8); return (float)v; }
        case ePlyUInt8:   { return (float)(unsigned char)*at; }
        case ePlyInt8:    { return (float)(signed char)*at; }
        case ePlyInt16:   { int16_t v;  std::memcpy(&v, at, 2); return (float)v; }
        case ePlyUInt16:  { uint16_t v; std::memcpy(&v, at, 2); return (float)v; }
        case ePlyInt32:   { int32_t v;  std::memcpy(&v, at, 4); return (float)v; }
        case ePlyUInt32:  { uint32_t v; std::memcpy(&v, at, 4); return (float)v; }
        default: break;
    }
    return 0.f;
}

inline float
srgbToLinearExact(float c)
{
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow( (c + 0.055f) / 1.055f, 2.4f );
}

// sRGB to linear for millions of values per render: a 4096-step table with linear
// interpolation over [0, 1] (error below 1e-6), the exact curve above 1.
struct SrgbTable
{
    enum { kSize = 4096 };
    float v[kSize + 2];
    SrgbTable()
    {
        for (int i = 0; i <= kSize + 1; ++i) v[i] = srgbToLinearExact( (float)i / (float)kSize );
    }
    float operator()(float c) const
    {
        if (c >= 1.f) return srgbToLinearExact(c);
        const float f = c * (float)kSize;
        const int i = (int)f;
        return v[i] + (v[i + 1] - v[i]) * ( f - (float)i );
    }
};

std::atomic<unsigned long long> g_splatVersion(0);

} // namespace

float
SplatData::sizeOf(std::size_t i) const
{
    const float* s = &scale[i * 3];
    return std::cbrt(s[0] * s[1] * s[2]);
}

SplatDataPtr
readSplatPly(const std::string& path, int maxShDegree, std::string* error)
{
    std::string dummy;
    std::string& err = error ? *error : dummy;
    err.clear();
    maxShDegree = std::max( 0, std::min(3, maxShDegree) );

    QFile f( QString::fromUtf8( path.c_str() ) );
    if ( !f.open(QIODevice::ReadOnly) ) {
        err = "Cannot open the file: " + path;
        return SplatDataPtr();
    }

    // ---- Header ----
    std::map<std::string, PlyProp> props;
    std::size_t rowSize = 0;
    std::size_t vertexCount = 0;
    bool sawPly = false, sawFormat = false, sawEnd = false, binaryLE = false;
    bool inVertex = false, vertexDone = false, vertexIsFirst = true, sawOtherElementFirst = false;
    std::string badProp;
    for (int lineNo = 0; lineNo < 4096 && !f.atEnd(); ++lineNo) {
        const QByteArray raw = f.readLine(4096);
        std::istringstream ls( std::string( raw.constData(), (std::size_t)raw.size() ) );
        std::string word;
        if ( !(ls >> word) ) continue;
        if (lineNo == 0) {
            sawPly = (word == "ply");
            if (!sawPly) break;
            continue;
        }
        if (word == "format") {
            std::string fmt;
            ls >> fmt;
            sawFormat = true;
            binaryLE = (fmt == "binary_little_endian");
        } else if (word == "element") {
            std::string name;
            unsigned long long n = 0;
            ls >> name >> n;
            if (name == "vertex" && !vertexDone && !inVertex) {
                inVertex = true;
                vertexCount = (std::size_t)n;
                vertexIsFirst = !sawOtherElementFirst;
            } else {
                if (inVertex) { inVertex = false; vertexDone = true; }
                else if (!vertexDone) sawOtherElementFirst = true;
            }
        } else if (word == "property" && inVertex) {
            std::string type, name;
            ls >> type;
            if (type == "list") {
                badProp = "a list property in the vertex element";
                continue;
            }
            ls >> name;
            std::size_t sz = 0;
            PlyProp p;
            p.type = parsePlyType(type, &sz);
            if (p.type == ePlyUnknown) {
                badProp = "an unknown property type '" + type + "'";
                continue;
            }
            p.offset = rowSize;
            rowSize += sz;
            props[name] = p;
        } else if (word == "end_header") {
            sawEnd = true;
            break;
        }
    }
    if (!sawPly) {
        err = "Not a PLY file (the first line is not 'ply'): " + path;
        return SplatDataPtr();
    }
    if (!sawEnd || !sawFormat) {
        err = "The PLY header is incomplete (no 'format' or no 'end_header' line).";
        return SplatDataPtr();
    }
    if (!binaryLE) {
        err = "Only binary little-endian PLY files are supported (this one is ASCII or big-endian). "
              "3DGS trainers write binary little-endian.";
        return SplatDataPtr();
    }
    if ( !badProp.empty() ) {
        err = "Unsupported PLY: " + badProp + ".";
        return SplatDataPtr();
    }
    if (!vertexIsFirst) {
        err = "Unsupported PLY: the 'vertex' element is not the first element in the file.";
        return SplatDataPtr();
    }
    if (vertexCount == 0 || rowSize == 0) {
        err = "The PLY file has no vertices.";
        return SplatDataPtr();
    }
    if ( !props.count("x") || !props.count("y") || !props.count("z") ) {
        err = "The PLY file has no x / y / z properties.";
        return SplatDataPtr();
    }

    const qint64 dataStart = f.pos();
    const unsigned long long needBytes = (unsigned long long)vertexCount * (unsigned long long)rowSize;
    if ( (unsigned long long)(f.size() - dataStart) < needBytes ) {
        std::ostringstream os;
        os << "The PLY file is truncated: the header announces " << vertexCount << " vertices of "
           << rowSize << " bytes, but only " << (f.size() - dataStart) << " bytes follow it.";
        err = os.str();
        return SplatDataPtr();
    }

    // ---- Which properties are there ----
    auto find = [&props](const std::string& n, PlyProp* out) -> bool {
        std::map<std::string, PlyProp>::const_iterator it = props.find(n);
        if ( it == props.end() ) return false;
        *out = it->second;
        return true;
    };
    PlyProp pX, pY, pZ, pScale[3], pRot[4], pOpacity, pDc[3], pRgb[3];
    find("x", &pX); find("y", &pY); find("z", &pZ);
    const bool hasScale = find("scale_0", &pScale[0]) && find("scale_1", &pScale[1]) && find("scale_2", &pScale[2]);
    const bool hasRot = find("rot_0", &pRot[0]) && find("rot_1", &pRot[1]) && find("rot_2", &pRot[2]) && find("rot_3", &pRot[3]);
    const bool hasOpacity = find("opacity", &pOpacity);
    const bool hasDc = find("f_dc_0", &pDc[0]) && find("f_dc_1", &pDc[1]) && find("f_dc_2", &pDc[2]);
    const bool hasRgb = !hasDc && find("red", &pRgb[0]) && find("green", &pRgb[1]) && find("blue", &pRgb[2]);
    const bool rgbIsByte = hasRgb && (pRgb[0].type == ePlyUInt8);
    const bool hasGaussians = hasScale && hasRot && hasOpacity;

    // f_rest_0 .. f_rest_(3K-1): K coefficients per colour channel, red first.
    std::vector<PlyProp> pRest;
    for (int i = 0; i < 45; ++i) {
        PlyProp p;
        std::ostringstream n;
        n << "f_rest_" << i;
        if ( !find(n.str(), &p) ) break;
        pRest.push_back(p);
    }
    int fileShDegree = 0;
    switch ( pRest.size() ) {
        case 9:  fileShDegree = 1; break;
        case 24: fileShDegree = 2; break;
        case 45: fileShDegree = 3; break;
        default: fileShDegree = 0; break;   // none, or a count that is no whole SH band
    }
    const int fileK = (fileShDegree + 1) * (fileShDegree + 1) - 1;
    const int keepDegree = hasDc ? std::min(maxShDegree, fileShDegree) : 0;
    const int keepK = (keepDegree + 1) * (keepDegree + 1) - 1;
    // The usual layout is 45 consecutive float32: copy each channel's run in one go.
    bool restContiguous = keepK > 0;
    for (std::size_t i = 0; restContiguous && i < pRest.size(); ++i) {
        if (pRest[i].type != ePlyFloat32 || pRest[i].offset != pRest[0].offset + i * 4) restContiguous = false;
    }

    // ---- Read ----
    std::shared_ptr<SplatData> d = std::make_shared<SplatData>();
    try {
        d->pos.resize(vertexCount * 3);
        d->scale.resize(vertexCount * 3);
        d->rot.resize(vertexCount * 4);
        d->opacity.resize(vertexCount);
        d->dc.resize(vertexCount * 3);
        if (keepK > 0) d->shRest.resize(vertexCount * 3 * (std::size_t)keepK);
    } catch (const std::bad_alloc&) {
        std::ostringstream os;
        os << "Not enough memory for " << vertexCount << " splats. Lower SH Degree, or use a smaller scene.";
        err = os.str();
        return SplatDataPtr();
    }
    d->shDegree = keepDegree;
    d->fileShDegree = fileShDegree;
    d->hasGaussians = hasGaussians;

    const std::size_t rowsPerChunk = std::max<std::size_t>( 1, (std::size_t)(4u << 20) / rowSize );
    std::vector<char> chunk(rowsPerChunk * rowSize);
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    std::size_t out = 0;
    std::size_t done = 0;
    while (done < vertexCount) {
        const std::size_t rows = std::min(rowsPerChunk, vertexCount - done);
        const qint64 want = (qint64)(rows * rowSize);
        qint64 got = 0;
        while (got < want) {
            const qint64 r = f.read(chunk.data() + got, want - got);
            if (r <= 0) break;
            got += r;
        }
        if (got != want) {
            err = "Read error part-way through the PLY file.";
            return SplatDataPtr();
        }
        for (std::size_t r = 0; r < rows; ++r) {
            const char* row = chunk.data() + r * rowSize;
            const float x = readProp(row, pX), y = readProp(row, pY), z = readProp(row, pZ);
            float sc[3] = {1.f, 1.f, 1.f};
            float q[4] = {1.f, 0.f, 0.f, 0.f};
            float op = 1.f;
            if (hasGaussians) {
                for (int k = 0; k < 3; ++k) sc[k] = std::exp( readProp(row, pScale[k]) );
                for (int k = 0; k < 4; ++k) q[k] = readProp(row, pRot[k]);
                const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (ql > 1e-12f) { for (int k = 0; k < 4; ++k) q[k] /= ql; }
                else { q[0] = 1.f; q[1] = q[2] = q[3] = 0.f; }
                op = 1.f / ( 1.f + std::exp( -readProp(row, pOpacity) ) );
            }
            float dc[3] = {(1.f - 0.5f) / kSH_C0, (1.f - 0.5f) / kSH_C0, (1.f - 0.5f) / kSH_C0};  // white
            if (hasDc) {
                for (int k = 0; k < 3; ++k) dc[k] = readProp(row, pDc[k]);
            } else if (hasRgb) {
                for (int k = 0; k < 3; ++k) {
                    const float c = readProp(row, pRgb[k]) * (rgbIsByte ? (1.f / 255.f) : 1.f);
                    dc[k] = (c - 0.5f) / kSH_C0;
                }
            }
            const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
                                && std::isfinite(sc[0]) && std::isfinite(sc[1]) && std::isfinite(sc[2])
                                && std::isfinite(op)
                                && std::isfinite(dc[0]) && std::isfinite(dc[1]) && std::isfinite(dc[2]);
            if (!finite) {
                ++d->droppedNonFinite;
                continue;
            }
            float* P = &d->pos[out * 3];
            P[0] = x; P[1] = y; P[2] = z;
            std::memcpy(&d->scale[out * 3], sc, sizeof(sc));
            std::memcpy(&d->rot[out * 4], q, sizeof(q));
            d->opacity[out] = op;
            std::memcpy(&d->dc[out * 3], dc, sizeof(dc));
            if (keepK > 0) {
                float* R = &d->shRest[out * 3 * (std::size_t)keepK];
                for (int c = 0; c < 3; ++c) {
                    if (restContiguous) {
                        std::memcpy(R + c * keepK, row + pRest[(std::size_t)(c * fileK)].offset, (std::size_t)keepK * 4);
                    } else {
                        for (int k = 0; k < keepK; ++k) R[c * keepK + k] = readProp(row, pRest[(std::size_t)(c * fileK + k)]);
                    }
                }
            }
            mn[0] = std::min(mn[0], x); mn[1] = std::min(mn[1], y); mn[2] = std::min(mn[2], z);
            mx[0] = std::max(mx[0], x); mx[1] = std::max(mx[1], y); mx[2] = std::max(mx[2], z);
            ++out;
        }
        done += rows;
    }
    if (out == 0) {
        err = "The PLY file holds no usable points (every value is NaN or infinite).";
        return SplatDataPtr();
    }
    d->count = out;
    if (out != vertexCount) {
        d->pos.resize(out * 3);
        d->scale.resize(out * 3);
        d->rot.resize(out * 4);
        d->opacity.resize(out);
        d->dc.resize(out * 3);
        if (keepK > 0) d->shRest.resize(out * 3 * (std::size_t)keepK);
    }
    for (int k = 0; k < 3; ++k) { d->bboxMin[k] = mn[k]; d->bboxMax[k] = mx[k]; }

    if (!hasGaussians) {
        // A plain point cloud carries no size: use the spacing points would have if
        // they tiled a surface the size of the bounding box.
        const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
        const float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float s = std::max( 1e-6f, 0.5f * diag / std::sqrt( (float)out ) );
        std::fill(d->scale.begin(), d->scale.end(), s);
        d->medianSize = s;
    } else {
        const std::size_t step = out / 200000 + 1;
        std::vector<float> sample;
        sample.reserve(out / step + 1);
        for (std::size_t i = 0; i < out; i += step) sample.push_back( d->sizeOf(i) );
        std::nth_element( sample.begin(), sample.begin() + sample.size() / 2, sample.end() );
        d->medianSize = sample[sample.size() / 2];
    }

    d->version = ++g_splatVersion;
    return d;
}

std::size_t
buildSplatPreviewPoints(const SplatData& data,
                        float opacityThreshold,
                        std::size_t maxPoints,
                        float sizeClamp,
                        bool linearColor,
                        std::vector<float>& outXyz,
                        std::vector<float>& outRgb,
                        std::vector<float>& outDiameter)
{
    outXyz.clear();
    outRgb.clear();
    outDiameter.clear();
    const std::size_t n = data.count;
    if (n == 0) return 0;

    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (data.opacity[i] >= opacityThreshold) ++kept;
    }
    if (kept == 0) return 0;
    const std::size_t take = (maxPoints > 0 && kept > maxPoints) ? maxPoints : kept;

    outXyz.reserve(take * 3);
    outRgb.reserve(take * 3);
    outDiameter.reserve(take);
    const float maxDiameter = (sizeClamp > 0.f && data.medianSize > 0.f) ? 2.f * data.medianSize * sizeClamp : 0.f;

    static const SrgbTable srgbToLinear;

    // Even subset: of the `kept` candidates, emit exactly `take`, spread across the file.
    std::size_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (data.opacity[i] < opacityThreshold) continue;
        acc += take;
        if (acc < kept) continue;
        acc -= kept;

        const float* P = &data.pos[i * 3];
        outXyz.push_back(P[0]); outXyz.push_back(P[1]); outXyz.push_back(P[2]);
        for (int k = 0; k < 3; ++k) {
            float c = std::max(0.f, 0.5f + kSH_C0 * data.dc[i * 3 + k]);
            if (linearColor) c = srgbToLinear(c);
            outRgb.push_back(c);
        }
        float dia = 2.f * data.sizeOf(i);
        if (maxDiameter > 0.f) dia = std::min(dia, maxDiameter);
        outDiameter.push_back(dia);
    }
    return outDiameter.size();
}

NATRON_NAMESPACE_EXIT
