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
#include <QThread>
#include <QWaitCondition>
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

static const double kVRPitchClampDeg = 87.5;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// Section 2: the async fetch worker
//
// Node::makePreviewImage is engine-sanctioned off the GUI thread: the engine's
// own PreviewThread (Gui/PreviewThread.cpp) runs exactly this call on a worker
// and it cleans up TLS for the calling thread itself. Running it here keeps
// scrubbing responsive — the GUI paints the LAST completed texture while the
// worker refreshes, and pending requests are REPLACED (newest wins), so a fast
// scrub never queues up a backlog of stale renders the way the 2D viewer does
// not either.
// ============================================================================

class VRFetchThread
    : public QThread
{
public:

    explicit VRFetchThread(QObject* notifier)
        : QThread()
        , _notifier(notifier)
        , _quitting(false)
        , _pendingHas(false)
        , _pendingNode()
        , _pendingTime(0)
        , _pendingGen(1)
        , _renderedGen(0)
        , _scratch(kVRPreviewMaxW * kVRPreviewMaxH)
        , _out(kVRPreviewMaxW * kVRPreviewMaxH)
        , _outGen(0)
        , _outW(0)
        , _outH(0)
    {
        setObjectName(QString::fromUtf8("ViewportVRFetch"));
        start(QThread::LowPriority);
    }

    virtual ~VRFetchThread()
    {
        stopAndJoin();
    }

    /** The node and frame the worker should be rendering. Replacing a pending
     *  request is free (it has not started); a request identical to one already
     *  queued is not re-queued. The widget re-queues explicitly on scrub so a
     *  frame revisited gets its (engine-cache-cheap) re-render. */
    void schedule(const NodePtr& node, double time)
    {
        QMutexLocker k(&_mutex);
        if (_quitting) {
            return;
        }
        if (_pendingHas && (_pendingNode == node) && (_pendingTime == time)) {
            return; // already queued
        }
        _pendingNode = node;
        _pendingTime = time;
        ++_pendingGen;
        _pendingHas = true;
        _cond.wakeOne();
    }

    /** Stop the loop and drop everything further. */
    void stopAndJoin()
    {
        {
            QMutexLocker k(&_mutex);
            _quitting = true;
            _cond.wakeOne();
        }
        if ( QThread::isRunning() ) {
            wait();
        }
    }

    /** Read the newest finished image into dst. Returns the generation or the
     *  last known one when nothing new. dst must be at least maxW*maxH ints. */
    int acquireResult(int lastUploadedGen, std::vector<unsigned int>& dst, int* w, int* h)
    {
        QMutexLocker k(&_mutex);
        if (_outGen == lastUploadedGen) {
            return lastUploadedGen;
        }
        std::memcpy( &dst.front(), &_out.front(), sizeof(unsigned int) * _out.size() );
        *w = _outW;
        *h = _outH;
        return _outGen;
    }

protected:

    virtual void run() OVERRIDE
    {
        for (;;) {
            NodePtr node;
            double time = 0;
            int gen;
            {
                QMutexLocker k(&_mutex);
                while (!_quitting && !_pendingHas) {
                    _cond.wait(&_mutex);
                }
                if (_quitting) {
                    return;
                }
                node = _pendingNode;
                time = _pendingTime;
                gen = _pendingGen;
                _pendingHas = false;
            }

            bool ok = false;
            int w = 0, h = 0;
            if (node) {
                w = kVRPreviewMaxW;
                h = kVRPreviewMaxH;
                SequenceTime t = (SequenceTime)time;
                ok = node->makePreviewImage(t, &w, &h, &_scratch.front());
                if ( !ok || (w <= 0) || (h <= 0) ) {
                    ok = false;
                    w = 0;
                    h = 0;
                }
            }
            {
                QMutexLocker k(&_mutex);
                if (ok) {
                    std::memcpy( &_out.front(), &_scratch.front(), sizeof(unsigned int) * _out.size() );
                    _outW = w;
                    _outH = h;
                    // Retire older published generations: only the newest
                    // result matters (same ask the 2D viewer makes).
                    _outGen = gen;
                }
                _pendingNode.reset();
                // Rendered even on failure so the schedule state advances.
                _renderedGen = gen;
            }
            // Nudge the GUI thread to repaint/upload (queued — we are NOT the
            // GUI thread; the queued call is dropped safely if the widget dies).
            if (_notifier) {
                QMetaObject::invokeMethod(_notifier, "update", Qt::QueuedConnection);
            }
        }
    }

private:

    QObject* _notifier; //!< the ViewportVR widget (queued update only)
    QMutex _mutex;
    QWaitCondition _cond;

    bool _quitting;
    bool _pendingHas;
    NodePtr _pendingNode;
    double _pendingTime;
    int _pendingGen;
    int _renderedGen;

    std::vector<unsigned int> _scratch;
    std::vector<unsigned int> _out;
    int _outGen, _outW, _outH;
};

// ============================================================================
// Section 3: constructor / view state
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
    , _gamma(1.0)
    , _channels(eChannelsRGB)
    , _fetchThread(NULL)
    , _scheduledTime(-1)
    , _scheduledOnce(false)
    , _forceNextFetch(false)
    , _texWidth(0)
    , _texHeight(0)
    , _tex(0)
    , _lastUploadedGen(0)
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
    , _uChan(-1)
    , _uGamma(-1)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);

    _fetchThread = new VRFetchThread(this);
}

