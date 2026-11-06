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
#include "Global/Enums.h"

#include "Engine/OpenGLViewerI.h"
#include "Engine/Texture.h"
#include "Engine/EngineFwd.h"

#include "Global/QtCompat.h"
#include <QOpenGLWidget>
#include <QMutex>
#include <QString>

#include "Gui/GuiFwd.h"

class QString;
class TimeLineGui;

NATRON_NAMESPACE_ENTER

class ViewerInstance;

/**
 * @brief The dedicated VR / 360 viewport: the OpenGL canvas of the "Viewer VR"
 * node's pane.
 *
 * This is a real ViewerGL counterpart: it implements OpenGLViewerI, so the
 * VT15 ViewerInstance render engine renders INTO it — the image cache, the
 * async renders, the color pipeline (channels / gain / gamma / colorspace-LUT
 * or CPU-baked OCIO) are all the same machinery the 2D viewer uses; the pane
 * samples the finished display-ready texture equirectangularly (spherical→
 * planar is simple GLSL, VR_PLAN.md), with mouse-drag look-around and
 * wheel-FOV. An optional ROI mode renders only the horizontal window visible
 * through the current FOV (turned on by the pane's ROI button) which makes
 * scrubbing as fast as the 2D viewer.
 *
 * The A/B textures come through the same two texture indices the 2D viewer
 * uses; the pane's comparison operator is applied at sample time.
 */
class ViewportVR
    : public QOpenGLWidget
    , public OpenGLViewerI
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

    // ---- attached preview (VR Viewer node plumbing, moved to the tab) ------

    QSize sizeHint() const OVERRIDE;

    // ---- look-around & VR options ------------------------------------------

    void setLook(double yawDeg, double pitchDeg, double fovDeg);
    void getLook(double* yawDeg, double* pitchDeg, double* fovDeg) const;
    void resetLook();

    void setLayoutMode(Layout l);
    Layout getLayoutMode() const { return _layout; }
    void setEye(int eye);

    /** ROI mode: only render the slice of the pano visible through the current
     *  FOV (much faster). Same knob as the 2D viewer's user-ROI button. */
    void setUserRoIEnabled(bool on);
    bool isUserRegionOfInterestEnabled_VR() const { QMutexLocker k(&_roimutex); return _roisEnabled; }

    void setCompositingOperator( ViewerCompositingOperatorEnum o) OVERRIDE FINAL;
    ViewerCompositingOperatorEnum getCompositingOperator() const OVERRIDE FINAL { return _compositingOp; }

    /** The pane feeds the getViewerFrameRange contract (playback bounds). */
    void setVRPlaybackRange(int first, int last);
    void clearVRTextures();

public Q_SLOTS:

    void requestRedrawVR();

Q_SIGNALS:

    /** Emitted when the look (yaw/pitch) changes: the pane uses it to schedule
     *  a cheap engine re-render (fills any tile a previously aborted render
     *  pass left out — with full-pano rendering the look does not change the
     *  render, so this is the only trigger that re-completes missing tiles). */
    void lookChanged();

    /** Emitted whenever the (clamped) FOV changes — the pane's spin box
     *  mirrors it so wheel-driven zoom reflects in the toolbar. */
    void fovChanged(double fovDegrees);

public:

    /*******************************************/
    /***OVERRIDDEN FROM OverlaySupport (GL glue) ***/
    /*******************************************/

    void swapOpenGLBuffers() OVERRIDE FINAL;
    void redraw() OVERRIDE FINAL;
    void getViewportSize(double &width, double &height) const OVERRIDE FINAL;
    void getPixelScale(double & xScale, double & yScale) const OVERRIDE FINAL;
    double getScreenPixelRatio() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    void getBackgroundColour(double &r, double &g, double &b) const OVERRIDE FINAL;
    virtual RectD getViewportRect() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    void getCursorPosition(double& x, double& y) const OVERRIDE FINAL;

    /*******************************************/
    /******OVERRIDDEN FROM OpenGLViewerI ******/
    /*******************************************/

    bool isViewerUIVisible() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    double getZoomFactor() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    void fitImageToFormat() OVERRIDE FINAL;
    bool isClippingImageToFormat() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    RectI getImageRectangleDisplayed(const RectI & imageRoDPixel, const double par, unsigned int mipmapLevel) OVERRIDE FINAL;
    RectI getImageRectangleDisplayedRoundedToTileSize(int texIndex,
                                                      const RectD & rod,
                                                      const double par,
                                                      unsigned int mipmapLevel,
                                                      std::vector<RectI>* tiles,
                                                      std::vector<RectI>* tilesRounded,
                                                      int *tileSize,
                                                      RectI* roiNotRounded) OVERRIDE FINAL;
    RectI getExactImageRectangleDisplayed(int texIndex, const RectD & rod, const double par,
                                          unsigned int mipmapLevel) OVERRIDE FINAL;
    ImageBitDepthEnum getBitDepth() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    bool isUserRegionOfInterestEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    RectD getUserRegionOfInterest() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    void clearPartialUpdateTextures() OVERRIDE FINAL;
    void transferBufferFromRAMtoGPU(const unsigned char* ramBuffer,
                                    size_t bytesCount,
                                    const RectI &roiRoundedToTileSize,
                                    const RectI& roi,
                                    const TextureRect & tileRect,
                                    int textureIndex,
                                    bool isPartialRect,
                                    bool isFirstTile,
                                    TexturePtr* texture) OVERRIDE FINAL;
    void endTransferBufferFromRAMToGPU(int textureIndex,
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
                                       bool isPartialRect) OVERRIDE FINAL;

    void disconnectInputTexture(int textureIndex, bool clearRoD) OVERRIDE FINAL;
    void updateColorPicker(int textureIndex, int x = std::numeric_limits<int>::max(), int y = std::numeric_limits<int>::max()) OVERRIDE FINAL;
    void getTextureColorAt(int x, int y, double* r, double *g, double *b, double *a) OVERRIDE FINAL;
    void makeOpenGLcontextCurrent() OVERRIDE FINAL;
    void removeGUI() OVERRIDE FINAL;
    ViewIdx getCurrentView() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    int getCurrentlyDisplayedTime() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    void getViewerFrameRange(int* first, int* last) const OVERRIDE FINAL;
    /** Must return a pointer to the current timeline used by the Viewer */
    virtual TimeLinePtr getTimeline() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    void saveOpenGLContext() OVERRIDE FINAL;
    void restoreOpenGLContext() OVERRIDE FINAL;
    void clearLastRenderedImage() OVERRIDE FINAL;
    void redrawNow() OVERRIDE FINAL;
    void toWidgetCoordinates(double *x, double *y) const OVERRIDE FINAL;
    void toCanonicalCoordinates(double *x, double *y) const OVERRIDE FINAL;
    int getWidgetFontHeight() const OVERRIDE FINAL;
    int getStringWidthForCurrentFont(const std::string& string) const OVERRIDE FINAL;
    /*******************************************/

