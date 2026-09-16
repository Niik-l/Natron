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
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Path3D.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <sstream>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct Path3DPrivate
{
    KnobStringWPtr pointsData;      // hidden, persistent: "x y z" per line
    KnobIntWPtr    selectedPoint;   // non-persistent editor state
    KnobDoubleWPtr translateX, translateY, translateZ;   // selected point, mirrored (gizmo reads/writes these)
    KnobButtonWPtr addPoint, deletePoint;
    KnobBoolWPtr   closed;
    KnobStringWPtr info;
    bool syncing;

    // Sampled curve + cumulative arc length, rebuilt when the points change.
    mutable std::mutex cacheMutex;
    mutable std::string cacheKey;
    mutable std::vector<double> samples;   // xyz per sample
    mutable std::vector<double> arc;       // cumulative length per sample

    Path3DPrivate() : syncing(false) {}
};

Path3D::Path3D(NodePtr node)
    : EffectInstance(node)
    , _imp(new Path3DPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Path3D::~Path3D()
{
}

std::string
Path3D::getPluginDescription() const
{
    return "A camera rail: a smooth curve through control points you place and drag in the 3D "
           "viewport. Select the node, click a point in the viewport (or pick it with Selected "
           "Point) and move it with the gizmo; Add Point continues the curve past the last point. "
           "Connect to a Camera3D's 'path' input and key the camera's Position Along Path from 0 "
           "to 1 — the parameter is arc-length based, so equal steps are equal distances.";
}

void
Path3D::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
Path3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Path3D::isHostChannelSelectorSupported(bool* defaultR, bool* defaultG, bool* defaultB, bool* defaultA) const
{
    *defaultR = *defaultG = *defaultB = *defaultA = false;
    return false;
}

// ==================== knobs ====================

void
Path3D::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Path"));

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info"); k->setAsLabel(); k->setIsPersistent(false); k->setEvaluateOnChange(false);
        page->addKnob(k); _imp->info = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Add Point"));
        k->setName("addPoint");
        k->setHintToolTip(tr("Append a control point past the last one, continuing the curve's direction, and select it."));
        page->addKnob(k); _imp->addPoint = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Delete Point"));
        k->setName("deletePoint");
        k->setHintToolTip(tr("Remove the selected control point."));
        page->addKnob(k); _imp->deletePoint = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Selected Point"));
        k->setName("selectedPoint"); k->setDefaultValue(0); k->setMinimum(0);
        k->setDisplayMinimum(0); k->setDisplayMaximum(20);
        k->setIsPersistent(false); k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("The control point the X / Y / Z knobs and the viewport gizmo act on. "
                             "Clicking a point in the 3D viewport sets this too."));
        page->addKnob(k); _imp->selectedPoint = k;
    }
    // The selected point's position. Named translate* so the viewport gizmo, which
    // reads and writes those knob names, drags the point with no special casing.
    const char* names[3] = { "translateX", "translateY", "translateZ" };
    const char* labels[3] = { "Point X", "Point Y", "Point Z" };
    KnobDoubleWPtr* slots[3] = { &_imp->translateX, &_imp->translateY, &_imp->translateZ };
    for (int i = 0; i < 3; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr(labels[i]));
        k->setName(names[i]); k->setDefaultValue(0.0);
        k->setDisplayMinimum(-100.0); k->setDisplayMaximum(100.0);
        k->setIsPersistent(false); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("World position of the selected control point."));
        page->addKnob(k); *slots[i] = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Closed"));
        k->setName("closed"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Join the last point back to the first (a loop)."));
        page->addKnob(k); _imp->closed = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Points Data"));
        k->setName("pointsData");
        k->setAsMultiLine();
        k->setSecret(true);
        k->setDefaultValue(std::string("-3 0 3\n0 0 5\n3 0 3\n"));
        page->addKnob(k); _imp->pointsData = k;
    }
    syncEditor();
}

std::vector<double>
Path3D::loadPoints() const
{
    std::vector<double> pts;
    KnobStringPtr k = _imp->pointsData.lock();
    if (!k) return pts;
    std::istringstream in(k->getValue());
    std::string line;
    while (std::getline(in, line)) {
        double x, y, z;
        if (std::sscanf(line.c_str(), "%lf %lf %lf", &x, &y, &z) == 3) {
            pts.push_back(x); pts.push_back(y); pts.push_back(z);
        }
    }
    return pts;
}

