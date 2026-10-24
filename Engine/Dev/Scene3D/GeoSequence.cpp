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
// Detection delegates the directory scan to the same library the image
// Reader uses (libs/SequenceParsing): FileNameContent identifies the digit
// runs of the file name, generatePatternWithFrameNumberAtIndex turns the
// rightmost run into a '#' hash pattern (padding = the run's digit count),
// and filesListFromPattern_slow collects the matching files of its directory.
// One subtlety: files whose number is the run adjacent to the extension are
// the canonical per-frame export convention (liquid_00002.abc), and the
// rightmost run is the safest frame-number slot for multi-number names
// (Circle01_00002.bin) — FileNameContent's own matchesPattern doc agrees.

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

    // SequenceParsing needs the absolute file name with path separators it
    // understands; paths already arrive forward-slashed from the knobs, but
    // be tolerant of Windows separators (FileNameContent/removePath accept
    // both, tinydir on Windows accepts '/').
    std::string abs = path;
    SequenceParsing::FileNameContent fc(abs);

    const int numRuns = fc.getPotentialFrameNumbersCount();
    if (numRuns <= 0) {
        return out; // no digit run — single static file
    }

    // The rightmost run is the frame-number candidate.
    const int runIdx = numRuns - 1;
    std::string numberStr;
    if (!fc.getNumberByIndex(runIdx, &numberStr) || numberStr.empty()) {
        return out;
    }
    // Padding: '#' matches exactly numHashes digits with leading zero
    // padding, so the hash count is the digit count of the run as written.
    const int numHashes = (int)numberStr.size();

    std::string pattern;
    fc.generatePatternWithFrameNumberAtIndex(runIdx, numHashes, &pattern);
    if (pattern.empty()) {
        return out;
    }

    SequenceParsing::SequenceFromPattern matches;
    if (!SequenceParsing::filesListFromPattern_slow(pattern, &matches) || matches.empty()) {
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

    // A sequence needs at least 2 files; one match is just the file itself.
    if (files.size() < 2) {
        return out;
    }

    out.pattern = pattern;
    out.files.swap(files);
    out.firstFrame = out.files.begin()->first;
    out.lastFrame = out.files.rbegin()->first;
    out.isSequence = true;
    return out;
}

NATRON_NAMESPACE_EXIT
