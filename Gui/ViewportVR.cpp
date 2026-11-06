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

#include "ViewportVR.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QDateTime>
#include <QMouseEvent>
#include <QWheelEvent>

#include "Engine/AppManager.h"
#include "Engine/TimeLine.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"

NATRON_NAMESPACE_ENTER

// ============================================================================
// Section 1: constants
// ============================================================================

/// The makePreviewImage buffer cap. The caller picks the maximum texture size;
/// the engine letterboxes (never exceeds) it. 2048x1024 is a 2:1 pano, the
/// common case; bigger sources get downscaled into it by the engine itself.
static const int kVRPreviewMaxW = 2048;
static const int kVRPreviewMaxH = 1024;

/// Paint-time fetch throttle: at most one node render per this many
/// milliseconds per widget (paintGL runs on every orbit frame; the node only
/// re-renders when the throttled fetch picks it up).
static const qint64 kVRFetchIntervalMs = 80;

static const double kVRPitchClampDeg = 87.5;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// Section 2: constructor / view state
// ============================================================================

ViewportVR::ViewportVR(Gui* gui, QWidget* parent)
    : QOpenGLWidget(parent)
    , _gui(gui)
    , _yawDeg(0)
    , _pitchDeg(0)
    , _fovDeg(70)
    , _layout(eLayoutMono)
    , _eye(0)
    , _lastMouseX(-1)
    , _lastMouseY(-1)
    , _dragging(false)
    , _texWidth(0)
    , _texHeight(0)
    , _tex(0)
    , _fetchedTime(-1)
    , _fetchedOnce(false)
    , _forceNextFetch(false)
    , _program(0)
    , _programReady(false)
    , _uTex(-1)
    , _uSize(-1)
    , _uTanHalfFov(-1)
    , _uAspect(-1)
    , _uYaw(-1)
    , _uPitch(-1)
    , _uUv0(-1)
    , _uUv1(-1)
    , _lastFetchMs(0)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    _pixels.resize( (std::size_t)kVRPreviewMaxW * (std::size_t)kVRPreviewMaxH );
}

ViewportVR::~ViewportVR()
{
    // GL resources are owned by the QOpenGLWidget context; if it still exists,
    // release them against it. QOpenGLWidget::initializeGL re-runs whenever the
    // context is recreated (e.g. reparenting during a layout restore), and the
    // stale handles must not leak there either.
    if ( isValid() ) {
        makeCurrent();
        if (_tex) {
            glDeleteTextures(1, &_tex);
            _tex = 0;
        }
        if (_program) {
            glDeleteProgram(_program);
            _program = 0;
        }
        _programReady = false;
        doneCurrent();
    }
}

QSize
ViewportVR::sizeHint() const
{
    return QSize(640, 480);
}

void
ViewportVR::setNodeScriptName(const QString& scriptName)
{
    if ( _attachedScriptName == scriptName ) {
        return;
    }
    _attachedScriptName = scriptName;
    _attachedNode.reset();
    _fetchedOnce = false;
    update();
}

void
ViewportVR::setLayoutMode(Layout l)
{
    if (_layout != l) {
        _layout = l;
        update();
    }
}

void
ViewportVR::setEye(int eye)
{
    if ( (eye != 0) && (eye != 1) ) {
        eye = 0;
    }
    if (_eye != eye) {
        _eye = eye;
        update();
    }
}

void
ViewportVR::setLook(double yawDeg, double pitchDeg, double fovDeg)
{
    _yawDeg = yawDeg;
    if (pitchDeg > kVRPitchClampDeg) {
        pitchDeg = kVRPitchClampDeg;
    } else if (pitchDeg < -kVRPitchClampDeg) {
        pitchDeg = -kVRPitchClampDeg;
    }
    _pitchDeg = pitchDeg;
    if (fovDeg < 10) {
        fovDeg = 10;
    } else if (fovDeg > 130) {
        fovDeg = 130;
    }
    _fovDeg = fovDeg;
    update();
}

