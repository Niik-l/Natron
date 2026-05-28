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

#ifndef NATRON_GUI_GRADIENTWIDGET_H
#define NATRON_GUI_GRADIENTWIDGET_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <vector>

#include <QWidget>

#include "Global/Macros.h"
#include "Engine/Dev/Particles/KnobGradient.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Interactive color-gradient editor widget.
 *
 * Renders a horizontal gradient bar with draggable color stops above it.
 *
 *   - **Left-click** on a stop handle: select it (becomes highlighted).
 *   - **Left-click** in the bar (not on a handle): add a new stop at
 *     that position, color sampled from the existing gradient.
 *   - **Drag** a selected handle: move position, clamped to [0, 1].
 *   - **Double-click** a stop: open QColorDialog to edit color.
 *   - **Right-click** a stop: delete it (keeps at least 2 stops).
 *
 * Emits stopsChanged() on any mutation; KnobGuiGradient relays this to
 * the engine-side KnobGradient string storage.
 */
class GradientWidget
    : public QWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    explicit GradientWidget(QWidget* parent = nullptr);
    virtual ~GradientWidget();

    void setStops(const std::vector<KnobGradient::Stop>& stops);
    const std::vector<KnobGradient::Stop>& stops() const { return _stops; }

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual QSize minimumSizeHint() const OVERRIDE FINAL;

Q_SIGNALS:

    /// Fires after any user-initiated change to the stops vector.
    void stopsChanged();

protected:

    virtual void paintEvent(QPaintEvent* event) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE FINAL;

private:

    // Hit-test: returns the stop index whose triangle handle contains the
    // given x position, or -1 if no handle is within tolerance.
    int hitTestHandle(int x) const;

    // Geometry: pixel positions of the gradient bar.
    int barLeft()  const { return 4; }
    int barRight() const { return width() - 4; }
    int barWidth() const { return barRight() - barLeft(); }
    int handleRowH() const { return 16; }
    int barTop()    const { return handleRowH(); }
    int barBottom() const { return height() - 2; }

    // Sample current gradient at a parametric position (helper used when
    // adding a new stop — pick the existing color at that x).
    void sampleAt(double t, float& r, float& g, float& b, float& a) const;

    std::vector<KnobGradient::Stop> _stops;
    int  _selectedStop = -1;
    bool _dragging     = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_GRADIENTWIDGET_H
