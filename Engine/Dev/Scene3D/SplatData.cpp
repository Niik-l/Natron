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
#include <QtCore/QFileInfo>
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

// ============================================================ other formats

namespace {

// Bounding box, median size and version of a scene filled by a reader.
void finishScene(SplatData& d)
{
    const std::size_t n = d.count;
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], d.pos[i * 3 + k]);
            mx[k] = std::max(mx[k], d.pos[i * 3 + k]);
        }
    }
    for (int k = 0; k < 3; ++k) { d.bboxMin[k] = mn[k]; d.bboxMax[k] = mx[k]; }
    if (n > 0 && d.medianSize <= 0.f) {
        const std::size_t step = n / 200000 + 1;
        std::vector<float> sample;
        sample.reserve(n / step + 1);
        for (std::size_t i = 0; i < n; i += step) sample.push_back( d.sizeOf(i) );
        std::nth_element( sample.begin(), sample.begin() + sample.size() / 2, sample.end() );
        d.medianSize = sample[sample.size() / 2];
    }
    d.version = ++g_splatVersion;
}

bool readWholeFile(const std::string& path, QByteArray& out, std::string& err)
{
    QFile f( QString::fromUtf8( path.c_str() ) );
    if ( !f.open(QIODevice::ReadOnly) ) {
        err = "Cannot open the file: " + path;
        return false;
    }
    out = f.readAll();
    if ( out.size() != f.size() ) {
        err = "Read error (or not enough memory) reading " + path;
        return false;
    }
    return true;
}

// ---- inflate (RFC 1951) and the gzip wrapper (RFC 1952). Self-contained so
// reading .spz adds no library to the build; .spz files are a few tens of MB.
struct Inflater
{
    const unsigned char* in;
    std::size_t inLen, inPos = 0;
    uint64_t bitBuf = 0;
    int bitCnt = 0;
    std::vector<unsigned char>& out;
    bool overrun = false;

    Inflater(const unsigned char* p, std::size_t n, std::vector<unsigned char>& o) : in(p), inLen(n), out(o) {}

    int bits(int need)
    {
        while (bitCnt < need) {
            if (inPos >= inLen) { overrun = true; return 0; }
            bitBuf |= (uint64_t)in[inPos++] << bitCnt;
            bitCnt += 8;
        }
        const int v = (int)( bitBuf & ( ( (uint64_t)1 << need ) - 1 ) );
        bitBuf >>= need;
        bitCnt -= need;
        return v;
    }

    struct Huff { unsigned short count[16]; unsigned short symbol[288]; };

    static int construct(Huff& h, const unsigned short* length, int n)
    {
        for (int len = 0; len <= 15; ++len) h.count[len] = 0;
        for (int sym = 0; sym < n; ++sym) ++h.count[length[sym]];
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        unsigned short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = (unsigned short)(offs[len] + h.count[len]);
        for (int sym = 0; sym < n; ++sym) {
            if (length[sym] != 0) h.symbol[offs[length[sym]]++] = (unsigned short)sym;
        }
        return left;
    }

