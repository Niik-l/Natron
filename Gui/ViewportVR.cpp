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
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"

NATRON_NAMESPACE_ENTER

// ============================================================================
// Section 1: constants
// ============================================================================

static const double kVRPitchClampDeg = 87.5;

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
    , _roisEnabled(true)
    , _compositingOp(eViewerCompositingOperatorNone)
    , _lut(eViewerColorSpaceLinear)
    , _rangeFirst(1)
    , _rangeLast(250)
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
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    std::memset(_slots, 0, sizeof(_slots));
    for (int i = 0; i < 2; ++i) {
        _slots[i].isVisible = false;
        _slots[i].time = -1;
        _slots[i].mipmapLevel = 0;
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
ViewportVR::clearVRTextures()
{
    QMutexLocker k(&_slotsMutex);
    for (int i = 0; i < 2; ++i) {
        _slots[i].isVisible = false;
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
        "    vec2 uvCropA = u_uvA0 + uv * (u_uvA1 - u_uvA0);\n"
        "    vec4 a = texture2D(u_texA, uvCropA);\n"
        "    vec3 col = a.rgb;\n"
        "    if (u_haveB > 0.5) {\n"
        "        vec2 uvCropB = u_uvB0 + uv * (u_uvB1 - u_uvB0);\n"
        "        vec4 b = texture2D(u_texB, uvCropB);\n"
        "        if (u_op == 1 || u_op == 6) {\n" // wipe-under / stack-under
        "            col = b.rgb;\n"
        "        } else if (u_op == 3 || u_op == 8) {\n" // wipe-minus / stack-minus
        "            col = abs(a.rgb - b.rgb);\n"
        "        } else if (u_op == 4 || u_op == 9) {\n" // onion skin
        "            col = abs(a.rgb - b.rgb);\n"
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
    float uva0[2], uva1[2], uvb0[2], uvb1[2];
    const float wa0 = (float)winA.x1, wa1 = (float)winA.x2;
    const float wa0y = (float)winA.y1, wa1y = (float)winA.y2;
    uva0[0] = wa0 + uv0[0] * (wa1 - wa0);
    uva1[0] = wa0 + uv1[0] * (wa1 - wa0);
    uva0[1] = wa0y + uv0[1] * (wa1y - wa0y);
    uva1[1] = wa0y + uv1[1] * (wa1y - wa0y);
    const float wb0 = (float)winB.x1, wb1 = (float)winB.x2;
    const float wb0y = (float)winB.y1, wb1y = (float)winB.y2;
    uvb0[0] = wb0 + uv0[0] * (wb1 - wb0);
    uvb1[0] = wb0 + uv1[0] * (wb1 - wb0);
    uvb0[1] = wb0y + uv0[1] * (wb1y - wb0y);
    uvb1[1] = wb0y + uv1[1] * (wb1y - wb0y);

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
        // wipe variants collapse to the same behavior in VR: under shows B.
        switch (_compositingOp) {
        case eViewerCompositingOperatorWipeUnder:
        case eViewerCompositingOperatorStackUnder:
            op = 1;
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
        case eViewerCompositingOperatorWipeOver:
        case eViewerCompositingOperatorStackOver:
        default:
            op = 0;
            break;
        }
    }
    glUniform1i(_uOp, op);

    glUniform2f(_uSize, (GLfloat)width(), (GLfloat)height());
    glUniform1f(_uTanHalfFov, (GLfloat)std::tan(_fovDeg * 0.5 * M_PI / 180.));
    glUniform1f(_uAspect, (GLfloat)width() / (GLfloat)height());
    glUniform1f(_uYaw, (GLfloat)(_yawDeg * M_PI / 180.));
    glUniform1f(_uPitch, (GLfloat)(_pitchDeg * M_PI / 180.));
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
    // The VR view has no zoom: it always shows everything the engines sends,
    // and mip levels are picked from the ROI size.
    return 1;
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
    // Without ROI mode the whole (2:1) pano is rendered; with ROI mode, only
    // the horizontal slice visible through the current FOV (full vertical):
    // that is what makes scrubbing cheap like the 2D viewer's ROI.
    bool roiEnabled;
    {
        QMutexLocker l(&_roimutex);
        roiEnabled = _roisEnabled;
    }
    if (!roiEnabled) {
        return imageRoDPixel;
    }

    double yawDeg;
    double fovDeg;
    double pitchDeg;
    getLook(&yawDeg, &pitchDeg, &fovDeg);
    const double hfovRad = 2. * std::atan( std::tan(fovDeg * 0.5 * M_PI / 180.) * ( (double)width() / (double)height() ) );

    double yawCenter = std::fmod(yawDeg * M_PI / 180., 2. * M_PI);
    if (yawCenter < -M_PI) {
        yawCenter += 2. * M_PI;
    } else if (yawCenter > M_PI) {
        yawCenter -= 2. * M_PI;
    }

    double lonLo = yawCenter - hfovRad * 0.5;
    double lonHi = yawCenter + hfovRad * 0.5;
    if ( (lonLo < -M_PI) || (lonHi > M_PI) ) {
        // Window crosses the vertical wrap: rendering the full width is the
        // only valid single-rectangle answer.
        return imageRoDPixel;
    }

    // lon -> pano u in [0,1] (u = 0.5 + lon/2Pi); u -> pixel x on the rod.
    RectI roi;
    const double u0 = 0.5 + lonLo / (2. * M_PI);
    const double u1 = 0.5 + lonHi / (2. * M_PI);
    roi.x1 = (int)std::floor( imageRoDPixel.x1 + u0 * (imageRoDPixel.x2 - imageRoDPixel.x1) );
    roi.x2 = (int)std::ceil(  imageRoDPixel.x1 + u1 * (imageRoDPixel.x2 - imageRoDPixel.x1) );
    roi.y1 = imageRoDPixel.y1;
    roi.y2 = imageRoDPixel.y2;
    // The view is level-invariant vertically (the pitch comes from the same
    // full-height texture): rendering the full vertical is both correct and
    // keeps the pano reusable between pitch changes.
    Q_UNUSED(par);
    Q_UNUSED(mipmapLevel);
    roi = roi.intersect(imageRoDPixel);
    return roi;
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

    int tileSizeLocal = ipow( 2, appPTR->getCurrentSettings()->getViewerTilesPowerOf2() );
    RectI texRect;
    texRect.x1 = (int)std::floor( ( (double)roi.x1 ) / tileSizeLocal ) * (double)tileSizeLocal;
    texRect.y1 = (int)std::floor( ( (double)roi.y1 ) / tileSizeLocal ) * (double)tileSizeLocal;
    texRect.x2 = (int)std::ceil( ( (double)roi.x2 ) / tileSizeLocal ) * (double)tileSizeLocal;
    texRect.y2 = (int)std::ceil( ( (double)roi.y2 ) / tileSizeLocal ) * (double)tileSizeLocal;

    if (roiNotRounded) {
        *roiNotRounded = roi;
    }
    if (tilesRounded) {
        // VR renders the ROI as a single tile: one transfer, no tile bookkeeping.
        tilesRounded->push_back(texRect);
        if (tiles) {
            tiles->push_back(texRect.intersect(bounds));
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
    QMutexLocker k(&_roimutex);
    return _roisEnabled;
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
    Q_UNUSED(bytesCount);
    assert(textureIndex == 0 || textureIndex == 1);
    assert(ramBuffer);
    // Engine calls this on the main thread (see the asserts in ViewerGL's twin).
    if ( QOpenGLContext::currentContext() != context() ) {
        makeCurrent();
    }

    TexturePtr tex;
    TextureRect textureRectangle;
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        // The texture spans the WHOLE rounded ROI of this viewer (ViewerGL
        // pattern); the buffer handed over here belongs to ONE tile, uploaded
        // into its own sub-rectangle below. Formatting the big rect from the
        // passed rounded-ROI guarantees a matching byte stride.
        textureRectangle.par = tileRect.par;
        textureRectangle.closestPo2 = tileRect.closestPo2;
        textureRectangle.set(roiRoundedToTileSize);
        tex = slot.texture;
        if (!tex || (tex->type() != Texture::eDataTypeByte)) {
            int format, internalFormat, glType;
            Texture::getRecommendedTexParametersForRGBAByteTexture(&format, &internalFormat, &glType);
            tex.reset( new Texture(GL_TEXTURE_2D, GL_LINEAR, GL_NEAREST, GL_CLAMP_TO_EDGE,
                                   Texture::eDataTypeByte, format, internalFormat, glType) );
            slot.texture = tex;
        }
        slot.roiRounded = roiRoundedToTileSize;
        slot.roi = roi;
    }

    if (!isPartialRect && tex) {
        if (isFirstTile) {
            tex->ensureTextureHasSize(textureRectangle, 0);
        }
        // The call's "roi" is the TILE's own rectangle: its bytes are uploaded
        // into the tile-sized sub-rect of the big texture (updateOnlyRoi=true),
        // exactly like ViewerGL's twin — the byte stride then matches the
        // engine's per-tile buffer.
        tex->fillOrAllocateTexture(textureRectangle, tileRect, true, ramBuffer);
    }
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
    {
        QMutexLocker k(&_slotsMutex);
        VRSlot& slot = _slots[textureIndex];
        slot.texture = texture;
        slot.isVisible = true;
        slot.time = time;
        slot.rod = rod;
        slot.par = par;
        slot.mipmapLevel = mipmapLevel;

        // Map the rendered pixel ROI back to pano UV space so the sampler can
        // address the little window inside the full panorama.
        const RectI rodPx = rod.toPixelEnclosing(mipmapLevel, par);
        const RectI& r = slot.roi;
        if ( (rodPx.width() > 0) && (rodPx.height() > 0) ) {
            slot.winUV.x1 = (double)(r.x1 - rodPx.x1) / (double)rodPx.width();
            slot.winUV.x2 = (double)(r.x2 - rodPx.x1) / (double)rodPx.width();
            // GL texture layout: row 0 = image BOTTOM; Natron rows grow with y.
            slot.winUV.y1 = (double)(r.y1 - rodPx.y1) / (double)rodPx.height();
            slot.winUV.y2 = (double)(r.y2 - rodPx.y1) / (double)rodPx.height();
        } else {
            slot.winUV = RectD(0, 0, 1, 1);
        }
    }
    update();
}

void
ViewportVR::disconnectInputTexture(int textureIndex, bool clearRoD)
{
    Q_UNUSED(clearRoD);
    QMutexLocker k(&_slotsMutex);
    if ( (textureIndex >= 0) && (textureIndex < 2) ) {
        _slots[textureIndex].isVisible = false;
    }
    update();
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

NATRON_NAMESPACE_EXIT