void
Path3D::savePoints(const std::vector<double>& pts)
{
    std::ostringstream out;
    out.precision(9);
    for (std::size_t i = 0; i + 2 < pts.size(); i += 3) {
        out << pts[i] << ' ' << pts[i + 1] << ' ' << pts[i + 2] << '\n';
    }
    if (KnobStringPtr k = _imp->pointsData.lock()) k->setValue(out.str());
    if (KnobStringPtr inf = _imp->info.lock()) {
        std::ostringstream s;
        s << pts.size() / 3 << " points";
        if (KnobBoolPtr c = _imp->closed.lock()) if (c->getValue()) s << ", closed";
        inf->setValue(s.str());
    }
}

void
Path3D::syncEditor()
{
    const std::vector<double> pts = loadPoints();
    const int n = (int)pts.size() / 3;
    KnobIntPtr sel = _imp->selectedPoint.lock();
    int i = sel ? sel->getValue() : 0;
    if (i >= n) i = n - 1;
    if (i < 0) i = 0;
    _imp->syncing = true;
    if (sel) { sel->setMaximum(std::max(0, n - 1)); if (sel->getValue() != i) sel->setValue(i); }
    KnobDoublePtr t[3] = { _imp->translateX.lock(), _imp->translateY.lock(), _imp->translateZ.lock() };
    for (int a = 0; a < 3; ++a) {
        if (!t[a]) continue;
        t[a]->setEnabled(0, n > 0);
        t[a]->setValue(n > 0 ? pts[(std::size_t)i * 3 + a] : 0.0);
    }
    if (KnobStringPtr inf = _imp->info.lock()) {
        std::ostringstream s;
        s << n << " points";
        if (KnobBoolPtr c = _imp->closed.lock()) if (c->getValue()) s << ", closed";
        inf->setValue(s.str());
    }
    _imp->syncing = false;
}

void
Path3D::onKnobsLoaded()
{
    syncEditor();
}

int
Path3D::selectedPointIndex() const
{
    KnobIntPtr sel = _imp->selectedPoint.lock();
    const int n = pathPointCount();
    if (!sel || n == 0) return -1;
    return std::max(0, std::min(n - 1, sel->getValue()));
}

void
Path3D::setSelectedPointIndex(int index)
{
    if (KnobIntPtr sel = _imp->selectedPoint.lock()) sel->setValue(index);
}

bool
Path3D::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                    double /*time*/, bool /*originatedFromMainThread*/)
{
    if (!k) return false;
    if (_imp->syncing) return false;

    if (k == _imp->addPoint.lock().get()) {
        std::vector<double> pts = loadPoints();
        const int n = (int)pts.size() / 3;
        double p[3] = { 0.0, 0.0, 0.0 };
        if (n >= 2) {          // continue the last direction
            for (int a = 0; a < 3; ++a) p[a] = 2.0 * pts[(std::size_t)(n - 1) * 3 + a] - pts[(std::size_t)(n - 2) * 3 + a];
        } else if (n == 1) {
            p[0] = pts[0] + 2.0; p[1] = pts[1]; p[2] = pts[2];
        }
        pts.insert(pts.end(), p, p + 3);
        savePoints(pts);
        if (KnobIntPtr sel = _imp->selectedPoint.lock()) { sel->setMaximum(n); _imp->syncing = true; sel->setValue(n); _imp->syncing = false; }
        syncEditor();
        return true;
    }
    if (k == _imp->deletePoint.lock().get()) {
        std::vector<double> pts = loadPoints();
        const int i = selectedPointIndex();
        if (i >= 0) {
            pts.erase(pts.begin() + (std::ptrdiff_t)i * 3, pts.begin() + (std::ptrdiff_t)i * 3 + 3);
            savePoints(pts);
            syncEditor();
        }
        return true;
    }
    if (k == _imp->selectedPoint.lock().get()) {
        syncEditor();
        return true;
    }
    if (k == _imp->translateX.lock().get() || k == _imp->translateY.lock().get() || k == _imp->translateZ.lock().get()) {
        std::vector<double> pts = loadPoints();
        const int i = selectedPointIndex();
        if (i >= 0) {
            pts[(std::size_t)i * 3 + 0] = _imp->translateX.lock()->getValue();
            pts[(std::size_t)i * 3 + 1] = _imp->translateY.lock()->getValue();
            pts[(std::size_t)i * 3 + 2] = _imp->translateZ.lock()->getValue();
            savePoints(pts);
        }
        return true;
    }
    if (k == _imp->closed.lock().get()) {
        syncEditor();
        return true;
    }
    if (k == _imp->pointsData.lock().get()) {   // undo / paste / project load
        syncEditor();
        return true;
    }
    return false;
}

// ==================== PathProvider ====================

int
Path3D::pathPointCount() const
{
    return (int)loadPoints().size() / 3;
}