    int decode(const Huff& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= bits(1);
            if (overrun) return -1;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool codes(const Huff& lencode, const Huff& distcode)
    {
        static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 0) return false;
            if (symbol < 256) {
                out.push_back( (unsigned char)symbol );
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) return false;
                const int len = lens[symbol] + bits(lext[symbol]);
                symbol = decode(distcode);
                if (symbol < 0 || symbol >= 30) return false;
                const std::size_t dist = (std::size_t)dists[symbol] + (std::size_t)bits(dext[symbol]);
                if ( overrun || dist > out.size() ) return false;
                std::size_t from = out.size() - dist;
                for (int k = 0; k < len; ++k) out.push_back(out[from++]);
            }
        }
    }

    bool run()
    {
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (overrun) return false;
            if (type == 0) {
                bitBuf = 0;
                bitCnt = 0;
                if (inPos + 4 > inLen) return false;
                const unsigned len = in[inPos] | (in[inPos + 1] << 8);
                const unsigned nlen = in[inPos + 2] | (in[inPos + 3] << 8);
                if ( len != (~nlen & 0xFFFFu) ) return false;
                inPos += 4;
                if (inPos + len > inLen) return false;
                out.insert(out.end(), in + inPos, in + inPos + len);
                inPos += len;
            } else if (type == 1) {
                Huff lencode, distcode;
                unsigned short lengths[288];
                int sym = 0;
                for (; sym < 144; ++sym) lengths[sym] = 8;
                for (; sym < 256; ++sym) lengths[sym] = 9;
                for (; sym < 280; ++sym) lengths[sym] = 7;
                for (; sym < 288; ++sym) lengths[sym] = 8;
                construct(lencode, lengths, 288);
                for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
                construct(distcode, lengths, 30);
                if ( !codes(lencode, distcode) ) return false;
            } else if (type == 2) {
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
                if (overrun || nlen > 286 || ndist > 30) return false;
                unsigned short lengths[320];
                int index = 0;
                for (; index < ncode; ++index) lengths[order[index]] = (unsigned short)bits(3);
                for (; index < 19; ++index) lengths[order[index]] = 0;
                Huff lencode, distcode;
                if (construct(lencode, lengths, 19) != 0) return false;
                index = 0;
                while (index < nlen + ndist) {
                    int symbol = decode(lencode);
                    if (symbol < 0) return false;
                    if (symbol < 16) {
                        lengths[index++] = (unsigned short)symbol;
                    } else {
                        int len = 0, rep;
                        if (symbol == 16) {
                            if (index == 0) return false;
                            len = lengths[index - 1];
                            rep = 3 + bits(2);
                        } else if (symbol == 17) {
                            rep = 3 + bits(3);
                        } else {
                            rep = 11 + bits(7);
                        }
                        if (index + rep > nlen + ndist) return false;
                        while (rep--) lengths[index++] = (unsigned short)len;
                    }
                }
                if (lengths[256] == 0) return false;
                int err = construct(lencode, lengths, nlen);
                if ( err < 0 || (err > 0 && nlen - lencode.count[0] != 1) ) return false;
                err = construct(distcode, lengths + nlen, ndist);
                if ( err < 0 || (err > 0 && ndist - distcode.count[0] != 1) ) return false;
                if ( !codes(lencode, distcode) ) return false;
            } else {
                return false;
            }
        } while (!last);
        return !overrun;
    }
};

bool gunzip(const QByteArray& file, std::vector<unsigned char>& out, std::string& err)
{
    const unsigned char* p = (const unsigned char*)file.constData();
    const std::size_t n = (std::size_t)file.size();
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) {
        err = "Not a gzip stream.";
        return false;
    }
    const int flags = p[3];
    std::size_t pos = 10;
    if (flags & 4) {
        if (pos + 2 > n) { err = "Truncated gzip header."; return false; }
        pos += 2 + ( p[pos] | (p[pos + 1] << 8) );
    }
    if (flags & 8)  { while (pos < n && p[pos]) ++pos; ++pos; }
    if (flags & 16) { while (pos < n && p[pos]) ++pos; ++pos; }
    if (flags & 2)  pos += 2;
    if (pos + 8 > n) { err = "Truncated gzip file."; return false; }
    const uint32_t isize = (uint32_t)p[n - 4] | ( (uint32_t)p[n - 3] << 8 ) | ( (uint32_t)p[n - 2] << 16 ) | ( (uint32_t)p[n - 1] << 24 );
    try {
        out.reserve( (std::size_t)isize + 16 );
        Inflater inf(p + pos, n - pos - 8, out);
        if ( !inf.run() ) {
            err = "The compressed data is damaged.";
            return false;
        }
    } catch (const std::bad_alloc&) {
        err = "Not enough memory to decompress the file.";
        return false;
    }
    if ( (uint32_t)out.size() != isize ) {
        err = "The compressed data is damaged (wrong size after decompression).";
        return false;
    }
    return true;
}

