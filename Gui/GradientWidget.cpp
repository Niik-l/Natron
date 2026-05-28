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

#include "GradientWidget.h"

#include <algorithm>
#include <cmath>

#include <QColorDialog>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>

NATRON_NAMESPACE_ENTER

GradientWidget::GradientWidget(QWidget* parent)
    : QWidget(parent)
    , _stops(KnobGradient::defaultStops())
{
    setMouseTracking(false);
    setFocusPolicy(Qt::ClickFocus);
}

GradientWidget::~GradientWidget()
{
}

void
GradientWidget::setStops(const std::vector<KnobGradient::Stop>& stops)
{
    if (stops.empty()) {
        _stops = KnobGradient::defaultStops();
    } else {
        _stops = stops;
    }
    if (_selectedStop >= (int)_stops.size()) _selectedStop = -1;
    update();
}

QSize
GradientWidget::sizeHint() const
{
    return QSize(260, 44);
}

QSize
GradientWidget::minimumSizeHint() const
{
    return QSize(180, 44);
}

int
GradientWidget::hitTestHandle(int x) const
{
    const int tol = 6; // pixels
    int bestIdx = -1;
    int bestDist = tol + 1;
    for (size_t i = 0; i < _stops.size(); ++i) {
        const int hx = barLeft() + (int)std::lround(_stops[i].position * barWidth());
        const int d = std::abs(x - hx);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = (int)i;
        }
    }
    return bestIdx;
}

void
GradientWidget::sampleAt(double t, float& r, float& g, float& b, float& a) const
{
    KnobGradient::sample(_stops, (float)t, &r, &g, &b, &a);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void
GradientWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect barRect(barLeft(), barTop(), barWidth(), barBottom() - barTop());

    // ---- Gradient bar ----
    if (_stops.size() == 1) {
        const KnobGradient::Stop& s = _stops.front();
        p.fillRect(barRect, QColor::fromRgbF(s.r, s.g, s.b, s.a));
    } else {
        QLinearGradient g(barRect.left(), 0, barRect.right(), 0);
        for (size_t i = 0; i < _stops.size(); ++i) {
            const KnobGradient::Stop& s = _stops[i];
            g.setColorAt(s.position, QColor::fromRgbF(s.r, s.g, s.b, s.a));
        }
        p.fillRect(barRect, QBrush(g));
    }

    p.setPen(QColor(0, 0, 0, 200));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barRect);

    // ---- Stop handles ----
    for (size_t i = 0; i < _stops.size(); ++i) {
        const KnobGradient::Stop& s = _stops[i];
        const int x = barRect.left() + (int)std::lround(s.position * barRect.width());
        QPolygon tri;
        tri << QPoint(x - 4, handleRowH() - 8)
            << QPoint(x + 4, handleRowH() - 8)
            << QPoint(x,     handleRowH() - 1);
        p.setBrush(QColor::fromRgbF(s.r, s.g, s.b, 1.0f));
        // Highlight the selected stop with a brighter pen.
        if ((int)i == _selectedStop) {
            p.setPen(QPen(QColor(255, 255, 255), 2));
        } else {
            p.setPen(QColor(0, 0, 0));
        }
        p.drawPolygon(tri);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void
GradientWidget::mousePressEvent(QMouseEvent* e)
{
    const int x = e->pos().x();
    const int y = e->pos().y();

    if (e->button() == Qt::RightButton) {
        const int hit = hitTestHandle(x);
        // Right-click on a stop: delete (keep at least 2 stops).
        if (hit >= 0 && _stops.size() > 2) {
            _stops.erase(_stops.begin() + hit);
            if (_selectedStop == hit) _selectedStop = -1;
            else if (_selectedStop > hit) --_selectedStop;
            update();
            Q_EMIT stopsChanged();
        }
        return;
    }

    if (e->button() != Qt::LeftButton) return;

    const int hit = hitTestHandle(x);
    if (hit >= 0) {
        // Click on existing handle: select + begin drag.
        _selectedStop = hit;
        _dragging = true;
        update();
        return;
    }

    // Click in the bar area (or empty handle row): add a new stop here.
    // Only add if click is within the widget's interactive vertical range.
    if (y < 0 || y > height()) return;

    const double t = std::max(0.0, std::min(1.0,
                              (double)(x - barLeft()) / std::max(1, barWidth())));
    KnobGradient::Stop ns;
    ns.position = t;
    sampleAt(t, ns.r, ns.g, ns.b, ns.a);
    _stops.push_back(ns);
    std::sort(_stops.begin(), _stops.end(),
              [](const KnobGradient::Stop& a, const KnobGradient::Stop& b) {
                  return a.position < b.position;
              });
    // Select the newly inserted stop.
    for (size_t i = 0; i < _stops.size(); ++i) {
        if (std::abs(_stops[i].position - t) < 1e-9) {
            _selectedStop = (int)i;
            break;
        }
    }
    _dragging = true;
    update();
    Q_EMIT stopsChanged();
}

void
GradientWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!_dragging || _selectedStop < 0 || _selectedStop >= (int)_stops.size()) return;

    const double t = std::max(0.0, std::min(1.0,
                              (double)(e->pos().x() - barLeft()) / std::max(1, barWidth())));
    _stops[_selectedStop].position = t;

    // Re-sort if the drag crossed a neighbour. Re-find the index for our stop.
    const KnobGradient::Stop moved = _stops[_selectedStop];
    std::sort(_stops.begin(), _stops.end(),
              [](const KnobGradient::Stop& a, const KnobGradient::Stop& b) {
                  return a.position < b.position;
              });
    for (size_t i = 0; i < _stops.size(); ++i) {
        // Identify by color since position may have duplicates after clamp;
        // moved stop is the one whose RGBA exactly matches `moved`.
        const KnobGradient::Stop& s = _stops[i];
        if (s.r == moved.r && s.g == moved.g && s.b == moved.b
            && s.a == moved.a && std::abs(s.position - moved.position) < 1e-9) {
            _selectedStop = (int)i;
            break;
        }
    }
    update();
    Q_EMIT stopsChanged();
}

void
GradientWidget::mouseReleaseEvent(QMouseEvent* /*e*/)
{
    _dragging = false;
}

void
GradientWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const int hit = hitTestHandle(e->pos().x());
    if (hit < 0) return;

    _selectedStop = hit;
    const KnobGradient::Stop& s = _stops[hit];
    QColor initial = QColor::fromRgbF(s.r, s.g, s.b, s.a);

    QColor picked = QColorDialog::getColor(initial, this, QStringLiteral("Edit stop color"),
                                           QColorDialog::ShowAlphaChannel);
    if (!picked.isValid()) return;

    _stops[hit].r = (float)picked.redF();
    _stops[hit].g = (float)picked.greenF();
    _stops[hit].b = (float)picked.blueF();
    _stops[hit].a = (float)picked.alphaF();
    update();
    Q_EMIT stopsChanged();
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_GradientWidget.cpp"