void
ViewportVR::getLook(double* yawDeg, double* pitchDeg, double* fovDeg) const
{
    if (yawDeg) {
        *yawDeg = _yawDeg;
    }
    if (pitchDeg) {
        *pitchDeg = _pitchDeg;
    }
    if (fovDeg) {
        *fovDeg = _fovDeg;
    }
}

void
ViewportVR::resetLook()
{
    setLook(0, 0, 70);
}

void
ViewportVR::forceRefresh()
{
    _forceNextFetch = true;
    update();
}

// ============================================================================
// Section 3: OpenGL
// ============================================================================

void
ViewportVR::initializeGL()
{
    // Same GL flavor as the 3D viewport ("#version 430 compatibility"): legacy
    // varyings + texture2D, fragment output into gl_FragColor, fed by an
    // immediate-mode fullscreen quad in paintGL. _program is a per-context GL
    // name, so drop any stale program whenever the context is (re)created.
    if (_tex) {
        glDeleteTextures(1, &_tex);
        _tex = 0;
    }
    if (_program) {
        glDeleteProgram(_program);
        _program = 0;
    }
    _programReady = false;
    _texWidth = _texHeight = 0;
    _fetchedOnce = false;

    glClearColor(0.08f, 0.08f, 0.09f, 1.f);
    glDisable(GL_DEPTH_TEST);

    const char* vtx =
        "#version 430 compatibility\n"
        "void main() {\n"
        "    gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);\n"
        "}\n";
    const char* frag =
        "#version 430 compatibility\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec2  u_size;\n"        // widget size in pixels
        "uniform float u_tanHalfFov;\n"  // tan(vfov / 2)
        "uniform float u_aspect;\n"      // width / height
        "uniform float u_yaw;\n"         // radians, + = turn east (right)
        "uniform float u_pitch;\n"       // radians, + = look up (north)
        "uniform vec2  u_uv0;\n"         // sub-rect low corner (stereo crop)
        "uniform vec2  u_uv1;\n"         // sub-rect high corner
        "void main() {\n"
        "    vec2 c = (gl_FragCoord.xy / u_size) * 2.0 - 1.0;\n"
        "    vec3 d = normalize(vec3(c.x * u_tanHalfFov * u_aspect, c.y * u_tanHalfFov, -1.0));\n"
        "    float cy = cos(u_yaw), sy = sin(u_yaw);\n"
        "    d = vec3(cy * d.x + sy * d.z, d.y, -sy * d.x + cy * d.z);\n"
        "    float cp = cos(u_pitch), sp = sin(u_pitch);\n"
        "    d = vec3(d.x, cp * d.y + sp * d.z, -sp * d.y + cp * d.z);\n"
        "    float lon = atan(d.x, -d.z);\n"
        "    float lat = asin(clamp(d.y, -1.0, 1.0));\n"
        "    const float k2Pi = 6.28318530717958647;\n"
        "    const float kPi = 3.1415926535897932;\n"
        "    vec2 uv = vec2(0.5 + lon / k2Pi, 0.5 - lat / kPi);\n"
        "    vec2 uvCrop = u_uv0 + uv * (u_uv1 - u_uv0);\n"
        "    gl_FragColor = texture2D(u_tex, uvCrop);\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vtx, NULL);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    GLuint fs = 0;
    if (ok) {
        fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &frag, NULL);
        glCompileShader(fs);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    }
    if (ok) {
        _program = glCreateProgram();
        glAttachShader(_program, vs);
        glAttachShader(_program, fs);
        glLinkProgram(_program);
        glGetProgramiv(_program, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(_program);
            _program = 0;
        }
    }
    if (ok) {
        _uTex          = glGetUniformLocation(_program, "u_tex");
        _uSize         = glGetUniformLocation(_program, "u_size");
        _uTanHalfFov   = glGetUniformLocation(_program, "u_tanHalfFov");
        _uAspect       = glGetUniformLocation(_program, "u_aspect");
        _uYaw          = glGetUniformLocation(_program, "u_yaw");
        _uPitch        = glGetUniformLocation(_program, "u_pitch");
        _uUv0          = glGetUniformLocation(_program, "u_uv0");
        _uUv1          = glGetUniformLocation(_program, "u_uv1");
        _programReady = true;
    } else {
        char log[1024] = {0};
        if (fs) {
            glGetShaderInfoLog(fs, sizeof(log) - 1, NULL, log);
        } else {
            glGetShaderInfoLog(vs, sizeof(log) - 1, NULL, log);
        }
        std::fprintf(stderr, "[ViewportVR] shader failed, view stays dark: %s\n", log);
    }
    if (vs) {
        glDeleteShader(vs);
    }
    if (fs) {
        glDeleteShader(fs);
    }
}

