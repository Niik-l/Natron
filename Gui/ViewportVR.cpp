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
#include <cstring>

#include <QCursor>
#include <QDateTime>
#include <QMouseEvent>
#include <QWheelEvent>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/Utils.h"
#include <QOpenGLContext>
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"
#include "Global/GLIncludes.h" // GLProtectAttrib / GLProtectMatrix
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/InfoViewerWidget.h"

NATRON_NAMESPACE_ENTER

// ============================================================================
// Section 1: constants
// ============================================================================

static const double kVRPitchClampDeg = 87.5;

// Wipe-control overlay geometry (ViewerGLPrivate's values): handle lengths
// are fractions of the widget size, not zoom-space pixels.
static const double kVRWipeMixHandleLength = 50.;
static const double kVRWipeRotateHandleLength = 100.;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// Section 2: constructor / view state
// ============================================================================

ViewportVR::ViewportVR(Gui* gui, QWidget* parent)
    : QOpenGLWidget(parent)
    , OpenGLViewerI()
    , _gui(gui)
    , _yawDeg(0)
    , _pitchDeg(0)
    , _fovDeg(70)
    , _layout(eLayoutMono)
    , _eye(0)
    , _lastMouseX(-1)
    , _lastMouseY(-1)
    , _dragging(false)
    , _lastEmittedYaw(0)
    , _lastEmittedPitch(0)
    , _lookEmittedOnce(false)
    , _lastRodH(2048)
    , _roisEnabled(true)
    , _compositingOp(eViewerCompositingOperatorNone)
    , _wipeMixX(-1.)
    , _wipeMixAmount(1.)
    , _fullFrameProcessing(true)
    , _lut(eViewerColorSpaceLinear)
    , _rangeFirst(1)
    , _rangeLast(250)
    , _infoViewer(NULL)
    , _infoViewerB(NULL)
    , _requestedOp(eViewerCompositingOperatorNone)
    , _program(0)
    , _programReady(false)
    , _uTexA(-1)
    , _uTexB(-1)
    , _uSize(-1)
    , _uTanHalfFov(-1)
    , _uAspect(-1)
    , _uYaw(-1)
    , _uPitch(-1)
    , _uUvA0(-1)
    , _uUvA1(-1)
    , _uUvB0(-1)
    , _uUvB1(-1)
    , _uOp(-1)
    , _uHaveB(-1)
    , _uSubWindow(-1)
    , _uWipeX(-1)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    std::memset(_slots, 0, sizeof(_slots));
    for (int i = 0; i < 2; ++i) {
        _slots[i].isVisible = false;
        _slots[i].time = -1;
        _slots[i].pickerBytes = 0;
        _slots[i].mipmapLevel = 0;
    }
    _pboIndex = 0;
    // Automated QA hook (MCP sessions): start pitched up so the zenith area is
    // visible in a window screenshot without user interaction.
    if (getenv("NATRON_DEBUG_VR_STARTPITCH") != NULL) {
        _pitchDeg = 75.;
        _fovDeg = 100.;
    }
}

