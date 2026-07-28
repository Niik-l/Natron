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

#ifndef NATRON_ENGINE_PARTICLEPARALLEL_H
#define NATRON_ENGINE_PARTICLEPARALLEL_H

#include <cstddef>
#include <utility>
#include <vector>

#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5+
#include <QThread>

#include "ParticleData.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Run `fn(Particle&)` over every particle, fanned out across cores.
 *
 * For use ONLY with per-particle-independent bodies: fn must read/write
 * fields of its own particle exclusively (no accumulators, no shared mutable
 * state, no order dependence). All the stateless force nodes qualify.
 * Small arrays run serially — the QtConcurrent dispatch overhead would
 * otherwise dominate.
 */
template <typename Fn>
inline void
forEachParticleParallel(std::vector<Particle>& particles, Fn&& fn)
{
    const std::size_t n = particles.size();
    if (n == 0) return;

    // Below this, serial is faster than the thread-pool round trip.
    static const std::size_t kParallelThreshold = 16384;
    if (n < kParallelThreshold) {
        for (std::size_t i = 0; i < n; ++i) fn(particles[i]);
        return;
    }

    // Chunked ranges: ~4 chunks per core balances load without per-particle
    // dispatch overhead.
    int threads = QThread::idealThreadCount();
    if (threads < 1) threads = 4;
    const std::size_t nChunks = (std::size_t)threads * 4;
    std::size_t chunkSize = (n + nChunks - 1) / nChunks;
    if (chunkSize < 1024) chunkSize = 1024;

    std::vector<std::pair<std::size_t, std::size_t> > ranges;
    ranges.reserve(nChunks);
    for (std::size_t begin = 0; begin < n; begin += chunkSize) {
        ranges.push_back(std::make_pair(begin, std::min(begin + chunkSize, n)));
    }

    Particle* base = particles.data();
    QtConcurrent::blockingMap(ranges,
        [base, &fn](const std::pair<std::size_t, std::size_t>& r) {
            for (std::size_t i = r.first; i < r.second; ++i) {
                fn(base[i]);
            }
        });
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PARTICLEPARALLEL_H