bool
Path3D::pathPoint(int index, double out[3]) const
{
    const std::vector<double> pts = loadPoints();
    if (index < 0 || (std::size_t)index * 3 + 2 >= pts.size()) return false;
    out[0] = pts[(std::size_t)index * 3];
    out[1] = pts[(std::size_t)index * 3 + 1];
    out[2] = pts[(std::size_t)index * 3 + 2];
    return true;
}

bool
Path3D::pathClosed() const
{
    KnobBoolPtr c = _imp->closed.lock();
    return c && c->getValue();
}

// Catmull-Rom through the points (phantom end points for an open curve),
// 32 samples per segment, with a cumulative arc-length table so evalPath can
// map u in [0,1] to a constant-speed position.
void
Path3D::ensureCache() const
{
    KnobStringPtr k = _imp->pointsData.lock();
    const std::string key = (k ? k->getValue() : std::string()) + (pathClosed() ? "|c" : "|o");
    std::lock_guard<std::mutex> lk(_imp->cacheMutex);
    if (key == _imp->cacheKey) return;
    _imp->cacheKey = key;
    _imp->samples.clear();
    _imp->arc.clear();

    const std::vector<double> pts = loadPoints();
    const int n = (int)pts.size() / 3;
    if (n < 2) return;
    const bool closed = pathClosed();
    const int nSeg = closed ? n : n - 1;
    auto P = [&](int i, int a) -> double {
        if (closed) { i = ((i % n) + n) % n; return pts[(std::size_t)i * 3 + a]; }
        if (i < 0)  return 2.0 * pts[a] - pts[3 + a];                                    // phantom before first
        if (i >= n) return 2.0 * pts[(std::size_t)(n - 1) * 3 + a] - pts[(std::size_t)(n - 2) * 3 + a];   // phantom after last
        return pts[(std::size_t)i * 3 + a];
    };
    const int kPer = 32;
    double prev[3] = { 0, 0, 0 };
    double len = 0.0;
    for (int s = 0; s < nSeg; ++s) {
        for (int j = (s == 0 ? 0 : 1); j <= kPer; ++j) {
            const double t = (double)j / kPer, t2 = t * t, t3 = t2 * t;
            double q[3];
            for (int a = 0; a < 3; ++a) {
                const double p0 = P(s - 1, a), p1 = P(s, a), p2 = P(s + 1, a), p3 = P(s + 2, a);
                q[a] = 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
            }
            if (!_imp->samples.empty()) {
                const double dx = q[0] - prev[0], dy = q[1] - prev[1], dz = q[2] - prev[2];
                len += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            _imp->samples.insert(_imp->samples.end(), q, q + 3);
            _imp->arc.push_back(len);
            prev[0] = q[0]; prev[1] = q[1]; prev[2] = q[2];
        }
    }
}

bool
Path3D::evalPath(double u, double pos[3], double tangent[3]) const
{
    ensureCache();
    std::lock_guard<std::mutex> lk(_imp->cacheMutex);
    const std::size_t ns = _imp->arc.size();
    if (ns < 2) return false;
    const double total = _imp->arc.back();
    if (pathClosed()) { u -= std::floor(u); } else { u = std::max(0.0, std::min(1.0, u)); }
    const double target = u * total;
    // binary search the sample interval
    std::size_t lo = 0, hi = ns - 1;
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (_imp->arc[mid] <= target) lo = mid; else hi = mid;
    }
    const double a0 = _imp->arc[lo], a1 = _imp->arc[hi];
    const double f = (a1 > a0) ? (target - a0) / (a1 - a0) : 0.0;
    const double* s0 = &_imp->samples[lo * 3];
    const double* s1 = &_imp->samples[hi * 3];
    double tl = 0.0;
    for (int a = 0; a < 3; ++a) {
        pos[a] = s0[a] + (s1[a] - s0[a]) * f;
        tangent[a] = s1[a] - s0[a];
        tl += tangent[a] * tangent[a];
    }
    tl = std::sqrt(tl);
    if (tl > 1e-12) { for (int a = 0; a < 3; ++a) tangent[a] /= tl; }
    else { tangent[0] = 0.0; tangent[1] = 0.0; tangent[2] = -1.0; }
    return true;
}

// ==================== dummy image output ====================

StatusEnum
Path3D::getRegionOfDefinition(U64 /*hash*/, double /*time*/, const RenderScale& /*scale*/,
                              ViewIdx /*view*/, RectD* rod)
{
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
Path3D::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusOK;
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusOK;
    RectI bounds = outImg->getBounds();
    Image::WriteAccess wa(outImg.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* pix = (float*)wa.pixelAt(x, y);
            if (pix) { pix[0] = pix[1] = pix[2] = pix[3] = 0.0f; }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_Path3D.cpp"