ViewportVR::~ViewportVR()
{
    if ( isValid() ) {
        makeCurrent();
        for (int i = 0; i < 2; ++i) {
            _slots[i].texture.reset();
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

// ============================================================================
// Section 3: look-around / options API
// ============================================================================

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
    bool mustEmitLook = false;
    if (!_lookEmittedOnce || (_pitchDeg != _lastEmittedPitch) || (_yawDeg != _lastEmittedYaw)) {
        mustEmitLook = true;
        _lastEmittedYaw = _yawDeg;
        _lastEmittedPitch = _pitchDeg;
        _lookEmittedOnce = true;
    }
    if (fovDeg < 10) {
        fovDeg = 10;
    } else if (fovDeg > 130) {
        fovDeg = 130;
    }
    const bool fovChangedNow = (fovDeg != _fovDeg);
    _fovDeg = fovDeg;
    update();
    if (fovChangedNow) {
        // Keep the pane's FOV spin box in sync with wheel/reset-driven changes.
        Q_EMIT fovChanged(_fovDeg);
    }
    if (mustEmitLook) {
        // Full-pano renders do not depend on the look, so the only reason to
        // re-render is to complete tiles the engine never delivered (aborted
        // scrub passes); the pane schedules that cheaply.
        Q_EMIT lookChanged();
    }
}

void
ViewportVR::getLook(double* yawDeg, double* pitchDeg, double* fovDeg) const
{
    if (yawDeg) *yawDeg = _yawDeg;
    if (pitchDeg) *pitchDeg = _pitchDeg;
    if (fovDeg) *fovDeg = _fovDeg;
}

void
ViewportVR::resetLook()
{
    setLook(0, 0, 70);
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
ViewportVR::setUserRoIEnabled(bool on)
{
    {
        QMutexLocker k(&_roimutex);
        _roisEnabled = on;
    }
    update();
}

void
ViewportVR::setCompositingOperator(ViewerCompositingOperatorEnum o)
{
    if (_compositingOp == o) {
        return;
    }
    _compositingOp = o;
    update();
}

void
ViewportVR::setVRPlaybackRange(int first, int last)
{
    _rangeFirst = first;
    _rangeLast = last;
}

void
ViewportVR::setInfoViewer(InfoViewerWidget* infoWidget)
{
    _infoViewer = infoWidget;
}

void
ViewportVR::clearVRTextures()
{
    QMutexLocker k(&_slotsMutex);
    for (int i = 0; i < 2; ++i) {
        _slots[i].isVisible = false;
        _slots[i].pickerValid = false;
    }
    update();
}

void
ViewportVR::clearVRSlotB()
{
    {
        QMutexLocker k(&_slotsMutex);
        _slots[1].isVisible = false;
        _slots[1].pickerValid = false;
    }
    update();
}

void
ViewportVR::requestRedrawVR()
{
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
    // immediate-mode fullscreen quad in paintGL. The program is per-context.
    if (_program) {
        glDeleteProgram(_program);
        _program = 0;
    }
    _programReady = false;

    glClearColor(0.08f, 0.08f, 0.09f, 1.f);
    glDisable(GL_DEPTH_TEST);

    const char* vtx =
        "#version 430 compatibility\n"
        "void main() {\n"
        "    gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);\n"
        "}\n";
    // The engine delivers DISPLAY-MANAGED 8-bit RGBA bytes (channels, gain,
    // gamma, colorspace/OCIO all baked CPU-side because isGPUOcioApplicable is
    // false here): no color work left — only spherical sampling + A/B op.
    // Texture memory layout is GL-native: row 0 = image bottom = south pole.
    //
    // Full-frame OFF (cogs unchecked): the engine renders only the FOV window
    // of the pano. The texture is a SUB-RECT of the sphere — it is NOT
    // stretched onto the sphere. The shader maps each fragment to the sphere,
    // converts to pano UV, and only samples the texture when the UV falls
    // inside the committed window; outside the window the pano is simply
    // "not rendered by this pass" and stays background (black).
    const char* frag =
        "#version 430 compatibility\n"
        "uniform sampler2D u_texA;\n"
        "uniform sampler2D u_texB;\n"
        "uniform vec2  u_size;\n"
        "uniform float u_tanHalfFov;\n"
        "uniform float u_aspect;\n"
        "uniform float u_yaw;\n"
        "uniform float u_pitch;\n"
        "uniform vec2  u_uvA0;\n"  // A texture window: low corner (eye crop composed)
        "uniform vec2  u_uvA1;\n"  // A texture window: high corner
        "uniform vec2  u_uvB0;\n"
        "uniform vec2  u_uvB1;\n"
        "uniform int   u_op;\n"    // comparison operator
        "uniform float u_haveB;\n" // 1 when input B displayed
        "uniform float u_subWindow;\n" // 1 when the texture covers only part of the pano
        "uniform float u_wipeX;\n"     // wipe boundary in clip-space x
        "void main() {\n"
        "    vec2 c = (gl_FragCoord.xy / u_size) * 2.0 - 1.0;\n"
        "    vec3 dir = normalize(vec3(c.x * u_tanHalfFov * u_aspect, c.y * u_tanHalfFov, -1.0));\n"
        "    float cy = cos(u_yaw), sy = sin(u_yaw);\n"
        "    dir = vec3(cy * dir.x + sy * dir.z, dir.y, -sy * dir.x + cy * dir.z);\n"
        "    float cp = cos(u_pitch), sp = sin(u_pitch);\n"
        "    dir = vec3(dir.x, cp * dir.y + sp * dir.z, -sp * dir.y + cp * dir.z);\n"
        "    float lon = atan(dir.x, -dir.z);\n"
        "    float lat = asin(clamp(dir.y, -1.0, 1.0));\n"
        "    const float k2Pi = 6.28318530717958647;\n"
        "    const float kPi = 3.1415926535897932;\n"
        "    vec2 uv = vec2(0.5 + lon / k2Pi, 0.5 + lat / kPi);\n" // row0 = bottom (GL-native layout)
        "    vec3 col = vec3(0.0);\n" // not rendered = black (background)
        "    // The texture is a sub-rect of the pano (full-frame OFF): only\n"
        "    // sample when the sphere UV falls inside the committed window.\n"
        "    // NOTE the sub-window UVs are exactly u_uvA0/u_uvA1 (the tile-\n"
        "    // rounded texture region intersected with the pano RoD).\n"
        "    bool inWin = (u_subWindow < 0.5) || (uv.x >= u_uvA0.x && uv.x <= u_uvA1.x && uv.y >= u_uvA0.y && uv.y <= u_uvA1.y);\n"
        "    if (inWin) {\n"
        "        vec2 uvCropA = u_uvA0 + uv * (u_uvA1 - u_uvA0);\n"
        "        vec4 a = texture2D(u_texA, uvCropA);\n"
        "        col = a.rgb;\n"
        "        if (u_haveB > 0.5) {\n"
        "            vec2 uvCropB = u_uvB0 + uv * (u_uvB1 - u_uvB0);\n"
        "            vec4 b = texture2D(u_texB, uvCropB);\n"
        "            if (u_op == 1) {\n"         // wipe-under: B to the right of the line
        "                col = mix(a.rgb, b.rgb, step(u_wipeX, c.x));\n"
        "            } else if (u_op == 2) {\n" // wipe-over: B to the left of the line
        "                col = mix(b.rgb, a.rgb, step(u_wipeX, c.x));\n"
        "            } else if (u_op == 3) {\n" // minus: difference over the whole image
        "                col = abs(a.rgb - b.rgb);\n"
        "            } else if (u_op == 4) {\n" // onion skin: difference over the whole image
        "                col = abs(a.rgb - b.rgb);\n"
        "            }\n"
        "        }\n"
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
        _uTexA        = glGetUniformLocation(_program, "u_texA");
        _uTexB        = glGetUniformLocation(_program, "u_texB");
        _uSize        = glGetUniformLocation(_program, "u_size");
        _uTanHalfFov  = glGetUniformLocation(_program, "u_tanHalfFov");
        _uAspect      = glGetUniformLocation(_program, "u_aspect");
        _uYaw         = glGetUniformLocation(_program, "u_yaw");
        _uPitch       = glGetUniformLocation(_program, "u_pitch");
        _uUvA0        = glGetUniformLocation(_program, "u_uvA0");
        _uUvA1        = glGetUniformLocation(_program, "u_uvA1");
        _uUvB0        = glGetUniformLocation(_program, "u_uvB0");
        _uUvB1        = glGetUniformLocation(_program, "u_uvB1");
        _uOp          = glGetUniformLocation(_program, "u_op");
        _uHaveB       = glGetUniformLocation(_program, "u_haveB");
        _uSubWindow   = glGetUniformLocation(_program, "u_subWindow");
        _uWipeX       = glGetUniformLocation(_program, "u_wipeX");
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
ViewportVR::resizeGL(int /*w*/, int /*h*/)
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
    drawVRImage();
}

void
ViewportVR::drawVRImage()
{
    TexturePtr texA, texB;
    RectD winA(0, 0, 1, 1);
    RectD winB(0, 0, 1, 1);
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slotA = _slots[0];
        VRSlot& slotB = _slots[1];
        texA = slotA.isVisible ? slotA.texture : TexturePtr();
        winA = slotA.winUV;
        bool bActive = slotB.isVisible && (_compositingOp != eViewerCompositingOperatorNone);
        texB = bActive ? slotB.texture : TexturePtr();
        winB = slotB.winUV;
    }

    // ---- stereo crop -----------------------------------------------------
    // s = 0 at the pano's left edge (lon -Pi), t = 0 at the BOTTOM row
    // (GL-native upload layout; over-under LEFT eye = TOP half).
    float uv0[2] = { 0.f, 0.f };
    float uv1[2] = { 1.f, 1.f };
    switch (_layout) {
    case eLayoutOverUnder:
        if (_eye == 0) {
            uv0[1] = 0.5f; uv1[1] = 1.f;
        } else {
            uv0[1] = 0.f; uv1[1] = 0.5f;
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

    // Compose the eye crop with the engine's rendered window (UV per slot).
    // Half-texel inset per texture: the image edge inside the tile-rounded
    // texture must land on the CENTER of its edge texel, otherwise GL_LINEAR
    // at the image border blends the last/first texel with the padded
    // garbage region around it — a thin vertical seam at the wrap line (and a
    // zenith/nadir smear when the height is not a tile multiple).
    float uva0[2], uva1[2], uvb0[2], uvb1[2];
    {
        float wa0 = (float)winA.x1, wa1 = (float)winA.x2;
        float wa0y = (float)winA.y1, wa1y = (float)winA.y2;
        if (texA && (texA->getTextureRect().width() > 1) && (texA->getTextureRect().height() > 1)) {
            const float ha = 0.5f * (float)winA.width() / (float)texA->getTextureRect().width();
            const float va = 0.5f * (float)winA.height() / (float)texA->getTextureRect().height();
            wa0 += ha; wa1 -= ha;
            wa0y += va; wa1y -= va;
        }
        uva0[0] = wa0 + uv0[0] * (wa1 - wa0);
        uva1[0] = wa0 + uv1[0] * (wa1 - wa0);
        uva0[1] = wa0y + uv0[1] * (wa1y - wa0y);
        uva1[1] = wa0y + uv1[1] * (wa1y - wa0y);
    }
    {
        float wb0 = (float)winB.x1, wb1 = (float)winB.x2;
        float wb0y = (float)winB.y1, wb1y = (float)winB.y2;
        if (texB && (texB->getTextureRect().width() > 1) && (texB->getTextureRect().height() > 1)) {
            const float hb = 0.5f * (float)winB.width() / (float)texB->getTextureRect().width();
            const float vb = 0.5f * (float)winB.height() / (float)texB->getTextureRect().height();
            wb0 += hb; wb1 -= hb;
            wb0y += vb; wb1y -= vb;
        }
        uvb0[0] = wb0 + uv0[0] * (wb1 - wb0);
        uvb1[0] = wb0 + uv1[0] * (wb1 - wb0);
        uvb0[1] = wb0y + uv0[1] * (wb1y - wb0y);
        uvb1[1] = wb0y + uv1[1] * (wb1y - wb0y);
    }

    glUseProgram(_program);

    GLuint nameA = texA ? texA->getTexID() : 0;
    GLuint nameB = texB ? texB->getTexID() : 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, nameA);
    glUniform1i(_uTexA, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, nameB);
    glUniform1i(_uTexB, 1);
    glUniform1f(_uHaveB, nameB ? 1.0f : 0.f);

    int op = 0;
    if (nameB) {
        switch (_compositingOp) {
        case eViewerCompositingOperatorWipeUnder:
        case eViewerCompositingOperatorStackUnder:
            op = 1;
            break;
        case eViewerCompositingOperatorWipeOver:
        case eViewerCompositingOperatorStackOver:
            op = 2;
            break;
        case eViewerCompositingOperatorWipeMinus:
        case eViewerCompositingOperatorStackMinus:
            op = 3;
            break;
        case eViewerCompositingOperatorWipeOnionSkin:
        case eViewerCompositingOperatorStackOnionSkin:
            op = 4;
            break;
        case eViewerCompositingOperatorNone:
        default:
            op = 0;
            break;
        }
    }
    glUniform1i(_uOp, op);

    // u_size must be in DEVICE pixels: gl_FragCoord is device-pixel based and
    // the wipe comparison uses the same c.x — in logical pixels on a HiDPI
    // screen the wipe line drifted left of the widget overlay line.
    glUniform2f(_uSize, (GLfloat)(width() * devicePixelRatio()), (GLfloat)(height() * devicePixelRatio()));
    glUniform1f(_uTanHalfFov, (GLfloat)std::tan(_fovDeg * 0.5 * M_PI / 180.));
    glUniform1f(_uAspect, (GLfloat)width() / (GLfloat)height());
    glUniform1f(_uYaw, (GLfloat)(_yawDeg * M_PI / 180.));
    glUniform1f(_uPitch, (GLfloat)(_pitchDeg * M_PI / 180.));
    // Full-frame OFF: the texture only spans the FOV window — tell the shader
    // to treat the rest of the pano as not-rendered (black). Only meaningful
    // when the committed window is actually smaller than the pano.
    const bool subWindow = !_fullFrameProcessing && texA
                          && (winA.x2 - winA.x1 < 1. - 1e-6
                              || winA.y2 - winA.y1 < 1. - 1e-6);
    glUniform1f(_uSubWindow, subWindow ? 1.0f : 0.f);
    // wipe boundary position in clip space (the overlay line the user sees).
    // The shader's c = fragCoord/u_size*2-1 is device-pixel based (u_size now
    // device px), and clip space is resolution-independent, so this stays a
    // plain logical-to-clip mapping.
    {
        const double w = width();
        const double mixX = (_wipeMixX >= 0.) ? _wipeMixX : 0.5 * w;
        glUniform1f(_uWipeX, w > 0 ? (GLfloat)(mixX / w * 2. - 1.) : 0.f);
    }
    glUniform2f(_uUvA0, uva0[0], uva0[1] );
    glUniform2f(_uUvA1, uva1[0], uva1[1] );
    glUniform2f(_uUvB0, uvb0[0], uvb0[1] );
    glUniform2f(_uUvB1, uvb1[0], uvb1[1] );

    glBegin(GL_QUADS);
    glVertex2d(-1.0, -1.0);
    glVertex2d( 1.0, -1.0);
    glVertex2d( 1.0,  1.0);
    glVertex2d(-1.0,  1.0);
    glEnd();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_LINE_SMOOTH);
    glPointSize(1.f);
    glColor4f(1., 1., 1., 1.);

    // Wipe/compare overlay: the 2D viewer's cross-like wipe control, drawn in
    // widget space. Visible only for the wipe variants (stack/minus/onion have
    // no positional handle).
    const bool haveB = (bool)texB;
    const bool isWipeOp = (_compositingOp == eViewerCompositingOperatorWipeUnder) ||
                          (_compositingOp == eViewerCompositingOperatorWipeOver);
    if (haveB && isWipeOp) {
        drawWipeOverlay();
    }
}

void
ViewportVR::drawWipeOverlay()
{
    // The 2D wipe control (ViewerGL::drawWipeControl) simplified for VR: a
    // vertical mixing line across the canvas at the mix position + horizontal
    // mix handle + dot. Drawn right after the pano quad with identity
    // matrices (clip space -1..1); widget coordinates are converted to clip
    // space first.
    const double w = width();
    const double h = height();
    if ( (w < 1) || (h < 1) ) {
        return;
    }
    // mix position follows the last mouse x while dragging the line (the
    // 2D viewer's mix handle); default center.
    const double mixX = (_wipeMixX >= 0.) ? _wipeMixX : 0.5 * w;
    // to clip space (y flip for the mid line)
    const double cx = mixX / w * 2. - 1.;
    const double hx = kVRWipeMixHandleLength / w * 2.;
    const double cyMid = 0.; // widget middle

    GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_POINT_BIT | GL_COLOR_BUFFER_BIT);
    {
        GLProtectMatrix p(GL_PROJECTION);
        glLoadIdentity();
        {
            GLProtectMatrix m(GL_MODELVIEW);
            glLoadIdentity();
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_LINE_SMOOTH);
            glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
            glLineWidth(1.5 * devicePixelRatio());

            // draw twice: shadow then line (ViewerGL's scheme)
            for (int l = 0; l < 2; ++l) {
                const double c = (l == 0) ? 0. : 0.8;
                glColor4f(c, c, c, 1.);
                glBegin(GL_LINES);
                // vertical wipe boundary (shadow offset by 1px)
                glVertex2d(cx + ((l == 0) ? 2. / w : -2. / w), -1.);
                glVertex2d(cx + ((l == 0) ? 2. / w : -2. / w),  1.);
                // small horizontal mix handle through the center
                glVertex2d(cx - hx, cyMid);
                glVertex2d(cx + hx, cyMid);
                glEnd();
            }
            glPointSize(5. * devicePixelRatio());
            glEnable(GL_POINT_SMOOTH);
            glBegin(GL_POINTS);
            glColor4f(0.8, 0.8, 0.8, 1.);
            glVertex2d(cx, cyMid);
            glEnd();
            glPointSize(1.f);
            glColor4f(1., 1., 1., 1.);
        }
    }
}