inline uint32_t rdU32(const unsigned char* p)
{
    return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

inline float halfBitsToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ( !(man & 0x400u) ) { man <<= 1; --exp; }
            man &= 0x3FFu;
            bits = sign | ( (exp + 112u) << 23 ) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ( (exp + 112u) << 23 ) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool allocScene(SplatData& d, std::size_t n, int keepK, std::string& err)
{
    try {
        d.pos.resize(n * 3);
        d.scale.resize(n * 3);
        d.rot.resize(n * 4);
        d.opacity.resize(n);
        d.dc.resize(n * 3);
        if (keepK > 0) d.shRest.resize(n * 3 * (std::size_t)keepK);
    } catch (const std::bad_alloc&) {
        std::ostringstream os;
        os << "Not enough memory for " << n << " splats. Lower SH Degree, or use a smaller scene.";
        err = os.str();
        return false;
    }
    d.count = n;
    return true;
}

// ---- .splat (antimatter15): 32 bytes per splat, no header:
//   position 3 x float32, scale 3 x float32 (linear), colour RGBA 4 x uint8 (rgb = display
//   colour, a = opacity), rotation 4 x uint8 ((b - 128) / 128, order w x y z).
SplatDataPtr readDotSplat(const std::string& path, std::string& err)
{
    QByteArray file;
    if ( !readWholeFile(path, file, err) ) return SplatDataPtr();
    const std::size_t n = (std::size_t)file.size() / 32;
    if ( n == 0 || (std::size_t)file.size() % 32 != 0 ) {
        err = "Not a .splat file: its size is not a whole number of 32-byte splats.";
        return SplatDataPtr();
    }
    std::shared_ptr<SplatData> d = std::make_shared<SplatData>();
    if ( !allocScene(*d, n, 0, err) ) return SplatDataPtr();
    const unsigned char* p = (const unsigned char*)file.constData();
    std::size_t out = 0;
    for (std::size_t i = 0; i < n; ++i, p += 32) {
        float v[6];
        std::memcpy(v, p, 24);
        bool finite = true;
        for (int k = 0; k < 6; ++k) finite = finite && std::isfinite(v[k]);
        if (!finite) { ++d->droppedNonFinite; continue; }
        std::memcpy(&d->pos[out * 3], v, 12);
        std::memcpy(&d->scale[out * 3], v + 3, 12);
        for (int k = 0; k < 3; ++k) d->dc[out * 3 + k] = ( (float)p[24 + k] / 255.f - 0.5f ) / kSH_C0;
        d->opacity[out] = (float)p[27] / 255.f;
        float q[4];
        for (int k = 0; k < 4; ++k) q[k] = ( (float)p[28 + k] - 128.f ) / 128.f;
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (ql > 1e-6f) { for (int k = 0; k < 4; ++k) q[k] /= ql; }
        else { q[0] = 1.f; q[1] = q[2] = q[3] = 0.f; }
        std::memcpy(&d->rot[out * 4], q, 16);
        ++out;
    }
    if (out == 0) { err = "The .splat file holds no usable splats."; return SplatDataPtr(); }
    if (out != n) {
        d->count = out;
        d->pos.resize(out * 3); d->scale.resize(out * 3); d->rot.resize(out * 4);
        d->opacity.resize(out); d->dc.resize(out * 3);
    }
    d->hasGaussians = true;
    finishScene(*d);
    return d;
}

// ---- .spz (Niantic), versions 1 to 3: one gzip stream holding a 16-byte header and
// one array per attribute, in this order: positions, alphas, colours, scales,
// rotations, spherical harmonics. Decoding follows the reference library
// (nianticlabs/spz, MIT): see the formulas beside each array below.
SplatDataPtr readSpz(const std::string& path, int maxShDegree, std::string& err)
{
    QByteArray file;
    if ( !readWholeFile(path, file, err) ) return SplatDataPtr();
    if ( file.size() >= 8 && rdU32( (const unsigned char*)file.constData() ) == 0x5053474eu ) {
        std::ostringstream os;
        os << "This is an SPZ version " << rdU32( (const unsigned char*)file.constData() + 4 )
           << " file. Versions 4 and later are compressed with zstd, which this build cannot read yet; "
              "versions 1 to 3 are supported. Convert the scene to .ply, or to an older SPZ version.";
        err = os.str();
        return SplatDataPtr();
    }
    std::vector<unsigned char> raw;
    if ( !gunzip(file, raw, err) ) {
        err = "Cannot read the SPZ file. " + err;
        return SplatDataPtr();
    }
    file.clear();
    if ( raw.size() < 16 || rdU32( raw.data() ) != 0x5053474eu ) {
        err = "Not an SPZ file (wrong signature).";
        return SplatDataPtr();
    }
    const uint32_t version = rdU32(raw.data() + 4);
    const std::size_t n = rdU32(raw.data() + 8);
    const int fileDeg = raw[12];
    const int fracBits = raw[13];
    if (version < 1 || version > 3) {
        std::ostringstream os;
        os << "Unsupported SPZ version " << version << ".";
        err = os.str();
        return SplatDataPtr();
    }
    static const int kDim[5] = {0, 3, 8, 15, 24};
    if (fileDeg > 4 || n == 0) { err = "The SPZ header is not valid."; return SplatDataPtr(); }
    const std::size_t fileK = (std::size_t)kDim[fileDeg];
    const std::size_t posBytes = (version == 1) ? 6 : 9;
    const std::size_t rotBytes = (version >= 3) ? 4 : 3;
    const std::size_t need = 16 + n * (posBytes + 1 + 3 + 3 + rotBytes + fileK * 3);
    if (raw.size() < need) {
        err = "The SPZ file is truncated.";
        return SplatDataPtr();
    }
    const unsigned char* pPos = raw.data() + 16;
    const unsigned char* pAlpha = pPos + n * posBytes;
    const unsigned char* pCol = pAlpha + n;
    const unsigned char* pScale = pCol + n * 3;
    const unsigned char* pRot = pScale + n * 3;
    const unsigned char* pSh = pRot + n * rotBytes;

    const int keepDeg = std::max( 0, std::min( std::min(maxShDegree, 3), fileDeg ) );
    const int keepK = (keepDeg + 1) * (keepDeg + 1) - 1;
    std::shared_ptr<SplatData> d = std::make_shared<SplatData>();
    if ( !allocScene(*d, n, keepK, err) ) return SplatDataPtr();

    const float posScale = 1.f / (float)(1 << fracBits);
    for (std::size_t i = 0; i < n; ++i) {
        // position: 24-bit signed fixed point (version 1: float16)
        for (int k = 0; k < 3; ++k) {
            if (version == 1) {
                const unsigned char* b = pPos + i * 6 + k * 2;
                d->pos[i * 3 + k] = halfBitsToFloat( (uint16_t)( b[0] | (b[1] << 8) ) );
            } else {
                const unsigned char* b = pPos + i * 9 + k * 3;
                int32_t fixed = (int32_t)b[0] | ( (int32_t)b[1] << 8 ) | ( (int32_t)b[2] << 16 );
                if (fixed & 0x800000) fixed |= (int32_t)0xff000000;
                d->pos[i * 3 + k] = (float)fixed * posScale;
            }
        }
        d->opacity[i] = (float)pAlpha[i] / 255.f;                       // stored sigmoid(opacity) * 255
        for (int k = 0; k < 3; ++k) {
            d->dc[i * 3 + k] = ( (float)pCol[i * 3 + k] / 255.f - 0.5f ) / 0.15f;        // SH band 0, colorScale 0.15
            d->scale[i * 3 + k] = std::exp( (float)pScale[i * 3 + k] / 16.f - 10.f );    // log scale
        }
        // rotation, stored x y z (w); ours is w x y z
        float q[4];   // x y z w
        if (version >= 3) {
            const unsigned char* b = pRot + i * 4;
            uint32_t comp = rdU32(b);
            const int iLargest = (int)(comp >> 30);
            float sum = 0.f;
            for (int k = 3; k >= 0; --k) {
                if (k == iLargest) continue;
                const uint32_t mag = comp & 511u;
                const uint32_t neg = (comp >> 9) & 1u;
                comp >>= 10;
                q[k] = 0.70710678f * (float)mag / 511.f;
                if (neg) q[k] = -q[k];
                sum += q[k] * q[k];
            }
            q[iLargest] = std::sqrt( std::max(0.f, 1.f - sum) );
        } else {
            const unsigned char* b = pRot + i * 3;
            for (int k = 0; k < 3; ++k) q[k] = (float)b[k] / 127.5f - 1.f;
            q[3] = std::sqrt( std::max(0.f, 1.f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2]) );
        }
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (ql > 1e-6f) { for (int k = 0; k < 4; ++k) q[k] /= ql; }
        else { q[0] = q[1] = q[2] = 0.f; q[3] = 1.f; }
        d->rot[i * 4] = q[3]; d->rot[i * 4 + 1] = q[0]; d->rot[i * 4 + 2] = q[1]; d->rot[i * 4 + 3] = q[2];
        // SH: the file has, per splat, coefficient after coefficient with r g b together;
        // ours is all red coefficients, then green, then blue. Value = (byte - 128) / 128.
        if (keepK > 0) {
            const unsigned char* b = pSh + i * fileK * 3;
            float* R = &d->shRest[i * 3 * (std::size_t)keepK];
            for (int k = 0; k < keepK; ++k) {
                for (int c = 0; c < 3; ++c) R[c * keepK + k] = ( (float)b[k * 3 + c] - 128.f ) / 128.f;
            }
        }
    }
    d->shDegree = keepDeg;
    d->fileShDegree = std::min(fileDeg, 3);
    d->hasGaussians = true;
    d->yUp = true;        // SPZ stores right-up-back; a .ply is right-down-forward
    finishScene(*d);
    return d;
}

} // namespace

