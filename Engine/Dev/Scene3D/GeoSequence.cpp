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

// GeoSequence implementation. See GeoSequence.h for the design.
//
// The stored filename value decides: the file dialog's Sequence: mode returns
// a '#' hash pattern (".../liquid_#####.abc") exactly like the image Reader's
// File value, File: mode returns a literal single path. Detection expands the
// pattern with the same library the image Reader uses (libs/SequenceParsing):
// filesListFromPattern_slow scans the pattern's directory and maps each
// present frame number to its file. A value without a hash run (or one that
// matches no file) is used as-is — single static file.

#include "GeoSequence.h"

#include <SequenceParsing.h>

#include <cctype>
#include <cstdlib>

NATRON_NAMESPACE_ENTER

int
GeoSequenceInfo::clampFrame(int frame) const
{
    if (files.empty()) {
        return frame;
    }
    if (frame < firstFrame) {
        return firstFrame;
    }
    if (frame > lastFrame) {
        return lastFrame;
    }
    // Walk down to the nearest present frame (larger gaps hold at the
    // sequence head — intentional: no look-ahead would break the "start
    // rendering at frame 1 even if the sim only starts at 50" convention).
    std::map<int, std::string>::const_iterator it = files.find(frame);
    while (frame > firstFrame && it == files.end()) {
        --frame;
        it = files.find(frame);
    }
    return it != files.end() ? frame : firstFrame;
}

GeoSequenceInfo
detectGeoSequence(const std::string& path)
{
    GeoSequenceInfo out;

    if (path.empty()) {
        return out;
    }

    // lowercase-with-dot extension ("liquid_00002.ABC" -> ".abc").
    {
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) {
            out.extension = path.substr(dot);
            for (size_t i = 0; i < out.extension.size(); ++i) {
                out.extension[i] = (char)std::tolower((unsigned char)out.extension[i]);
            }
        }
    }
    if (out.extension.empty()) {
        return out;
    }

    // Gathering is an explicit choice made in the file dialog (the Read-node
    // practice): the returned value carries a '#' run only when the Sequence:
    // mode grouped files. A literal path (File: mode) is a single static file
    // — no silent sibling scan around it.
    if (path.find('#') == std::string::npos) {
        return out;
    }

    SequenceParsing::SequenceFromPattern matches;
    if (!SequenceParsing::filesListFromPattern_slow(path, &matches) || matches.empty()) {
        return out;
    }

    // Single view only (geo files have no '%v' concept).
    std::map<int, std::string> files;
    for (SequenceParsing::SequenceFromPattern::const_iterator it = matches.begin();
         it != matches.end(); ++it) {
        if (it->second.empty()) {
            continue;
        }
        files[it->first] = it->second.begin()->second;
    }
    if (files.empty()) {
        return out;
    }

    out.pattern = path;
    out.files.swap(files);
    out.firstFrame = out.files.begin()->first;
    out.lastFrame = out.files.rbegin()->first;
    out.isSequence = true;
    return out;
}

NATRON_NAMESPACE_EXIT