// ============================================================================
// Section 5: OpenGLViewerI (the engine → canvas contract)
// ============================================================================

bool
ViewportVR::isViewerUIVisible() const
{
    return isVisible();
}

void
ViewportVR::swapOpenGLBuffers()
{
    // No plugin overlays are drawn through this canvas; Qt swaps for us.
}

void
ViewportVR::redraw()
{
    update();
}

void
ViewportVR::getViewportSize(double &width, double &height) const
{
    width = this->width();
    height = this->height();
}

void
ViewportVR::getPixelScale(double & xScale, double & yScale) const
{
    xScale = 1.;
    yScale = 1.;
}

double
ViewportVR::getScreenPixelRatio() const
{
    return devicePixelRatio();
}

void
ViewportVR::getBackgroundColour(double &r, double &g, double &b) const
{
    r = 0.08;
    g = 0.08;
    b = 0.09;
}

RectD
ViewportVR::getViewportRect() const
{
    return RectD(0., (double)height(), (double)width(), 0.);
}

void
ViewportVR::getCursorPosition(double& x, double& y) const
{
    QPointF pos = mapFromGlobal(QCursor::pos());
    x = pos.x();
    y = pos.y();
}

TimeLinePtr
ViewportVR::getTimeline() const
{
    GuiAppInstancePtr app = _gui ? _gui->getApp() : GuiAppInstancePtr();
    return app ? app->getTimeLine() : TimeLinePtr();
}