SplatDataPtr
readSplatFile(const std::string& path, int maxShDegree, std::string* error)
{
    std::string dummy;
    std::string& err = error ? *error : dummy;
    err.clear();
    const std::string ext = QFileInfo( QString::fromUtf8( path.c_str() ) ).suffix().toLower().toStdString();
    if (ext == "splat") return readDotSplat(path, err);
    if (ext == "spz") return readSpz(path, maxShDegree, err);
    if (ext == "ksplat") {
        err = "The .ksplat format is not supported yet. Convert the scene to .ply, .spz (version 3 or older) or .splat.";
        return SplatDataPtr();
    }
    return readSplatPly(path, maxShDegree, error);
}

SplatDataPtr
filterSplats(const SplatData& data, const SplatFilter& f)
{
    const std::size_t n = data.count;
    const float flip = f.flipYZ ? -1.f : 1.f;
    const float half[3] = {0.5f * std::fabs(f.size[0]), 0.5f * std::fabs(f.size[1]), 0.5f * std::fabs(f.size[2])};
    const float r2 = f.radius * f.radius;
    const float maxSize = (f.maxSize > 0.f && data.medianSize > 0.f) ? f.maxSize * data.medianSize : 0.f;
    std::vector<unsigned char> keep(n);
    std::size_t m = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bool k = data.opacity[i] >= f.minOpacity;
        if (k && maxSize > 0.f) k = data.sizeOf(i) <= maxSize;
        if (k && f.cropMode != 0) {
            const float x = data.pos[i * 3] - f.center[0];
            const float y = flip * data.pos[i * 3 + 1] - f.center[1];
            const float z = flip * data.pos[i * 3 + 2] - f.center[2];
            bool inside;
            if (f.cropMode == 1) inside = std::fabs(x) <= half[0] && std::fabs(y) <= half[1] && std::fabs(z) <= half[2];
            else inside = x * x + y * y + z * z <= r2;
            k = (inside != f.invert);
        }
        keep[i] = k ? 1 : 0;
        if (k) ++m;
    }
    if (m == 0) return SplatDataPtr();

    std::shared_ptr<SplatData> d = std::make_shared<SplatData>();
    const std::size_t K = (std::size_t)data.shRestPerChannel();
    d->pos.resize(m * 3);
    d->scale.resize(m * 3);
    d->rot.resize(m * 4);
    d->opacity.resize(m);
    d->dc.resize(m * 3);
    if ( K > 0 && !data.shRest.empty() ) d->shRest.resize(m * 3 * K);
    std::size_t o = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        std::memcpy(&d->pos[o * 3], &data.pos[i * 3], 12);
        std::memcpy(&d->scale[o * 3], &data.scale[i * 3], 12);
        std::memcpy(&d->rot[o * 4], &data.rot[i * 4], 16);
        d->opacity[o] = data.opacity[i];
        std::memcpy(&d->dc[o * 3], &data.dc[i * 3], 12);
        if ( !d->shRest.empty() ) std::memcpy(&d->shRest[o * 3 * K], &data.shRest[i * 3 * K], 3 * K * sizeof(float));
        ++o;
    }
    d->count = m;
    d->shDegree = data.shDegree;
    d->fileShDegree = data.fileShDegree;
    d->hasGaussians = data.hasGaussians;
    d->yUp = data.yUp;
    d->medianSize = data.medianSize;     // sizes still mean "times the scene's typical splat"
    finishScene(*d);
    return d;
}

void
robustSplatBounds(const SplatData& data, bool flipYZ, float center[3], float size[3])
{
    const std::size_t n = data.count;
    const std::size_t step = n / 500000 + 1;
    std::vector<float> v;
    v.reserve(n / step + 1);
    for (int k = 0; k < 3; ++k) {
        v.clear();
        const float s = (flipYZ && k > 0) ? -1.f : 1.f;
        for (std::size_t i = 0; i < n; i += step) v.push_back(s * data.pos[i * 3 + k]);
        if ( v.empty() ) { center[k] = 0.f; size[k] = 1.f; continue; }
        const std::size_t lo = v.size() / 50, hi = v.size() - 1 - v.size() / 50;
        std::nth_element( v.begin(), v.begin() + lo, v.end() );
        const float a = v[lo];
        std::nth_element( v.begin(), v.begin() + hi, v.end() );
        const float b = v[hi];
        center[k] = 0.5f * (a + b);
        size[k] = std::max(1e-6f, b - a);
    }
}

NATRON_NAMESPACE_EXIT
