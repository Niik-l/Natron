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

#ifndef NATRON_ENGINE_OCIOCOLORSPACEUTILS_H
#define NATRON_ENGINE_OCIOCOLORSPACEUTILS_H

// ***** BEGIN PYTHON BLOCK *****
// Strict aliasing rules and such are turned off by including this header
#include "Global/Macros.h"
// ***** END PYTHON BLOCK *****

#include <string>
#include <vector>
#include <cctype>

#ifdef NATRON_HAVE_OPENIMAGEIO
#include <OpenImageIO/color.h>   // OIIO::ColorConfig (wraps the active OCIO config)
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief Returns the names of every colorspace in the active OCIO config.
 *
 * The config is the one the OCIO env var points to (Natron's Settings export it),
 * which is the same config the Read/Write/OCIO* plugins, Cycles and the viewer use,
 * so a name returned here is guaranteed to resolve everywhere downstream.
 *
 * Header-only so multiple nodes (Material3D, etc.) can share it without a new
 * translation unit / CMake change. Returns an empty vector if Natron was built
 * without OpenImageIO or OCIO support is unavailable in the running OIIO.
 */
inline std::vector<std::string>
getOcioColorSpaceNames()
{
    std::vector<std::string> out;
#ifdef NATRON_HAVE_OPENIMAGEIO
    static OIIO::ColorConfig config;   // reads $OCIO once (config change needs restart)
    if ( OIIO::ColorConfig::supportsOpenColorIO() ) {
        const int n = config.getNumColorSpaces();
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            const char* name = config.getColorSpaceNameByIndex(i);
            if (name) {
                out.push_back( std::string(name) );
            }
        }
    }
#endif
    return out;
}

/**
 * @brief Name of the config's scene_linear colorspace (the rendering / working
 * space — "ACEScg" under an ACES config, "Linear Rec.709" under others). Empty
 * if unavailable. This is what Cycles renders in and what disk-writers treat as
 * the source space for any output conversion.
 */
inline std::string
getOcioSceneLinearName()
{
#ifdef NATRON_HAVE_OPENIMAGEIO
    static OIIO::ColorConfig config;
    if ( OIIO::ColorConfig::supportsOpenColorIO() ) {
        const char* n = config.getColorSpaceNameByRole("scene_linear");
        if (n && n[0]) {
            return std::string(n);
        }
    }
#endif
    return std::string();
}

/**
 * @brief A sensible display-referred colorspace for review LDR (PNG/JPG) output:
 * prefers an ACES SDR Rec.709 output, then a plain sRGB display, then any sRGB
 * name. Falls back to scene_linear if nothing display-like is found. Used so an
 * 8-bit review file is never written as raw linear (which looks dark/desaturated).
 */
inline std::string
getOcioDefaultDisplayColorSpace()
{
#ifdef NATRON_HAVE_OPENIMAGEIO
    const std::vector<std::string> names = getOcioColorSpaceNames();
    auto containsCI = [](const std::string& hay, const char* needle) -> bool {
        std::string h = hay;
        for (std::size_t i = 0; i < h.size(); ++i) {
            h[i] = (char)std::tolower( (unsigned char)h[i] );
        }
        return h.find(needle) != std::string::npos;
    };
    // 1) ACES SDR Rec.709 output (matches the viewer's ACES look).
    for (std::size_t i = 0; i < names.size(); ++i) {
        if ( containsCI(names[i], "aces") && containsCI(names[i], "sdr") && containsCI(names[i], "709") ) {
            return names[i];
        }
    }
    // 2) Plain sRGB display.
    for (std::size_t i = 0; i < names.size(); ++i) {
        if ( containsCI(names[i], "srgb") && containsCI(names[i], "display") ) {
            return names[i];
        }
    }
    // 3) Any sRGB-encoded space.
    for (std::size_t i = 0; i < names.size(); ++i) {
        if ( containsCI(names[i], "srgb") ) {
            return names[i];
        }
    }
#endif
    return getOcioSceneLinearName();
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_OCIOCOLORSPACEUTILS_H