double
ViewportVR::getZoomFactor() const
{
    // The engine picks the render mip level from this zoom. Rendering the
    // full-res pano every scrub step is wasteful: the sampler eats roughly
    // (panoH * fov / pi) texels to fill the widget's height, so report the
    // display-derived zoom (screen rows per pano row) — the GPU LINEAR filter
    // upscales, preview quality is unaffected and the clamped zoom stays <= 1
    // so nothing renders beyond full res.
    const double fovRad = _fovDeg * M_PI / 180.;
    const double displayRows = (double)_lastRodH * fovRad / M_PI;
    if (displayRows < 1. || height() < 1) {
        return 1.;
    }
    double zoom = (double)height() / displayRows;
    if (zoom > 1.) {
        zoom = 1.;
    } else if (zoom < 0.01) {
        zoom = 0.01; // never request absurd mips
    }
    return zoom;
}

void
ViewportVR::fitImageToFormat()
{
    resetLook();
}

bool
ViewportVR::isClippingImageToFormat() const
{
    return true;
}

RectI
ViewportVR::getImageRectangleDisplayed(const RectI & imageRoDPixel,
                                       const double par,
                                       unsigned int mipmapLevel)
{
    // The whole (2:1) pano is rendered by default (full-frame processing, the
    // cogs button ON). When full-frame processing is OFF (cogs unchecked),
    // clip the rendered ROI to the pano slice visible through the current FOV
    // — the faster path. That ROI is a SUB-RECT of the pano: the shader
    // samples it at its sphere position (winUV) and leaves the rest of the
    // sphere black (never stretched).
    Q_UNUSED(par);
    Q_UNUSED(mipmapLevel);
    if (!_fullFrameProcessing) {
        const double fovRad = _fovDeg * M_PI / 180.;
        const double yawRad = _yawDeg * M_PI / 180.;
        const double pitchRad = _pitchDeg * M_PI / 180.;
        const double latSpan = std::min(fovRad / 2., M_PI / 2.);
        const double cosPitch = std::max(1e-3, std::cos(pitchRad));
        const double lonSpan = std::min(fovRad / (2. * cosPitch), M_PI);
        double latMin = pitchRad - latSpan;
        double latMax = pitchRad + latSpan;
        if (latMin < -M_PI / 2.) { latMin = -M_PI / 2.; }
        if (latMax > M_PI / 2.) { latMax = M_PI / 2.; }
        double lonMin = yawRad - lonSpan;
        double lonMax = yawRad + lonSpan;
        const int W = imageRoDPixel.width();
        const int H = imageRoDPixel.height();
        if ( (W > 0) && (H > 0) ) {
            auto wrapLon = [](double u) {
                while (u < 0.) { u += 1.; }
                while (u > 1.) { u -= 1.; }
                return u;
            };
            const double u0 = wrapLon(0.5 + lonMin / (2. * M_PI));
            const double u1 = wrapLon(0.5 + lonMax / (2. * M_PI));
            const double v0 = 0.5 + latMin / M_PI;
            const double v1 = 0.5 + latMax / M_PI;
            RectI roi;
            const int x1 = (int)std::floor( std::min(u0, u1) * W );
            const int x2 = (int)std::ceil( std::max(u0, u1) * W );
            roi.x1 = imageRoDPixel.x1 + std::max(0, std::min(W, x1));
            roi.x2 = imageRoDPixel.x1 + std::max(0, std::min(W, x2));
            if (roi.x2 <= roi.x1) {
                roi.x1 = imageRoDPixel.x1;
                roi.x2 = imageRoDPixel.x2;
            }
            roi.y1 = imageRoDPixel.y1 + std::max(0, std::min(H, (int)std::floor(v0 * H)));
            roi.y2 = imageRoDPixel.y1 + std::max(0, std::min(H, (int)std::ceil(v1 * H)));
            if (roi.y2 <= roi.y1) {
                roi.y1 = imageRoDPixel.y1;
                roi.y2 = imageRoDPixel.y2;
            }
            RectI clipped = roi.intersect(imageRoDPixel);
            if (!clipped.isNull()) {
                return clipped;
            }
        }
    }
    return imageRoDPixel;
}

RectI
ViewportVR::getImageRectangleDisplayedRoundedToTileSize(int texIndex,
                                                        const RectD & rod,
                                                        const double par,
                                                        unsigned int mipmapLevel,
                                                        std::vector<RectI>* tiles,
                                                        std::vector<RectI>* tilesRounded,
                                                        int *tileSize,
                                                        RectI* roiNotRounded)
{
    const RectI bounds = rod.toPixelEnclosing(mipmapLevel, par);
    const RectI roi = getImageRectangleDisplayed(bounds, par, mipmapLevel);

    if (roiNotRounded) {
        *roiNotRounded = roi;
    }

    int tileSizeLocal = ipow( 2, appPTR->getCurrentSettings()->getViewerTilesPowerOf2() );
    RectI texRect;
    texRect.x1 = (int)std::floor( ( (double)roi.x1 ) / tileSizeLocal ) * (double)tileSizeLocal;
    texRect.y1 = (int)std::floor( ( (double)roi.y1 ) / tileSizeLocal ) * (double)tileSizeLocal;
    texRect.x2 = (int)std::ceil( ( (double)roi.x2 ) / tileSizeLocal ) * (double)tileSizeLocal;
    texRect.y2 = (int)std::ceil( ( (double)roi.y2 ) / tileSizeLocal ) * (double)tileSizeLocal;

    // CRITICAL contract: the engine sizes each cached tile's byte buffer to a
    // full tileSize² (ViewereInstance.cpp "the internal buffer is rounded to
    // the tile size anyway"), so the tiles handed out here MUST tile the viewer
    // ROI at tileSize steps. One big tile made the engine allocate buffers for
    // only a fraction of the pano (the black-chunk/blade artifact). Same loop
    // as ViewerGL's twin.
    if (tilesRounded) {
        for (int y = texRect.y1; y < texRect.y2; y += tileSizeLocal) {
            int y2 = std::min(y + tileSizeLocal, texRect.y2);
            for (int x = texRect.x1; x < texRect.x2; x += tileSizeLocal) {
                tilesRounded->resize(tilesRounded->size() + 1);
                RectI& tile = tilesRounded->back();
                tile.x1 = x;
                tile.x2 = std::min(x + tileSizeLocal, texRect.x2);
                tile.y1 = y;
                tile.y2 = y2;
                assert( texRect.contains(tile) );
                if (tiles) {
                    tiles->push_back(tile.intersect(bounds)); // cacheable part
                }
            }
        }
    }

    if (tileSize) {
        *tileSize = tileSizeLocal;
    }
    return texRect;
}