ViewportVR::~ViewportVR()
{
    // Stop the worker first: it holds the node and calls engine APIs; the Qt
    // object tree may be torn down before this destructor finishes.
    if (_fetchThread) {
        _fetchThread->stopAndJoin();
        delete _fetchThread;
        _fetchThread = NULL;
    }
    // GL resources are owned by the QOpenGLWidget context; if it still exists,
    // release them against it. initializeGL re-runs whenever the context is
    // recreated, so the stale handles must not leak there either.
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
    _scheduledOnce = false;
    _forceNextFetch = true;
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
ViewportVR::setGammaDebug(double gamma)
{
    if (gamma <= 0) {
        gamma = 1;
    }
    if (_gamma != gamma) {
        _gamma = gamma;
        update();
    }
}

void
ViewportVR::setChannelsMode(ChannelsMode m)
{
    if (_channels != m) {
        _channels = m;
        update();
    }
}

void
ViewportVR::forceRefresh()
{
    _forceNextFetch = true;
    update();
}

// ============================================================================
// Section 4: OpenGL
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
    _lastUploadedGen = 0;
    _scheduledOnce = false; // context restored fresh: re-schedule the frame

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
        "uniform int   u_chan;\n"        // channel mode 0..4
        "uniform float u_gamma;\n"       // display gamma (color ^ (1/gamma))
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
        "    vec4 tex = texture2D(u_tex, uvCrop);\n"
        "    vec3 col = tex.rgb;\n"
        "    if (u_chan == 1) {\n"
        "        col = vec3(tex.r);\n"
        "    } else if (u_chan == 2) {\n"
        "        col = vec3(tex.g);\n"
        "    } else if (u_chan == 3) {\n"
        "        col = vec3(tex.b);\n"
        "    } else if (u_chan == 4) {\n"
        "        float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));\n"
        "        col = vec3(lum);\n"
        "    }\n"
        "    if (u_gamma != 1.0) {\n"
        "        col = pow(max(col, 0.0), vec3(1.0 / u_gamma));\n"
        "    }\n"
        "    gl_FragColor = vec4(col, 1.0);\n"
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
        _uChan         = glGetUniformLocation(_program, "u_chan");
        _uGamma        = glGetUniformLocation(_program, "u_gamma");
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

    // Queue a fetch when needed: first paint, changed frame, changed new node,
    // or the panel/central refresh funnel marked us stale. The worker thread
    // replaces pending requests (newest wins) so a fast scrub coalesces.
    bool needsSchedule = !_scheduledOnce || (_scheduledTime != time) || _forceNextFetch;
    if (needsSchedule) {
        scheduleFetch(app);
    }

    // Upload whenever the worker published something we have not shown yet.
    if (_fetchThread) {
        std::vector<unsigned int>& buf = _uploadBuffer;
        if (buf.size() < (std::size_t)kVRPreviewMaxW * (std::size_t)kVRPreviewMaxH) {
            buf.resize( (std::size_t)kVRPreviewMaxW * (std::size_t)kVRPreviewMaxH );
        }
        int gen = _fetchThread->acquireResult(_lastUploadedGen, buf, &_texWidth, &_texHeight);
        if ( (gen != _lastUploadedGen) && (_texWidth > 0) && (_texHeight > 0) ) {
            if (!_tex) {
                glGenTextures(1, &_tex);
            }
            glBindTexture(GL_TEXTURE_2D, _tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            // The engine buffer is BGRA uint32 little-endian, line 0 = image top,
            // alpha always 255 (Node.cpp toBGRA). GL_BGRA with
            // GL_UNSIGNED_INT_8_8_8_8_REV maps those bytes exactly (the same
            // packing the preview consumers use to paint these buffers).
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _texWidth, _texHeight, 0, GL_BGRA,
                         GL_UNSIGNED_INT_8_8_8_8_REV, &buf.front());
            _lastUploadedGen = gen;
        }
    }

    if (!_tex || _texWidth <= 0 || _texHeight <= 0) {
        return;
    }

    applyDisplayUniforms();
}

void
ViewportVR::applyDisplayUniforms()
{
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
    glUniform1i(_uChan, (GLint)_channels);
    glUniform1f(_uGamma, (GLfloat)_gamma);

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
// Section 5: fetch scheduling
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

void
ViewportVR::scheduleFetch(GuiAppInstancePtr app)
{
    if (!_fetchThread) {
        return;
    }
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project || _attachedScriptName.isEmpty()) {
        return;
    }
    NodePtr node = _attachedNode.lock();
    if (!node || node->getScriptName() != _attachedScriptName.toStdString()) {
        node = findNodeByScriptName(project, _attachedScriptName);
        _attachedNode = node;
    }
    if (!node || !node->isActivated()) {
        return;
    }

    double time = app->getTimeLine()->currentFrame();
    _fetchThread->schedule(node, time);
    _scheduledTime = time;
    _scheduledOnce = true;
    _forceNextFetch = false;
}

// ============================================================================
// Section 6: mouse interaction
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
