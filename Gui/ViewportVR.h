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

#ifndef NATRON_GUI_VIEWPORTVR_H
#define NATRON_GUI_VIEWPORTVR_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include "Global/GLIncludes.h" //!< must be included before QGlWidget because of gl.h and glew.h
#include "Global/GlobalDefines.h"

#include <vector>

#include "Global/QtCompat.h"
#include <QOpenGLWidget>
#include <QString>

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The dedicated VR / 360 viewport widget.
 *
 * Renders the frame of an attached node as an equirectangular (latitude/longitude,
 * 2:1) panorama the user can look around in: mouse-drag turns the virtual camera,
 * the wheel changes the field of view. The image comes from the attached node's
 * own render pipeline at the current frame, through Node::makePreviewImage (the
 * same blocking GUI-thread render the node thumbnail uses, Node.cpp), converted
 * to a GL texture and cached — orbit-only repaints never re-render the node.
 *
 * Stereo packing is handled at sample time: an Over/Under (left eye = top half,
 * the Google "top-bottom" convention) or Side-by-Side (left eye = left half)
 * source is cropped to the selected eye's sub-rectangle; the engine fetch is
 * always the full frame (a true per-view stereoscopic fetch is a later step).
 */

class ViewportVR
    : public QOpenGLWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    /** How the 2:1 panorama is packed with the eyes. */
    enum Layout
    {
        eLayoutMono = 0,   //!< full frame, both eyes see the whole pano
        eLayoutOverUnder,  //!< left eye = top half, right eye = bottom half
        eLayoutSideBySide  //!< left eye = left half, right eye = right half
    };

    explicit ViewportVR(Gui* gui, QWidget* parent = NULL);
    virtual ~ViewportVR();

    // ---- attached node ------------------------------------------------------

    /** Attach the node with this script name (empty = none attached). A fresh
     *  fetch is scheduled for the next paint. */
    void setNodeScriptName(const QString& scriptName);

    /** The script name of the attached node (empty = none). */
    QString getAttachedNodeName() const { return _attachedScriptName; }

    // ---- view state ---------------------------------------------------------

    void setLayoutMode(Layout l);
    Layout getLayoutMode() const { return _layout; }

    /** 0 = left eye, 1 = right eye (no effect for eLayoutMono). */
    void setEye(int eye);
    int getEye() const { return _eye; }

    void setLook(double yawDeg, double pitchDeg, double fovDeg);
    void getLook(double* yawDeg, double* pitchDeg, double* fovDeg) const;
    void resetLook();

    // ---- refresh ------------------------------------------------------------

    /** Re-render the attached node and re-upload at the next paint, even if the
     *  frame and node did not change and the throttle would normally skip. The
     *  fetch itself runs at paint time so we are always on the GUI thread. */
    void forceRefresh();

    QSize sizeHint() const OVERRIDE;

protected:

    virtual void initializeGL() OVERRIDE;
    virtual void paintGL() OVERRIDE;
    virtual void resizeGL(int width, int height) OVERRIDE;

    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE;

private:

    /** Render the attached node into _pixels and upload the GL texture.
     *  Returns true if the node produced an image. Call only on the GUI thread
     *  with the GL context current. */
    bool fetchAndUploadImage(double time);

    Gui* _gui;

    // Attached node: script name is the persistent identity (it survives save/
    // load and node renames are re-resolved lazily); the weak pointer is the
    // fast-hold so a dead node cannot be kept alive by the viewport.
    QString _attachedScriptName;
    NodeWPtr _attachedNode;

    // view state
    double _yawDeg;   //!< 0 = looking at the center of the pano; + = turn east (right)
    double _pitchDeg; //!< 0 = level; + = look up (north pole), clamped to +-87.5
    double _fovDeg;   //!< vertical field of view in degrees
    Layout _layout;
    int _eye;

    // drag state
    int _lastMouseX, _lastMouseY;
    bool _dragging;

    // image cache / GL texture
    std::vector<unsigned int> _pixels; //!< BGRA ints from makePreviewImage (line0 = image top)
    int _texWidth, _texHeight;
    GLuint _tex;
    SequenceTime _fetchedTime;
    bool _fetchedOnce;
    bool _forceNextFetch; //!< set by forceRefresh(), consumed by the next paint

    // shader program (per context — rebuilt whenever the GL context is recreated)
    GLuint _program;
    bool _programReady;
    GLint _uTex, _uSize, _uTanHalfFov, _uAspect, _uYaw, _uPitch, _uUv0, _uUv1;

    // paint-time fetch throttle (one node render per kBildFetchIntervalMs max)
    qint64 _lastFetchMs;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORTVR_H