RectI
ViewportVR::getExactImageRectangleDisplayed(int texIndex,
                                            const RectD & rod,
                                            const double par,
                                            unsigned int mipmapLevel)
{
    const RectI bounds = rod.toPixelEnclosing(mipmapLevel, par);
    return getImageRectangleDisplayed(bounds, par, mipmapLevel);
}

ImageBitDepthEnum
ViewportVR::getBitDepth() const
{
    // 8-bit transfer: the engine bakes the full color pipeline (channels, gain,
    // gamma, LUT / OCIO CPU path) into the bytes — the VR sampler needs no
    // color work, leaving only the spherical mapping and the A/B op.
    return eImageBitDepthByte;
}

bool
ViewportVR::isUserRegionOfInterestEnabled() const
{
    // deliberately disabled while the FOV-window ROI mode is parked (VR_PLAN
    // 5b may re-introduce it with a drag-to-select rect over the pano).
    QMutexLocker k(&_roimutex);
    return false;
}

RectD
ViewportVR::getUserRegionOfInterest() const
{
    // "User ROI" in VR = the FOV window; the engine bounds the rod with the
    // pixel-space ROI computed from getImageRectangleDisplayed() instead, so
    // the canonical rect spans the full pano (the full rect intersected with
    // the rendered pixel ROI is applied there already). RectD order is
    // (x1, y1, x2, y2) — completely open in every direction.
    const double inf = std::numeric_limits<double>::infinity();
    return RectD(-inf, -inf, inf, inf);
}

void
ViewportVR::clearPartialUpdateTextures()
{
    // no partial textures in VR
}

void
ViewportVR::transferBufferFromRAMtoGPU(const unsigned char* ramBuffer,
                                       size_t bytesCount,
                                       const RectI &roiRoundedToTileSize,
                                       const RectI& roi,
                                       const TextureRect & tileRect,
                                       int textureIndex,
                                       bool isPartialRect,
                                       bool isFirstTile,
                                       TexturePtr* texture)
{
    Q_UNUSED(isFirstTile);
    Q_UNUSED(isPartialRect);
    Q_UNUSED(roi);
    assert(textureIndex == 0 || textureIndex == 1);
    assert(ramBuffer);
    // CPU staging only — NO GL here: the atomic commit happens in
    // endTransferBufferFromRAMtoGPU (exactly once per frame per A/B index),
    // so a partially delivered frame can never reach the canvas, and no GL
    // lock is held on the per-tile path (scrub keeps the UI fluid).

    TextureRect textureRectangle;
    textureRectangle.par = tileRect.par;
    textureRectangle.closestPo2 = tileRect.closestPo2;
    textureRectangle.set(roiRoundedToTileSize);

    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];

        if ( !slot.stagedValid ||
             (slot.stagedRect.x1 != textureRectangle.x1) || (slot.stagedRect.y1 != textureRectangle.y1) ||
             (slot.stagedRect.x2 != textureRectangle.x2) || (slot.stagedRect.y2 != textureRectangle.y2) ) {
            // New frame / new ROI: reset staging.
            slot.stagedValid = true;
            slot.stagedRect.set(textureRectangle);
            slot.stagedTiles.clear();
            slot.stagedArea = 0;
            slot.staged.assign( (std::size_t)textureRectangle.width() * (std::size_t)textureRectangle.height() * 4, 0 );
        }

        // Copy this tile into its sub-rect of the staging buffer. The engine's
        // cached tiles round rows/cols to the tile size, so the tile's byte
        // rows are tileRect.width()-strided — exactly matching the memcpy.
        const std::size_t rowBytes = (std::size_t)textureRectangle.width() * 4;
        const std::size_t dstY = (std::size_t)(tileRect.y1 - textureRectangle.y1);
        const std::size_t dstXBytes = (std::size_t)(tileRect.x1 - textureRectangle.x1) * 4;
        const std::size_t tileRows = (std::size_t)tileRect.height();
        const std::size_t srcRowBytes = (std::size_t)tileRect.width() * 4;
        for (std::size_t row = 0; row < tileRows; ++row) {
            std::memcpy( &slot.staged[ dstY * rowBytes + dstXBytes + row * rowBytes ],
                         ramBuffer + row * srcRowBytes,
                         srcRowBytes );
        }
        slot.stagedTiles.push_back( RectI(tileRect.x1, tileRect.y1, tileRect.x2, tileRect.y2) );
        slot.stagedArea += (unsigned long long)tileRect.width() * (unsigned long long)tileRect.height();
    }

    // Legacy API surface: the engine reuses whatever texture object the
    // previous transfers produced; the true commit happens below.
    QMutexLocker k(&_slotsMutex);
    TexturePtr tex = _slots[textureIndex].texture;
    k.unlock();
    *texture = tex;
}

