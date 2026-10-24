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

#ifndef NATRON_ENGINE_GEOSEQUENCE_H
#define NATRON_ENGINE_GEOSEQUENCE_H

#include "../../../Global/Macros.h"

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief One-file-per-frame geometry sequence support (ReadGeo/ReadGeoArchive).
 *
 * A sequence comes from the file dialog's Sequence: mode (the Read-node
 * practice): the filename value is a '#' hash pattern over a directory of
 * mesh files whose names differ only by a padded frame number — e.g.
 * liquid_#####.abc standing for liquid_00002.abc, liquid_00003.abc, ...
 * Each file holds the complete geometry of one timeline frame (topology may
 * change per frame, unlike a single-file animated format such as Alembic, so
 * every frame is parsed independently and the built mesh is cached).
 *
 * Detection (detectGeoSequence) takes the stored value: a value with a '#'
 * run is expanded through the same library the image Reader uses
 * (libs/SequenceParsing), scanning the pattern's directory for matching
 * files; a literal path (the dialog's File: mode) is not a sequence and is
 * used as a single static file.
 *
 * Frame mapping: the FILE's frame numbers are kept on the Natron timeline —
 * timeline frame N evaluates the file numbered N, held (clamped) at the
 * sequence ends and at gaps (hold the nearest earlier present frame).
 */

struct GeoSequenceInfo
{
    bool isSequence = false;
    std::string pattern;   // absolute '#' pattern (".../liquid_#####.abc")
    std::string extension; // lowercase, with dot (".abc")
    int firstFrame = 0;
    int lastFrame = 0;
    std::map<int, std::string> files; // file frame -> absolute file path

    bool empty() const { return !isSequence; }

    /**
     * @brief Resolve a timeline frame to a present file frame: clamped to
     * [firstFrame, lastFrame]; a gap holds the nearest earlier frame in range.
     */
    int clampFrame(int frame) const;
};

/**
 * @brief Detect the sequence from the stored filename value. A value with a
 *   '#' run (the file dialog's Sequence: mode) is expanded and returns the
 *   per-frame file map; a literal path (File: mode) or a pattern matching no
 *   file returns a non-sequence info (with the extension filled) so callers
 *   fall back to single-file loading.
 */
GeoSequenceInfo detectGeoSequence(const std::string& path);

/**
 * @brief Bounded LRU cache keyed by sequence frame. NOT thread-safe — the
 * nodes use it under their own data mutex (the same discipline as
 * GeoArchiveRuntime, which is guarded by the owning node's mutex).
 *
 * `T` is the cached payload type; values are shared_ptr-held so a consumer
 * that fetched one before an eviction keeps it alive as long as needed
 * (eviction only drops the cache's own reference).
 */
template <typename T>
class GeoSequenceLruCache
{
public:

    explicit GeoSequenceLruCache(int capacity = 72)
        : capacity(capacity > 0 ? capacity : 1) {}

    void setCapacity(int c)
    {
        capacity = c > 0 ? c : 1;
        evictOverflow();
    }

    void clear()
    {
        recency.clear();
        entries.clear();
    }

    /** Cache hit returns the payload and moves it to the most-recent slot. */
    std::shared_ptr<T> get(int frame)
    {
        typename std::map<int, std::shared_ptr<T> >::iterator it = entries.find(frame);
        if (it == entries.end()) {
            return std::shared_ptr<T>();
        }
        touch(frame);
        return it->second;
    }

    void put(int frame, std::shared_ptr<T> value)
    {
        if (!value) {
            return;
        }
        entries[frame] = value;
        touch(frame);
        evictOverflow();
    }

private:

    void touch(int frame)
    {
        recency.remove(frame);
        recency.push_back(frame);
    }

    void evictOverflow()
    {
        while ((int)recency.size() > capacity) {
            const int lru = recency.front();
            recency.pop_front();
            entries.erase(lru);
        }
    }

    int capacity;
    std::map<int, std::shared_ptr<T> > entries;
    std::list<int> recency; // front = oldest (least recently used)
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GEOSEQUENCE_H