void
ViewportVR::resizeGL(int /*width*/, int /*height*/)
{
    // Nothing to do: the shader takes the widget size as a uniform each paint.
}

void
ViewportVR::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (!_programReady) {
        return;
    }

    GuiAppInstancePtr app = _gui ? _gui->getApp() : GuiAppInstancePtr();
    double time = app ? app->getTimeLine()->currentFrame() : 0;

    // Fetch when needed: first paint, changed frame, changed node, or the
    // panel/central refresh funnel marked us stale. Throttled to one render per
    // kVRFetchIntervalMs, matching the 2D viewer's per-frame render cadence but
    // skipping bursts within a single drag.
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool needsFetch = !_fetchedOnce || (_fetchedTime != time) || _forceNextFetch;
    if (needsFetch && (nowMs - _lastFetchMs >= kVRFetchIntervalMs)) {
        _forceNextFetch = false;
        fetchAndUploadImage(time);
        _lastFetchMs = nowMs;
    } else if (needsFetch && _forceNextFetch) {
        // Throttle held: keep the force pending rather than losing it.
        _forceNextFetch = true;
    }

    if (!_tex || _texWidth <= 0 || _texHeight <= 0) {
        return;
    }

    // ---- stereo crop -----------------------------------------------------
    // s = 0 at the pano's left edge, t = 0 at the pano's TOP row (the makePreview
    // buffer is top-down, line 0 = image top = north pole).
    float uv0[2] = { 0.f, 0.f };
    float uv1[2] = { 1.f, 1.f };
    switch (_layout) {
    case eLayoutOverUnder:
        // Left eye = top half (Google "top-bottom": left over right)
        if (_eye == 0) {
            uv0[1] = 0.f; uv1[1] = 0.5f;
        } else {
            uv0[1] = 0.5f; uv1[1] = 1.f;
        }
        break;
    case eLayoutSideBySide:
        uv0[0] = (_eye == 0) ? 0.f : 0.5f;
        uv1[0] = (_eye == 0) ? 0.5f : 1.f;
        break;
    case eLayoutMono:
    default:
        break;
    }

    glUseProgram(_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _tex);
    glUniform1i(_uTex, 0);
    glUniform2f(_uSize, (GLfloat)width(), (GLfloat)height());
    glUniform1f(_uTanHalfFov, (GLfloat)std::tan(_fovDeg * 0.5 * M_PI / 180.));
    glUniform1f(_uAspect, (GLfloat)width() / (GLfloat)height());
    glUniform1f(_uYaw, (GLfloat)(_yawDeg * M_PI / 180.));
    glUniform1f(_uPitch, (GLfloat)(_pitchDeg * M_PI / 180.));
    glUniform2f(_uUv0, uv0[0], uv0[1]);
    glUniform2f(_uUv1, uv1[0], uv1[1]);

    // Fullscreen quad; the vertex shader merely passes gl_Vertex through.
    glBegin(GL_QUADS);
    glVertex2d(-1.0, -1.0);
    glVertex2d( 1.0, -1.0);
    glVertex2d( 1.0,  1.0);
    glVertex2d(-1.0,  1.0);
    glEnd();
    glUseProgram(0);
}

// ============================================================================
// Section 4: image acquisition
// ============================================================================