void
ViewportVR::endTransferBufferFromRAMToGPU(int textureIndex,
                                          const TexturePtr& texture,
                                          const ImagePtr& image,
                                          int time,
                                          const RectD& rod,
                                          double par,
                                          ImageBitDepthEnum depth,
                                          unsigned int mipmapLevel,
                                          ImagePremultiplicationEnum premult,
                                          double gain,
                                          double gamma,
                                          double offset,
                                          int lut,
                                          bool recenterViewer,
                                          const Point& viewportCenter,
                                          bool isPartialRect)
{
    Q_UNUSED(texture);
    Q_UNUSED(image);
    Q_UNUSED(depth);
    Q_UNUSED(premult);
    Q_UNUSED(gain);
    Q_UNUSED(gamma);
    Q_UNUSED(offset);
    Q_UNUSED(lut);
    Q_UNUSED(recenterViewer);
    Q_UNUSED(viewportCenter);
    Q_UNUSED(isPartialRect);
    assert( QThread::currentThread() == qApp->thread() );

    // ---- atomic commit (called once per frame per A/B index) --------------
    // The engine made the GL context current before updateViewer, so GL calls
    // are legal on this thread. Only commit when the staged tiles cover the
    // whole staged rectangle — an aborted/incomplete pass is dropped and the
    // previous frame stays on screen (no mixed-frame artifacts, no holes; the
    // next complete pass re-renders them).
    bool complete = false;
    RectI stagedRect;
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        if (slot.stagedValid) {
            stagedRect = slot.stagedRect;
            unsigned long long full = (unsigned long long)stagedRect.width() * (unsigned long long)stagedRect.height();
            complete = (slot.stagedArea >= full);
        }
        if (!complete) {
            // drop the partial staging: next pass restarts it
            slot.stagedValid = false;
            slot.stagedTiles.clear();
            slot.stagedArea = 0;
        }
    }
    if (!complete) {
        return;
    }

    if ( QOpenGLContext::currentContext() != context() ) {
        makeCurrent();
    }

    // slot texture: (re)create on size change
    TexturePtr tex;
    int w, h;
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        w = stagedRect.width();
        h = stagedRect.height();
        tex = slot.texture;
        if (!tex || (tex->type() != Texture::eDataTypeByte) || (tex->w() != w) || (tex->h() != h)) {
            int format, internalFormat, glType;
            Texture::getRecommendedTexParametersForRGBAByteTexture(&format, &internalFormat, &glType);
            tex.reset( new Texture(GL_TEXTURE_2D, GL_LINEAR, GL_LINEAR /*mip off*/, GL_CLAMP_TO_EDGE,
                                   Texture::eDataTypeByte, format, internalFormat, glType) );
            slot.texture = tex;
        }
    }

    // ---- upload through the double-buffered PBO pool (ViewerGL scheme) ----
    // Context FIRST: PBO handles are context resources.
    makeCurrent();
    if ( _pboIds.size() < 2 ) {
        while ( _pboIds.size() < 2 ) {
            GLuint id = 0;
            glGenBuffers(1, &id);
            _pboIds.push_back(id);
        }
    }
    const GLuint pboId = _pboIds[_pboIndex];
    _pboIndex = (_pboIndex + 1) % 2;

    const std::size_t bytes = (std::size_t)w * (std::size_t)h * 4;
    const GLvoid* uploadPtr = 0;
    std::vector<unsigned char> flat; // released after the upload
    unsigned char* stagedCopy = 0;
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        slot.staged.swap(flat); // steal the staged bytes
        // keep the committed bytes for the info bar / color picker until the
        // next commit of this slot replaces them (they were about to be
        // freed anyway, so this costs no extra allocation while it lives)
        if ( !slot.picker.empty() && (slot.picker.size() >= bytes) ) {
            std::memcpy( &slot.picker.front(), flat.empty() ? (const void*)0 : &flat.front(), bytes );
        } else {
            slot.picker.assign( flat.begin(), flat.end() );
        }
        slot.pickerBytes = bytes;
        slot.pickerRect = stagedRect;
        slot.pickerValid = true;
        slot.staged.clear();
        slot.stagedValid = false;
        slot.stagedTiles.clear();
        slot.stagedArea = 0;
    }
    stagedCopy = flat.empty() ? 0 : &flat.front();

    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboId);
    glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, bytes, stagedCopy, GL_STREAM_DRAW_ARB);
    uploadPtr = 0; // read from the bound PBO at offset 0

    // Allocate storage for freshly created textures (Texture only generates the
    // canvas name; without ensureTextureHasSize the glTexSubImage2D lands on an
    // unsized texture and silently draws nothing).
    {
        TextureRect allocRect;
        allocRect.set(0, 0, w, h);
        allocRect.par = 1.;
        allocRect.closestPo2 = 1;
        tex->ensureTextureHasSize(allocRect, 0);
    }

    glBindTexture(GL_TEXTURE_2D, tex->getTexID());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    0, 0, w, h,
                    tex->getFormat(), tex->getGLType(),
                    uploadPtr);
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);

    GLint currentBoundPBO = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB, &currentBoundPBO);
    Q_UNUSED(currentBoundPBO);

    // ---- commit the slot metadata + winUV ----------------------------------
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        slot.isVisible = true;
        slot.time = time;
        slot.rod = rod;
        _lastRodH = (int)std::lround( rod.height() );
        slot.par = par;
        // The GL texture spans the STAGED (tile-rounded) rect — which is larger
        // than the image whenever the image dims are not tile multiples (e.g.
        // a 1920x1080 source inside a 2048x2048 texture). The usable sphere
        // content is the IMAGE region inside that texture, so winUV maps the
        // unit square onto the image extent RELATIVE TO THE TEXTURE SIZE:
        // uvCrop.y = winUV.y2 lands exactly on the image's top row.
        const RectI rodPx = rod.toPixelEnclosing(mipmapLevel, par);
        const RectI imgPx = stagedRect.intersect(rodPx);
        const double texW = (double)stagedRect.width();
        const double texH = (double)stagedRect.height();
        if ( (texW > 0.) && (texH > 0.) ) {
            slot.winUV.x1 = (double)(imgPx.x1 - stagedRect.x1) / texW;
            slot.winUV.x2 = (double)(imgPx.x2 - stagedRect.x1) / texW;
            slot.winUV.y1 = (double)(imgPx.y1 - stagedRect.y1) / texH;
            slot.winUV.y2 = (double)(imgPx.y2 - stagedRect.y1) / texH;
        } else {
            slot.winUV = RectD(0, 0, 1, 1);
        }
        slot.mipmapLevel = mipmapLevel;
    }
    // The info bar mirrors exactly the committed frame: format, resolution,
    // RoD, and the "~" approximation flag when a proxy mip is displayed.
    updateInfoStatics(textureIndex, image, depth, par, rod, mipmapLevel);
    update();
}

void
ViewportVR::disconnectInputTexture(int textureIndex, bool clearRoD)
{
    // The engine signals this when a render FAILED or returned BLACK (e.g.
    // the Alpha display channel on an input without alpha): drop the slot so
    // the stale last frame does not stick on screen — same as ViewerGL,
    // which sets isVisible=false here. (Keeping the frame caused the
    // "Alpha shows the old RGB / scrub never updates" artifacts: every
    // black result left the previous texture committed.)
    assert(textureIndex == 0 || textureIndex == 1);
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        slot.isVisible = false;
        slot.pickerValid = false;
        slot.stagedValid = false;
        slot.stagedTiles.clear();
        slot.stagedArea = 0;
        if (clearRoD) {
            slot.rod = RectD();
            slot.winUV = RectD(0, 0, 1, 1);
        }
    }
    update();
}

