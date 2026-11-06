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

#ifndef NATRON_GUI_VIEWPORTVRTAB_H
#define NATRON_GUI_VIEWPORTVRTAB_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Global/QtCompat.h"
#include "Global/GlobalDefines.h"

#include <QWidget>

#include "Gui/PanelWidget.h"
#include "Gui/GuiFwd.h"

class QLabel;
class QSpinBox;
class QToolButton;

NATRON_NAMESPACE_ENTER

class TimeLineGui;
class ViewportVR;

/**
 * @brief Wrapper panel for the dedicated VR / 360 viewport, matching the 3D
 * viewport tab layout.
 *
 * Layout:
 *   Top toolbar — attached-node chooser, stereo layout + eye, reset view,
 *                 force refresh
 *   ViewportVR  — the equirectangular look-around widget
 *   Bottom bar  — timeline strip
 */
class ViewportVRTab
    : public QWidget
    , public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    explicit ViewportVRTab(Gui* gui, QWidget* parent = NULL);
    virtual ~ViewportVRTab();

    ViewportVR* getViewport() const { return _viewport; }

public Q_SLOTS:

    /** Build the attached-node dropdown by enumerating every node in the project
     *  right now (on aboutToShow, so the list is always fresh — same pattern as
     *  the 3D viewport's camera menu). */
    void onAttachMenuAboutToShow();
    void onAttachSelected();

    /** Stereo packing dropdown: Mono / Over-Under / Side-by-Side. */
    void onLayoutSelected();

    /** Eye dropdown: Left / Right (checked items enable only for O/U + SbS). */
    void onEyeSelected();

    void onResetView();
    void onForceRefresh();
    void onFrameChanged(SequenceTime frame, int reason);

private:

    ViewportVR* _viewport;
    TimeLineGui* _timelineGui;
    QToolButton* _attachDropdown;
    QToolButton* _layoutDropdown;
    QToolButton* _eyeDropdown;
    QToolButton* _refreshBtn;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORTVRTAB_H