static NodePtr
findNodeByScriptName(const ProjectPtr& project, const QString& scriptName)
{
    if (!project || scriptName.isEmpty()) {
        return NodePtr();
    }
    const std::string name = scriptName.toStdString();
    NodesList allNodes;
    project->getNodes_recursive(allNodes, true);
    for (NodesList::const_iterator it = allNodes.begin(); it != allNodes.end(); ++it) {
        if ( (*it)->isActivated() && ((*it)->getScriptName() == name) ) {
            return *it;
        }
    }
    return NodePtr();
}

bool
ViewportVR::fetchAndUploadImage(double time)
{
    GuiAppInstancePtr app = _gui ? _gui->getApp() : GuiAppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project || _attachedScriptName.isEmpty()) {
        return false;
    }

    NodePtr node = _attachedNode.lock();
    if (!node || node->getScriptName() != _attachedScriptName.toStdString()) {
        node = findNodeByScriptName(project, _attachedScriptName);
        _attachedNode = node;
    }
    if (!node || !node->isActivated()) {
        return false;
    }

    int w = kVRPreviewMaxW;
    int h = kVRPreviewMaxH;
    const bool ok = node->makePreviewImage( (SequenceTime)time, &w, &h, &_pixels.front() );
    if (!ok || (w <= 0) || (h <= 0) || ( (std::size_t)w * (std::size_t)h > _pixels.size() )) {
        // Back off — a failed render (plugin error, group empty, etc.) must not
        // be retried at the full paint cadence.
        _lastFetchMs += 4 * kVRFetchIntervalMs;
        return false;
    }
    _fetchedTime = (SequenceTime)time;
    _fetchedOnce = true;

    // The engine buffer is BGRA uint32 little-endian, line 0 = image top,
    // alpha always 255 (Node.cpp toBGRA). GL_BGRA with
    // GL_UNSIGNED_INT_8_8_8_8_REV maps those bytes exactly (the same packing
    // the preview consumers use to paint these buffers).
    if (!_tex) {
        glGenTextures(1, &_tex);
    }
    glBindTexture(GL_TEXTURE_2D, _tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &_pixels.front());
    _texWidth = w;
    _texHeight = h;

    return true;
}

// ============================================================================
// Section 5: mouse interaction
// ============================================================================

void
ViewportVR::mousePressEvent(QMouseEvent* e)
{
    if ( (e->button() == Qt::LeftButton) ) {
        _lastMouseX = e->x();
        _lastMouseY = e->y();
        _dragging = true;
    } else {
        QOpenGLWidget::mousePressEvent(e);
    }
}

void
ViewportVR::mouseMoveEvent(QMouseEvent* e)
{
    if (!_dragging || ( (_lastMouseX < 0) || (_lastMouseY < 0) )) {
        QOpenGLWidget::mouseMoveEvent(e);
        return;
    }
    int dx = e->x() - _lastMouseX;
    int dy = e->y() - _lastMouseY;
    _lastMouseX = e->x();
    _lastMouseY = e->y();

    // Turn speed: dragging the widget's full height = one vertical field of
    // view. Same pitch sign as YouTube's player: drag down = look down.
    int base = height() > 0 ? height() : 480;
    double k = _fovDeg / (double)std::max(240, base);
    double newYaw = _yawDeg + dx * k;
    double newPitch = _pitchDeg - dy * k;
    setLook(newYaw, newPitch, _fovDeg);
    // setLook already called update().
}

void
ViewportVR::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        _dragging = false;
        _lastMouseX = _lastMouseY = -1;
    } else {
        QOpenGLWidget::mouseReleaseEvent(e);
    }
}

void
ViewportVR::wheelEvent(QWheelEvent* e)
{
    float delta = e->angleDelta().y() / 120.0f;
    if (std::fabs(delta) < 0.01f) {
        QOpenGLWidget::wheelEvent(e);
        return;
    }
    // Wheel up = zoom in (narrow FOV), same direction as the 2D / 3D viewers.
    double factor = std::pow(0.92, (double)delta);
    setLook(_yawDeg, _pitchDeg, _fovDeg * factor); // clamps inside setLook
}

NATRON_NAMESPACE_EXIT