void
ViewportVR::updateInfoStatics(int textureIndex,
                              const ImagePtr& image,
                              ImageBitDepthEnum depth,
                              double par,
                              const RectD& rod,
                              unsigned int mipmapLevel)
{
    Q_UNUSED(mipmapLevel);
    if ( !_infoViewer || !_infoViewer->isVisible() ) {
        return;
    }
    if ( _gui && _gui->isGUIFrozen() ) {
        return;
    }
    if (textureIndex != 0) {
        // B slot: report into the second info bar ("B:") when the pane has one.
        if ( !_infoViewerB || !_infoViewerB->isVisible() ) {
            return;
        }
        if (image) {
            _infoViewerB->setImageFormat( image->getComponents(), depth );
        }
        const RectI pixelRodB = rod.toPixelEnclosing(0, par);
        _infoViewerB->setDataWindow(pixelRodB);
        QString infoBarNameB;
        {
            std::string formatName;
            bool named = false;
            if (_gui) {
                GuiAppInstancePtr app = _gui->getApp();
                if (app) {
                    ProjectPtr project = app->getProject();
                    if (project) {
                        named = project->getFormatNameFromRect(pixelRodB, par, &formatName);
                    }
                }
            }
            if (named) {
                infoBarNameB = QString::fromUtf8( formatName.c_str() );
                infoBarNameB += QString::fromUtf8(" %1x%2").arg(pixelRodB.width()).arg(pixelRodB.height());
            } else {
                infoBarNameB = QString::fromUtf8("%1x%2").arg(pixelRodB.width()).arg(pixelRodB.height());
            }
        }
        _infoViewerB->setResolution(infoBarNameB);
        return;
    }

    if (image) {
        _infoViewer->setImageFormat( image->getComponents(), depth );
    }
    const RectI pixelRod = rod.toPixelEnclosing(0, par);
    _infoViewer->setDataWindow(pixelRod);

    // resolution: the project format name when the RoD matches one, else plain WxH
    QString infoBarName;
    {
        std::string formatName;
        bool named = false;
        if (_gui) {
            GuiAppInstancePtr app = _gui->getApp();
            if (app) {
                ProjectPtr project = app->getProject();
                if (project) {
                    named = project->getFormatNameFromRect(pixelRod, par, &formatName);
                }
            }
        }
        if (named) {
            infoBarName = QString::fromUtf8( formatName.c_str() );
            infoBarName += QString::fromUtf8(" %1x%2").arg(pixelRod.width()).arg(pixelRod.height());
        } else {
            infoBarName = QString::fromUtf8("%1x%2").arg(pixelRod.width()).arg(pixelRod.height());
        }
    }
    _infoViewer->setResolution(infoBarName);
}

bool
ViewportVR::pickColorAtWidgetPos(int widgetX, int widgetY, float* r, float* g, float* b, float* a, bool* approx)
{
    return pickColorAtWidgetPosSlot(0, widgetX, widgetY, r, g, b, a, approx);
}

bool
ViewportVR::pickColorAtWidgetPosSlot(int slotIdx,
                                     int widgetX,
                                     int widgetY,
                                     float* r,
                                     float* g,
                                     float* b,
                                     float* a,
                                     bool* approx)
{
    if (r) *r = 0.f;
    if (g) *g = 0.f;
    if (b) *b = 0.f;
    if (a) *a = 0.f;
    if (approx) *approx = false;
    if ( (slotIdx != 0) && (slotIdx != 1) ) {
        return false;
    }
    if ( (widgetX < 0) || (widgetY < 0) || (widgetX >= width()) || (widgetY >= height()) ) {
        return false;
    }

    // Snapshot the committed-frame geometry; the 4 bytes themselves are read
    // under the lock right before returning.
    RectI pickerRect;
    RectD win;
    bool visible = false, pickerValid = false;
    unsigned int mip = 0;
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[slotIdx];
        visible = slot.isVisible;
        pickerValid = slot.pickerValid;
        pickerRect = slot.pickerRect;
        win = slot.winUV;
        mip = slot.mipmapLevel;
    }
    if (!visible || !pickerValid) {
        return false;
    }
    if ( (std::size_t)pickerRect.width() * (std::size_t)pickerRect.height() * 4 == 0 ) {
        return false;
    }
    if (approx) *approx = (mip > 0);

    // ---- replicate the fragment shader's spherical path for one sample -----
    const double fragX = (double)widgetX + 0.5;
    const double fragY = (double)height() - (double)widgetY - 0.5; // GL bottom-origin
    double cX = (fragX / (double)width() ) * 2. - 1.;
    double cY = (fragY / (double)height() ) * 2. - 1.;
    double tanHalf = std::tan(_fovDeg * 0.5 * M_PI / 180.);
    double aspect = (double)width() / (double)height();
    double dx = cX * tanHalf * aspect;
    double dy = cY * tanHalf;
    double dz = -1.;
    {
        double len = std::sqrt(dx*dx + dy*dy + dz*dz);
        dx /= len; dy /= len; dz /= len;
    }
    {
        const double yr = _yawDeg * M_PI / 180.;
        const double cy = std::cos(yr), sy = std::sin(yr);
        const double nx = cy * dx + sy * dz;
        dz = -sy * dx + cy * dz;
        dx = nx;
    }
    {
        const double pr = _pitchDeg * M_PI / 180.;
        const double cp = std::cos(pr), sp = std::sin(pr);
        const double ny = cp * dy + sp * dz;
        dz = -sp * dy + cp * dz;
        dy = ny;
    }
    double lon = std::atan2(dx, -dz);
    double lat = std::asin( Image::clamp(dy, -1., 1.) );
    const double k2Pi = 2. * M_PI;
    double u = 0.5 + lon / k2Pi; // 0 at the pano's left edge, bottom-origin v
    double v = 0.5 + lat / M_PI;

    // Sub-window render (full-frame OFF): the texture only covers a sub-rect
    // of the pano — the picker reads committed pano-space pixels, so nothing
    // to sample when the UV falls outside the window (matches the shader's
    // black background).
    if ( (win.x2 - win.x1 < 1. - 1e-6) || (win.y2 - win.y1 < 1. - 1e-6) ) {
        if ( (u < win.x1) || (u > win.x2) || (v < win.y1) || (v > win.y2) ) {
            return false;
        }
    }

    // Compose the eye crop with the committed window (same math as drawVRImage)
    double winX1 = win.x1, winX2 = win.x2, winY1 = win.y1, winY2 = win.y2;
    double uv0x = 0., uv0y = 0., uv1x = 1., uv1y = 1.;
    switch (_layout) {
    case eLayoutOverUnder:
        if (_eye == 0) {
            uv0y = 0.5; uv1y = 1.;
        } else {
            uv0y = 0.; uv1y = 0.5;
        }
        break;
    case eLayoutSideBySide:
        uv0x = (_eye == 0) ? 0. : 0.5;
        uv1x = (_eye == 0) ? 0.5 : 1.;
        break;
    case eLayoutMono:
    default:
        break;
    }
    {
        const double wx1 = winX1 + uv0x * (winX2 - winX1);
        const double wx2 = winX1 + uv1x * (winX2 - winX1);
        const double wy1 = winY1 + uv0y * (winY2 - winY1);
        const double wy2 = winY1 + uv1y * (winY2 - winY1);
        winX1 = wx1; winX2 = wx2; winY1 = wy1; winY2 = wy2;
    }

    const int texW = pickerRect.width();
    const int texH = pickerRect.height();
    if ( (texW < 1) || (texH < 1) ) {
        return false;
    }
    int col = (int)std::floor( ( winX1 + u * (winX2 - winX1) ) * (double)texW );
    int row = (int)std::floor( ( winY1 + v * (winY2 - winY1) ) * (double)texH );
    if ( (col < pickerRect.x1) || (col >= pickerRect.x2) || (row < pickerRect.y1) || (row >= pickerRect.y2) ) {
        return false;
    }
    const std::size_t offset = ( (std::size_t)(row - pickerRect.y1) * (std::size_t)texW
                                 + (std::size_t)(col - pickerRect.x1) ) * 4;
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[slotIdx];
        if (!slot.pickerValid || (slot.picker.size() < offset + 4)) {
            return false;
        }
        // The engine packs display bytes as BGRA (toBGRA: a<<24|r<<16|g<<8|b,
        // uploaded with GL_BGRA / UNSIGNED_INT_8_8_8_8_REV). In the staged
        // byte buffer that's byte order B,G,R,A per pixel.
        if (r) *r = (float)slot.picker[offset + 2] / 255.f;
        if (g) *g = (float)slot.picker[offset + 1] / 255.f;
        if (b) *b = (float)slot.picker[offset] / 255.f;
        if (a) *a = (float)slot.picker[offset + 3] / 255.f;
    }
    return true;
}


