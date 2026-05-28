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

#include "KnobGradient.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

NATRON_NAMESPACE_ENTER

KnobGradient::KnobGradient(KnobHolder* holder,
                           const std::string& description,
                           int dimension,
                           bool declaredByPlugin)
    : QObject()
    , AnimatingKnobStringHelper(holder, description, dimension, declaredByPlugin)
{
}

KnobGradient::~KnobGradient()
{
}

const std::string KnobGradient::_typeNameStr("DevGradient");

const std::string&
KnobGradient::typeNameStatic()
{
    return _typeNameStr;
}

// ----------------------------------------------------------------------------
// Stops API
// ----------------------------------------------------------------------------

std::vector<KnobGradient::Stop>
KnobGradient::defaultStops()
{
    // Warm fire-style ramp by default — matches the prototype's "Color over
    // Life" first preset. Keeps the very first creation visually meaningful.
    std::vector<Stop> s(3);
    s[0] = { 0.00, 1.000f, 0.820f, 0.290f, 1.0f }; // ffd14a
    s[1] = { 0.45, 1.000f, 0.353f, 0.000f, 1.0f }; // ff5a00
    s[2] = { 1.00, 0.102f, 0.000f, 0.063f, 1.0f }; // 1a0010
    return s;
}

namespace {
// Parse 6 or 8 hex chars to 0-1 floats. Returns false on malformed.
bool
parseHexRGBA(const std::string& hex, float& r, float& g, float& b, float& a)
{
    if (hex.size() != 6 && hex.size() != 8) return false;
    unsigned int rr, gg, bb, aa = 255;
    int n = (hex.size() == 8)
            ? std::sscanf(hex.c_str(), "%2x%2x%2x%2x", &rr, &gg, &bb, &aa)
            : std::sscanf(hex.c_str(), "%2x%2x%2x",     &rr, &gg, &bb);
    if (n < 3) return false;
    r = rr / 255.0f; g = gg / 255.0f; b = bb / 255.0f; a = aa / 255.0f;
    return true;
}

// Format 0-1 float channels to "rrggbbaa".
std::string
toHexRGBA(float r, float g, float b, float a)
{
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    int ri = (int)std::lround(clamp01(r) * 255.0f);
    int gi = (int)std::lround(clamp01(g) * 255.0f);
    int bi = (int)std::lround(clamp01(b) * 255.0f);
    int ai = (int)std::lround(clamp01(a) * 255.0f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", ri, gi, bi, ai);
    return buf;
}
} // anonymous namespace

// Serialization format: "pos,hex,pos,hex,..." — comma-only, paired tokens.
// Matches the KnobShuffle precedent ("0,1,2,3") which is known to survive
// Natron's YAML project serialization unchanged. Earlier formats using
// `:` or `|` separators both broke project loads.
std::vector<KnobGradient::Stop>
KnobGradient::parse(const std::string& serialized)
{
    if (serialized.empty()) return defaultStops();

    // Split into tokens by comma, then walk in pairs (pos, hex).
    std::vector<std::string> tokens;
    {
        std::stringstream ss(serialized);
        std::string tok;
        while (std::getline(ss, tok, ',')) tokens.push_back(tok);
    }

    std::vector<Stop> out;
    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        const std::string& posTok = tokens[i];
        const std::string& hexTok = tokens[i + 1];
        if (posTok.empty() || hexTok.empty()) continue;

        double pos = 0.0;
        try { pos = std::stod(posTok); } catch (...) { continue; }
        if (pos < 0.0) pos = 0.0;
        if (pos > 1.0) pos = 1.0;

        Stop s;
        s.position = pos;
        if (!parseHexRGBA(hexTok, s.r, s.g, s.b, s.a)) continue;
        out.push_back(s);
    }
    if (out.empty()) return defaultStops();

    std::sort(out.begin(), out.end(),
              [](const Stop& a, const Stop& b) { return a.position < b.position; });
    return out;
}

std::string
KnobGradient::serialize(const std::vector<Stop>& stops)
{
    std::ostringstream os;
    for (size_t i = 0; i < stops.size(); ++i) {
        if (i) os << ',';
        os << stops[i].position << ',' << toHexRGBA(stops[i].r, stops[i].g, stops[i].b, stops[i].a);
    }
    return os.str();
}

void
KnobGradient::sample(const std::vector<Stop>& stops, float t,
                     float* r, float* g, float* b, float* a)
{
    if (stops.empty()) {
        *r = *g = *b = *a = 1.0f;
        return;
    }
    // Clamp t to [0,1] — caller is expected to have normalized but be safe.
    if (t <= (float)stops.front().position) {
        *r = stops.front().r; *g = stops.front().g;
        *b = stops.front().b; *a = stops.front().a;
        return;
    }
    if (t >= (float)stops.back().position) {
        *r = stops.back().r; *g = stops.back().g;
        *b = stops.back().b; *a = stops.back().a;
        return;
    }
    // Find the bracketing stop pair. Linear scan — stop count is small
    // (typically 2-6), so binary search isn't worth the complexity.
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const Stop& s0 = stops[i];
        const Stop& s1 = stops[i + 1];
        if (t >= (float)s0.position && t <= (float)s1.position) {
            const float span = (float)(s1.position - s0.position);
            const float lt = (span > 1e-6f) ? ((t - (float)s0.position) / span) : 0.0f;
            *r = s0.r + (s1.r - s0.r) * lt;
            *g = s0.g + (s1.g - s0.g) * lt;
            *b = s0.b + (s1.b - s0.b) * lt;
            *a = s0.a + (s1.a - s0.a) * lt;
            return;
        }
    }
    // Shouldn't reach here given the clamps above, but keep it safe.
    *r = stops.back().r; *g = stops.back().g;
    *b = stops.back().b; *a = stops.back().a;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobGradient.cpp"