protected:

    virtual void initializeGL() OVERRIDE;
    virtual void paintGL() OVERRIDE;
    virtual void resizeGL(int width, int height) OVERRIDE;

    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE;

private:

    /** Bind the A/B slot textures + the op and draw the fullscreen quad. */
    void drawVRImage();

    Gui* _gui;

    // view state
    double _yawDeg;   //!< 0 = looking at the center of the pano; + = turn east (right)
    double _pitchDeg; //!< 0 = level; + = look up (north pole), clamped to +-87.5
    double _fovDeg;   //!< vertical field of view in degrees
    Layout _layout;
    int _eye;

    // drag state
    int _lastMouseX, _lastMouseY;
    bool _dragging;
    double _lastEmittedYaw, _lastEmittedPitch;
    bool _lookEmittedOnce;

    // last committed image height in pixels (drives the render mip choice)
    int _lastRodH;

    // ROI render mode (user face of the interface's isUserRegionOfInterestEnabled)
    mutable QMutex _roimutex;
    bool _roisEnabled;

    // op applied in shader between the A and B textures
    ViewerCompositingOperatorEnum _compositingOp;

    /** Colorspace state tracked for the interface contract (the CPU pipeline
     *  bakes it, we only report). */
    ViewerColorSpaceEnum _lut;

    // playback bounds reported to the engine through getViewerFrameRange
    int _rangeFirst, _rangeLast;

    // engine-side comparison op requested by the pane
    ViewerCompositingOperatorEnum _requestedOp; // unused currently, kept for 5b A/B parity

    // per-texture-index slots (A and B). Staging is CPU-side; the engine calls
    // transferBufferFromRAMtoGPU per tile (pure memcpy now) and
    // endTransferBufferFromRAMToGPU exactly ONCE per frame per index — that is
    // the atomic commit point: the frame goes to the GL texture (through the
    // PBO pool) only when the staged tiles cover the whole render ROI; an
    // incomplete set is dropped and the previous frame stays on screen (no
    // mixed-frame artifacts, and aborted passes are simply re-rendered). The
    // GL-swap state machine mirrors ViewerGL's displayTextures model.
    struct VRSlot
    {
        TexturePtr texture;          //!< committed GL texture
        RectI roi;                   //!< committed display rect (pixels)
        RectD rod;                   //!< committed rod
        RectD winUV;                 //!< committed window mapped into pano UV
        double par;
        bool isVisible;
        int time;                    //!< the committed frame

        // staging (only valid between a transfer frame's first tile and its
        // matching endTransfer call)
        std::vector<unsigned char> staged;
        RectI stagedRect;            //!< the staged texture rect (full rounded ROI)
        std::vector<RectI> stagedTiles;
        unsigned long long stagedArea;
        bool stagedValid;
    };
    VRSlot _slots[2];
    mutable QMutex _slotsMutex;

    /** Commit the staged frame of one slot (GL upload through the PBO pool). */
    void commitStagedSlot(int textureIndex);

    // double-buffered PBO pool for async uploads (ViewerGL's scheme)
    std::vector<GLuint> _pboIds;
    int _pboIndex;

    // shader program (per context — rebuilt whenever the GL context is recreated)
    GLuint _program;
    bool _programReady;
    GLint _uTexA, _uTexB, _uSize, _uTanHalfFov, _uAspect, _uYaw, _uPitch, _uUvA0, _uUvA1, _uUvB0, _uUvB1, _uOp, _uHaveB;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_VIEWPORTVR_H