void
ViewportVR::updateColorPicker(int, int, int)
{
    // color picker not in the VR v1
}

void
ViewportVR::getTextureColorAt(int, int, double* r, double* g, double* b, double* a)
{
    if (r) *r = 0;
    if (g) *g = 0;
    if (b) *b = 0;
    if (a) *a = 0;
}

void
ViewportVR::makeOpenGLcontextCurrent()
{
    makeCurrent();
}

void
ViewportVR::removeGUI()
{
    // The pane owns the widget lifecycle (ViewerTab-like model).
}

ViewIdx
ViewportVR::getCurrentView() const
{
    // VR is single-view for now (stereo via top-bottom / side-by-side packing).
    return ViewIdx(0);
}

int
ViewportVR::getCurrentlyDisplayedTime() const
{
    GuiAppInstancePtr app = _gui ? _gui->getApp() : GuiAppInstancePtr();
    return app ? (int)app->getTimeLine()->currentFrame() : 0;
}

void
ViewportVR::getViewerFrameRange(int* first, int* last) const
{
    if (first) {
        *first = _rangeFirst;
    }
    if (last) {
        *last = _rangeLast;
    }
}

void
ViewportVR::saveOpenGLContext()
{
    // The VR view never shows plugin overlays; nothing to stash.
}

void
ViewportVR::restoreOpenGLContext()
{
    // ditto
}

void
ViewportVR::clearLastRenderedImage()
{
    for (int i = 0; i < 2; ++i) {
        _slots[i].texture.reset();
        _slots[i].isVisible = false;
        _slots[i].pickerValid = false;
    }
    update();
}

void
ViewportVR::redrawNow()
{
    update();
}

void
ViewportVR::toWidgetCoordinates(double *x, double *y) const
{
    if (x) {
        *x = ( *x + 1. ) / 2. * width();
    }
    if (y) {
        *y = ( 1. - *y ) / 2. * height();
    }
}

void
ViewportVR::toCanonicalCoordinates(double *x, double *y) const
{
    if (x) {
        *x = *x * 2. / width() - 1.;
    }
    if (y) {
        *y = 1. - *y * 2. / height();
    }
}

int
ViewportVR::getWidgetFontHeight() const
{
    return 10;
}

int
ViewportVR::getStringWidthForCurrentFont(const std::string& string) const
{
    return (int)string.size() * 5;
}

// ============================================================================
// Section 6: mouse interaction
// ============================================================================

void
ViewportVR::mousePressEvent(QMouseEvent* e)
{
    if ( e->button() == Qt::LeftButton ) {
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
    // The info bar follows the cursor on every move (2D ViewerGL behavior);
    // the look-around drag is separate.
    updateInfoWidgetAtPos( e->x(), e->y() );
    if (!_dragging || ( (_lastMouseX < 0) || (_lastMouseY < 0) )) {
        QOpenGLWidget::mouseMoveEvent(e);
        return;
    }
    int dx = e->x() - _lastMouseX;
    int dy = e->y() - _lastMouseY;
    _lastMouseX = e->x();
    _lastMouseY = e->y();

    int base = height() > 0 ? height() : 480;
    double k = _fovDeg / (double)std::max(240, base);
    setLook(_yawDeg + dx * k, _pitchDeg - dy * k, _fovDeg);
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
    double factor = std::pow(0.92, (double)delta);
    setLook(_yawDeg, _pitchDeg, _fovDeg * factor);
}

void
ViewportVR::leaveEvent(QEvent* e)
{
    if (_infoViewer) {
        if ( _infoViewer->colorVisible() ) {
            _infoViewer->hideColorInfo();
        }
        if ( _infoViewer->mouseVisible() ) {
            _infoViewer->hideMouseInfo();
        }
    }
    if (_infoViewerB) {
        if ( _infoViewerB->colorVisible() ) {
            _infoViewerB->hideColorInfo();
        }
        if ( _infoViewerB->mouseVisible() ) {
            _infoViewerB->hideMouseInfo();
        }
    }
    QOpenGLWidget::leaveEvent(e);
}

void
ViewportVR::updateInfoWidgetAtPos(int widgetX, int widgetY)
{
    if ( !_infoViewer || !_infoViewer->isVisible() ) {
        return;
    }
    if ( _gui && _gui->isGUIFrozen() ) {
        return;
    }
    if ( !_infoViewer->mouseVisible() ) {
        _infoViewer->showMouseInfo();
    }
    _infoViewer->setMousePos( QPoint(widgetX, widgetY) );

    float r, g, b, a;
    bool approx = false;
    if ( pickColorAtWidgetPosSlot(0, widgetX, widgetY, &r, &g, &b, &a, &approx) ) {
        _infoViewer->setColorApproximated(approx);
        _infoViewer->setColorValid(true);
        if ( !_infoViewer->colorVisible() ) {
            _infoViewer->showColorInfo();
        }
        _infoViewer->setColor(r, g, b, a);
    } else {
        _infoViewer->setColorValid(false);
    }

    // The B info row (compare mode): pixel color under the cursor sampled
    // from the B slot's committed texture, like the 2D viewer's infoWidget[1].
    if (_infoViewerB && _infoViewerB->isVisible()) {
        if ( !_infoViewerB->mouseVisible() ) {
            _infoViewerB->showMouseInfo();
        }
        _infoViewerB->setMousePos( QPoint(widgetX, widgetY) );
        bool approxB = false;
        if ( pickColorAtWidgetPosSlot(1, widgetX, widgetY, &r, &g, &b, &a, &approxB) ) {
            _infoViewerB->setColorApproximated(approxB);
            _infoViewerB->setColorValid(true);
            if ( !_infoViewerB->colorVisible() ) {
                _infoViewerB->showColorInfo();
            }
            _infoViewerB->setColor(r, g, b, a);
        } else {
            _infoViewerB->setColorValid(false);
        }
    }
}

NATRON_NAMESPACE_EXIT
