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
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "OSGLContext_win.h"

#include <cstdio>

#ifdef __NATRON_WIN32__
#include <iostream> // std::cerr
#include <sstream> // stringstream

#include <QtGlobal>      // for Q_UNUSED
#include <QCoreApplication>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <cstdlib>       // std::getenv
#include <map>

#include "Engine/AppManager.h"
#include "Engine/MemoryInfo.h" // isApplication32Bits
#include "Engine/OSGLContext.h"

#ifdef NATRON_USE_OPTIMUS_HPG

// Applications exporting this symbol with this value will be automatically
// directed to the high-performance GPU on Nvidia Optimus systems with
// up-to-date drivers
//
__declspec(dllexport) DWORD NvOptimusEnablement = 1;

// Applications exporting this symbol with this value will be automatically
// directed to the high-performance GPU on AMD PowerXpress systems with
// up-to-date drivers
//
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#endif // NATRON_USE_OPTIMUS_HPG


#define NATRON_WNDCLASSNAME L"NATRON30"


NATRON_NAMESPACE_ENTER

namespace {

// Which thread last made each rendering context current, kept so that a make-current failure can
// say who holds the context. Set NATRON_DEBUG_GLCTX=1 to trace every make-current on stderr.
static QMutex g_currentHoldersMutex;
static std::map<HGLRC, DWORD> g_currentHolders;

static bool glCtxTraceEnabled()
{
    static const bool on = (std::getenv("NATRON_DEBUG_GLCTX") != NULL);
    return on;
}

static void noteCurrent(HGLRC context)
{
    const DWORD tid = GetCurrentThreadId();
    QMutexLocker l(&g_currentHoldersMutex);
    for (std::map<HGLRC, DWORD>::iterator it = g_currentHolders.begin(); it != g_currentHolders.end();) {
        if (it->second == tid) {
            g_currentHolders.erase(it++);
        } else {
            ++it;
        }
    }
    if (context) {
        g_currentHolders[context] = tid;
    }
}

// DC -> the hidden window it was taken from (0 for an NV affinity DC), for the failure report.
static std::map<HDC, HWND> g_dcWindows;

static void describeDC(HDC dc, char* buf, size_t n)
{
    HWND stored = 0;
    {
        QMutexLocker l(&g_currentHoldersMutex);
        std::map<HDC, HWND>::const_iterator it = g_dcWindows.find(dc);
        if (it != g_dcWindows.end()) {
            stored = it->second;
        }
    }
    POINT vo = {0, 0}, wo = {0, 0};
    SIZE ve = {0, 0}, we = {0, 0};
    GetViewportOrgEx(dc, &vo);
    GetWindowOrgEx(dc, &wo);
    GetViewportExtEx(dc, &ve);
    GetWindowExtEx(dc, &we);
    HDC again = stored ? GetDC(stored) : 0;
    std::snprintf(buf, n,
                  "GetObjectType(dc)=%lu (3=DC, 0=dead) WindowFromDC=%p storedWindow=%p %s GetDC(storedWindow)=%p mapMode=%d gfxMode=%d "
                  "viewportOrg=(%ld,%ld) windowOrg=(%ld,%ld) viewportExt=(%ld,%ld) windowExt=(%ld,%ld) pixelFormat=%d",
                  (unsigned long)GetObjectType(dc), (void*)WindowFromDC(dc), (void*)stored,
                  stored ? (IsWindow(stored) ? "alive" : "DESTROYED") : "(affinity DC, no window)", (void*)again,
                  GetMapMode(dc), GetGraphicsMode(dc), vo.x, vo.y, wo.x, wo.y, ve.cx, ve.cy, we.cx, we.cy, GetPixelFormat(dc));
    if (again && again != dc) {
        ReleaseDC(stored, again);
    }
}

static DWORD holderOf(HGLRC context)
{
    QMutexLocker l(&g_currentHoldersMutex);
    std::map<HGLRC, DWORD>::const_iterator it = g_currentHolders.find(context);
    return it == g_currentHolders.end() ? 0 : it->second;
}

bool makeDCAndContextCurrent(HDC dc, HGLRC context)
{
    const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();

    assert(wglInfo);
    bool ok = wglInfo->MakeCurrent(dc, context);
    if (ok) {
        if (glCtxTraceEnabled()) {
            std::fprintf(stderr, "[GLCTX] tid %lu: %s rc=%p\n", (unsigned long)GetCurrentThreadId(),
                         context ? "made current" : "released", (void*)context);
        }
        noteCurrent(context);
        return ok;
    }
    if (!context) {
        if (glCtxTraceEnabled()) {
            std::fprintf(stderr, "[GLCTX] tid %lu: wglMakeCurrent(NULL) failed: Win32 error %lu\n",
                         (unsigned long)GetCurrentThreadId(), (unsigned long)GetLastError());
        }
        return ok;
    }

    // Safety net for transient refusals. NVIDIA's threaded driver ("Threaded optimization", on by
    // default) is known to refuse wglMakeCurrent now and then when several threads switch
    // contexts, with Win32 error 2004 (ERROR_TRANSFORM_NOT_SUPPORTED) or 6, and succeed a few
    // milliseconds later (community.khronos.org thread 61048: NVIDIA acknowledged a driver bug,
    // the confirmed workaround is a retry loop with Sleep(10)). ERROR_BUSY (170) means another
    // thread still holds the context and is about to release it. A failure that survives every
    // retry is reported in full below; a destroyed DC (see GLWindowOwner) looked exactly like
    // that: error 2004 at first, then 6 for good.
    const unsigned long firstError = (unsigned long)GetLastError();
    int retries = 0;
    for (; retries < 20 && !ok; ++retries) {
        Sleep(10);
        ok = wglInfo->MakeCurrent(dc, context);
    }
    if (ok) {
        std::fprintf(stderr, "[OSGLContext_win] wglMakeCurrent(rc=%p) succeeded after %d retr%s (first Win32 error %lu)\n",
                     (void*)context, retries, retries == 1 ? "y" : "ies", firstError);
        noteCurrent(context);
    } else {
        // Printed because callers only see a bool.
        const unsigned long lastError = (unsigned long)GetLastError();
        char dcInfo[512];
        describeDC(dc, dcInfo, sizeof(dcInfo));
        std::fprintf(stderr, "[OSGLContext_win] wglMakeCurrent(dc=%p, rc=%p) failed after %d retries: Win32 error %lu (first %lu); "
                             "this tid %lu, rc last made current by tid %lu, this thread's current rc=%p; %s\n",
                     (void*)dc, (void*)context, retries, lastError, firstError,
                     (unsigned long)GetCurrentThreadId(), (unsigned long)holderOf(context),
                     (void*)wglInfo->GetCurrentContext(), dcInfo);
    }
    std::fflush(stderr);
    return ok;
}

}  // namespace

bool
OSGLContext_win::extensionSupported(const char* extension,
                                    OSGLContext_wgl_data* data)
{
    const char* extensions;

    if (data->GetExtensionsStringEXT) {
        extensions = data->GetExtensionsStringEXT();
        if (extensions) {
            if ( OSGLContext::stringInExtensionString(extension, extensions) ) {
                return true;
            }
        }
    }

    if (data->GetExtensionsStringARB) {
        extensions = data->GetExtensionsStringARB(_dc);
        if (extensions) {
            if ( OSGLContext::stringInExtensionString(extension, extensions) ) {
                return true;
            }
        }
    }

    return false;
}

void
OSGLContext_win::initWGLData(OSGLContext_wgl_data* wglInfo)
{
    memset( wglInfo, 0, sizeof(OSGLContext_wgl_data) );
    wglInfo->instance = LoadLibraryW(L"opengl32.dll");
    if (!wglInfo->instance) {
        throw std::runtime_error("WGL: Failed to load opengl32.dll");
    }

    wglInfo->CreateContext = (WGLCREATECONTEXT_T)GetProcAddress(wglInfo->instance, "wglCreateContext");
    wglInfo->DeleteContext = (WGLDELETECONTEXT_T)GetProcAddress(wglInfo->instance, "wglDeleteContext");
    wglInfo->GetCurrentContext = (WGLGETCURRENTCONTEXT_T)GetProcAddress(wglInfo->instance, "wglGetCurrentContext");
    wglInfo->GetCurrentDC = (WGLGETCURRENTDC_T)GetProcAddress(wglInfo->instance, "wglGetCurrentDC");
    wglInfo->GetProcAddress = (WGLGETPROCADDRESS_T)GetProcAddress(wglInfo->instance, "wglGetProcAddress");
    wglInfo->MakeCurrent = (WGLMAKECURRENT_T)GetProcAddress(wglInfo->instance, "wglMakeCurrent");
    wglInfo->ShareLists = (WGLSHARELISTS_T)GetProcAddress(wglInfo->instance, "wglShareLists");

    WNDCLASSEXW wc;

    ZeroMemory( &wc, sizeof(wc) );
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = (WNDPROC) DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = 0;//LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = NATRON_WNDCLASSNAME;
    bool ok = (bool)RegisterClassExW(&wc);
    assert(ok);
    Q_UNUSED(ok);
}

bool
OSGLContext_win::loadWGLExtensions(OSGLContext_wgl_data* wglInfo)
{
    // This should be protected by a mutex externally
    if (wglInfo->extensionsLoaded) {
        return false;
    }
    // Functions for WGL_EXT_extension_string
    // NOTE: These are needed by extensionSupported
    wglInfo->GetExtensionsStringEXT = (PFNWGLGETEXTENSIONSSTRINGEXTPROC)wglInfo->GetProcAddress("wglGetExtensionsStringEXT");
    wglInfo->GetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglInfo->GetProcAddress("wglGetExtensionsStringARB");

    // Functions for WGL_ARB_create_context
    wglInfo->CreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglInfo->GetProcAddress("wglCreateContextAttribsARB");

    // Functions for WGL_EXT_swap_control
    wglInfo->SwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglInfo->GetProcAddress("wglSwapIntervalEXT");

    // Functions for WGL_ARB_pixel_format
    wglInfo->GetPixelFormatAttribivARB = (PFNWGLGETPIXELFORMATATTRIBIVARBPROC)wglInfo->GetProcAddress("wglGetPixelFormatAttribivARB");

    // This needs to include every extension used below except for
    // WGL_ARB_extensions_string and WGL_EXT_extensions_string
    wglInfo->ARB_multisample = extensionSupported("WGL_ARB_multisample", wglInfo);
    wglInfo->ARB_framebuffer_sRGB = extensionSupported("WGL_ARB_framebuffer_sRGB", wglInfo);
    wglInfo->EXT_framebuffer_sRGB = extensionSupported("WGL_EXT_framebuffer_sRGB", wglInfo);
    wglInfo->ARB_create_context = extensionSupported("WGL_ARB_create_context", wglInfo);
    wglInfo->ARB_create_context_profile = extensionSupported("WGL_ARB_create_context_profile", wglInfo);
    wglInfo->EXT_create_context_es2_profile = extensionSupported("WGL_EXT_create_context_es2_profile", wglInfo);
    wglInfo->ARB_create_context_robustness = extensionSupported("WGL_ARB_create_context_robustness", wglInfo);
    wglInfo->EXT_swap_control = extensionSupported("WGL_EXT_swap_control", wglInfo);
    wglInfo->ARB_pixel_format = extensionSupported("WGL_ARB_pixel_format", wglInfo);
    wglInfo->ARB_context_flush_control = extensionSupported("WGL_ARB_context_flush_control", wglInfo);
    wglInfo->NV_gpu_affinity = extensionSupported("WGL_NV_gpu_affinity", wglInfo);
    wglInfo->AMD_gpu_association = extensionSupported("WGL_AMD_gpu_association", wglInfo);

    if (wglInfo->NV_gpu_affinity) {
        wglInfo->EnumGpusNV = (PFNWGLENUMGPUSNV)wglInfo->GetProcAddress("wglEnumGpusNV");
        wglInfo->EnumGpuDevicesNV = (PFNWGLENUMGPUDEVICESNV)wglInfo->GetProcAddress("wglEnumGpuDevicesNV");
        wglInfo->CreateAffinityDCNV = (PFNWGLCREATEAFFINITYDCNV)wglInfo->GetProcAddress("wglCreateAffinityDCNV");
        wglInfo->EnumGpusFromAffinityDCNV = (PFNWGLENUMGPUSFROMAFFINITYDCNV)wglInfo->GetProcAddress("wglEnumGpusFromAffinityDCNV");
        wglInfo->DeleteDCNV = (PFNWGLDELETEDCNV)wglInfo->GetProcAddress("wglDeleteDCNV");
    }

    if (wglInfo->AMD_gpu_association) {
        wglInfo->GetGpuIDAMD = (PFNWGLGETGPUIDSAMD)wglInfo->GetProcAddress("wglGetGPUIDsAMD");
        wglInfo->GetGPUInfoAMD = (PFNWGLGETGPUINFOAMD)wglInfo->GetProcAddress("wglGetGPUInfoAMD");
        wglInfo->GetContextGpuIDAMD = (PFNWGLGETCONTEXTGPUIDAMD)wglInfo->GetProcAddress("wglGetContextGPUIDAMD");
        wglInfo->CreateAssociatedContextAMD = (PFNWGLCREATEASSOCIATEDCONTEXTAMD)wglInfo->GetProcAddress("wglCreateAssociatedContextAMD");
        wglInfo->CreateAssociatedContextAttribsAMD = (PFNWGLCREATEASSOCIATEDCONTEXTATTRIBSAMD)wglInfo->GetProcAddress("wglCreateAssociatedContextAttribsAMD");
        wglInfo->DeleteAssociatedContextAMD = (PFNWGLDELETEASSOCIATEDCONTEXTAMD)wglInfo->GetProcAddress("wglDeleteAssociatedContextAMD");
        wglInfo->MakeAssociatedContetCurrentAMD = (PFNWGLMAKEASSOCIATEDCONTEXTCURRENTAMD)wglInfo->GetProcAddress("wglMakeAssociatedContextCurrentAMD");
        wglInfo->GetCurrentAssociatedContextAMD = (PFNWGLGETCURRENTASSOCIATEDCONTEXTAMD)wglInfo->GetProcAddress("wglGetCurrentAssociatedContextAMD");
        wglInfo->BlitContextFrameBufferAMD = (PFNWGLBLITCONTEXTFRAMEBUFFERAMD)wglInfo->GetProcAddress("wglBlitContextFramebufferAMD");
    }

    wglInfo->extensionsLoaded = GL_TRUE;

    return true;
} // OSGLContext_win::loadWGLExtensions

void
OSGLContext_win::destroyWGLData(OSGLContext_wgl_data* wglInfo)
{
    if (wglInfo->instance) {
        FreeLibrary(wglInfo->instance);
        wglInfo->instance = 0;
    }
    UnregisterClassW( NATRON_WNDCLASSNAME, GetModuleHandleW(NULL) );
}

// The hidden windows that give the rendering contexts their device contexts are created and
// destroyed on this one thread, which lives as long as the process.
//
// Win32 windows and common DCs are thread-affine: when the thread that created a window exits,
// the system destroys the window; a DC obtained with GetDC from a thread other than the window's
// creator is a common DC that is released when that thread exits (see createWindow). The context
// pool creates its contexts lazily on whichever render thread asks first, so the shared contexts
// used to belong to the render threads of the first Write that rendered; when that Write finished
// and its threads exited, every other render still using the contexts got wglMakeCurrent errors
// 2004 then ERROR_INVALID_HANDLE (GetObjectType(dc) == 0) and ended as a bare "Rendering Failed".
// Three ScanlineRender Writes started together in the GUI showed it every time the first one
// finished before the others. (Qt documents the same constraint for QOffscreenSurface.)
namespace {
class GLWindowOwner : public QObject
{
public:
    static GLWindowOwner* instance()
    {
        static QMutex mutex;
        static GLWindowOwner* owner = 0;
        QMutexLocker l(&mutex);
        if (!owner) {
            owner = new GLWindowOwner;
            QThread* thread = new QThread;   // never stopped: it must outlive every context
            thread->setObjectName( QString::fromUtf8("NatronGLWindowOwner") );
            owner->moveToThread(thread);
            thread->start();
            // start() returns before the thread runs; run() below must not fall back to the
            // calling thread because isRunning() is still false.
            while ( !thread->isRunning() ) {
                QThread::yieldCurrentThread();
            }
        }
        return owner;
    }

    // Runs fn on the owner thread and waits for it. Falls back to running it in place when there
    // is no event loop to go through (process teardown) or when already on the owner thread.
    template <typename F>
    void run(F fn)
    {
        if (QThread::currentThread() == thread() || !QCoreApplication::instance() || !thread()->isRunning()) {
            fn();
            return;
        }
        QMetaObject::invokeMethod(this, fn, Qt::BlockingQueuedConnection);
    }
};
} // namespace

// Creates the hidden 32x32 window the context's DC comes from, and takes that DC.
//
// Both happen on the owner thread. GetDC on a CS_OWNDC window returns the window's private DC
// only when called from the thread that created the window; from any other thread it returns a
// common DC from the system cache, and a common DC is released when the thread that obtained it
// exits (verified on Windows 11: GetObjectType(dc) == 0 and WindowFromDC(dc) == NULL afterwards).
// The old code took the DC on the render thread that created the context, so the shared contexts'
// DCs died with the first Write's render threads.
//
static bool
createWindow(HWND* window, HDC* dc)
{
    HWND created = 0;
    HDC privateDc = 0;
    DWORD creatorTid = 0;
    GLWindowOwner::instance()->run([&created, &privateDc, &creatorTid]() {
        creatorTid = GetCurrentThreadId();
        created = CreateWindowExW(/*exStyle*/ 0,
                                  NATRON_WNDCLASSNAME,
                                  L"",
                                  /*style*/ 0,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  32, 32,
                                  NULL, // No parent window
                                  NULL, // No window menu
                                  GetModuleHandleW(NULL),
                                  NULL);
        if (created) {
            privateDc = GetDC(created);
        }
    });
    *window = created;
    *dc = privateDc;
    if (glCtxTraceEnabled()) {
        std::fprintf(stderr, "[GLCTX] hidden window %p / private DC %p created on tid %lu (requested from tid %lu)\n",
                     (void*)created, (void*)privateDc, (unsigned long)creatorTid, (unsigned long)GetCurrentThreadId());
    }

    return created != 0 && privateDc != 0;
}

int
OSGLContext_win::getPixelFormatAttrib(const OSGLContext_wgl_data* wglInfo,
                                      int pixelFormat,
                                      int attrib)
{
    int value = 0;

    assert(wglInfo->ARB_pixel_format);

    if ( !wglInfo->GetPixelFormatAttribivARB(_dc, pixelFormat, 0, 1, &attrib, &value) ) {
        std::stringstream ss;
        ss << "WGL: Failed to retrieve pixel format attribute ";
        ss << attrib;
        throw std::runtime_error( ss.str() );
    }

    return value;
}

static void
setAttribs(int /*major*/,
           int /*minor*/,
           bool coreProfile,
           std::vector<int>& attribs)
{
    int mask = 0;

    // Note that we do NOT request a specific version. Sometimes a driver might report that its maximum compatibility
    //profile is 3.0 but we ask for 2.0, hence it will fail context creation whereas it should not. Instead we check
    //OpenGL version once context is created and check that we have at least 2.0
    /*
       if ( (major != 1) || (minor != 0) ) {
        attribs.push_back(WGL_CONTEXT_MAJOR_VERSION_ARB);
        attribs.push_back(major);
        attribs.push_back(WGL_CONTEXT_MINOR_VERSION_ARB);
        attribs.push_back(minor);
       }*/

    if (coreProfile) {
        mask |= WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
    } else {
        mask |= WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
    }
    if (mask) {
        attribs.push_back(WGL_CONTEXT_PROFILE_MASK_ARB);
        attribs.push_back(mask);
    }
    attribs.push_back(0);
    attribs.push_back(0);
}

void
OSGLContext_win::createGLContext(const FramebufferConfig& pixelFormatAttrs,
                                 int major,
                                 int minor,
                                 bool coreProfile,
                                 const GLRendererID &rendererID,
                                 const OSGLContext_win* shareContext)
{
    const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();

    assert(wglInfo);
    if (!wglInfo) {
        throw std::invalid_argument("No wgl info");
    }
    bool useNVGPUAffinity = rendererID.rendererHandle && wglInfo->NV_gpu_affinity;
    if (useNVGPUAffinity) {
        // Created on the owner thread as well, in case the driver ties it to the calling thread.
        HGPUNV GpuMask[2] = {(HGPUNV)rendererID.rendererHandle, NULL};
        HDC affinityDc = 0;
        GLWindowOwner::instance()->run([&affinityDc, &GpuMask, wglInfo]() {
            affinityDc = wglInfo->CreateAffinityDCNV(GpuMask);
        });
        _dc = affinityDc;
    } else {
        _dc = _windowDc;   // the private DC taken on the owner thread, never GetDC() from here
    }
    if (!_dc) {
        throw std::runtime_error("WGL: Failed to retrieve DC for window");
    }
    {
        QMutexLocker l(&g_currentHoldersMutex);
        g_dcWindows[_dc] = useNVGPUAffinity ? (HWND)0 : _windowHandle;
    }
    if (glCtxTraceEnabled()) {
        char dcInfo[512];
        describeDC(_dc, dcInfo, sizeof(dcInfo));
        std::fprintf(stderr, "[GLCTX] context DC %p created on tid %lu (%s): %s\n", (void*)_dc, (unsigned long)GetCurrentThreadId(),
                     useNVGPUAffinity ? "NV affinity DC" : "window DC", dcInfo);
    }


    std::vector<FramebufferConfig> usableConfigs;
    int nativeCount = 0, usableCount = 0;

    if (wglInfo->ARB_pixel_format) {
        nativeCount = getPixelFormatAttrib(wglInfo, 1, WGL_NUMBER_PIXEL_FORMATS_ARB);
    } else {
        nativeCount = DescribePixelFormat(_dc, 1, sizeof(PIXELFORMATDESCRIPTOR), NULL);
    }

    usableConfigs.resize(nativeCount);

    for (int i = 0; i < nativeCount; ++i) {
        const int n = i + 1;
        FramebufferConfig& u = usableConfigs[usableCount];

        if (wglInfo->ARB_pixel_format) {
            // Get pixel format attributes through "modern" extension

            if ( !getPixelFormatAttrib(wglInfo, n, WGL_SUPPORT_OPENGL_ARB) ||
                 !getPixelFormatAttrib(wglInfo, n, WGL_DRAW_TO_WINDOW_ARB) ) {
                continue;
            }

            if (getPixelFormatAttrib(wglInfo, n, WGL_PIXEL_TYPE_ARB) != WGL_TYPE_RGBA_ARB) {
                continue;
            }

            if (getPixelFormatAttrib(wglInfo, n, WGL_ACCELERATION_ARB) == WGL_NO_ACCELERATION_ARB) {
                continue;
            }

            u.redBits = getPixelFormatAttrib(wglInfo, n, WGL_RED_BITS_ARB);
            u.greenBits = getPixelFormatAttrib(wglInfo, n, WGL_GREEN_BITS_ARB);
            u.blueBits = getPixelFormatAttrib(wglInfo, n, WGL_BLUE_BITS_ARB);
            u.alphaBits = getPixelFormatAttrib(wglInfo, n, WGL_ALPHA_BITS_ARB);

            u.depthBits = getPixelFormatAttrib(wglInfo, n, WGL_DEPTH_BITS_ARB);
            u.stencilBits = getPixelFormatAttrib(wglInfo, n, WGL_STENCIL_BITS_ARB);

            u.accumRedBits = getPixelFormatAttrib(wglInfo, n, WGL_ACCUM_RED_BITS_ARB);
            u.accumGreenBits = getPixelFormatAttrib(wglInfo, n, WGL_ACCUM_GREEN_BITS_ARB);
            u.accumBlueBits = getPixelFormatAttrib(wglInfo, n, WGL_ACCUM_BLUE_BITS_ARB);
            u.accumAlphaBits = getPixelFormatAttrib(wglInfo, n, WGL_ACCUM_ALPHA_BITS_ARB);

            u.auxBuffers = getPixelFormatAttrib(wglInfo, n, WGL_AUX_BUFFERS_ARB);

            if ( getPixelFormatAttrib(wglInfo, n, WGL_STEREO_ARB) ) {
                u.stereo = GL_TRUE;
            }
            if ( getPixelFormatAttrib(wglInfo, n, WGL_DOUBLE_BUFFER_ARB) ) {
                u.doublebuffer = GL_TRUE;
            }

            if (wglInfo->ARB_multisample) {
                u.samples = getPixelFormatAttrib(wglInfo, n, WGL_SAMPLES_ARB);
            }

            if (wglInfo->ARB_framebuffer_sRGB ||  wglInfo->EXT_framebuffer_sRGB) {
                if ( getPixelFormatAttrib(wglInfo, n, WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB) ) {
                    u.sRGB = GL_TRUE;
                }
            }
        } else {
            PIXELFORMATDESCRIPTOR pfd;

            // Get pixel format attributes through legacy PFDs

            if ( !DescribePixelFormat(_dc, n, sizeof(PIXELFORMATDESCRIPTOR), &pfd) ) {
                continue;
            }

            if ( !(pfd.dwFlags & PFD_DRAW_TO_WINDOW) || !(pfd.dwFlags & PFD_SUPPORT_OPENGL) ) {
                continue;
            }

            if ( !(pfd.dwFlags & PFD_GENERIC_ACCELERATED) && (pfd.dwFlags & PFD_GENERIC_FORMAT) ) {
                continue;
            }

            if (pfd.iPixelType != PFD_TYPE_RGBA) {
                continue;
            }

            u.redBits = pfd.cRedBits;
            u.greenBits = pfd.cGreenBits;
            u.blueBits = pfd.cBlueBits;
            u.alphaBits = pfd.cAlphaBits;

            u.depthBits = pfd.cDepthBits;
            u.stencilBits = pfd.cStencilBits;

            u.accumRedBits = pfd.cAccumRedBits;
            u.accumGreenBits = pfd.cAccumGreenBits;
            u.accumBlueBits = pfd.cAccumBlueBits;
            u.accumAlphaBits = pfd.cAccumAlphaBits;

            u.auxBuffers = pfd.cAuxBuffers;

            if (pfd.dwFlags & PFD_STEREO) {
                u.stereo = GL_TRUE;
            }
            if (pfd.dwFlags & PFD_DOUBLEBUFFER) {
                u.doublebuffer = GL_TRUE;
            }
        }

        u.handle = n;
        ++usableCount;
    }

    if (!usableCount) {
        throw std::runtime_error("WGL: The driver does not appear to support OpenGL");
    }
    FramebufferConfig closestConfig = OSGLContext::chooseFBConfig(pixelFormatAttrs, usableConfigs, usableCount);
    int pixelFormat = closestConfig.handle;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC share = shareContext ? shareContext->_handle : 0;

    if ( !DescribePixelFormat(_dc, pixelFormat, sizeof(pfd), &pfd) ) {
        throw std::runtime_error("WGL: Failed to retrieve PFD for selected pixel format");
    }

    if ( !SetPixelFormat(_dc, pixelFormat, &pfd) ) {
        throw std::runtime_error("WGL: Failed to set selected pixel format");
    }


    if (useNVGPUAffinity) {
        _handle = wglInfo->CreateContext(_dc);
    } else if ( (rendererID.renderID > 0) && wglInfo->AMD_gpu_association ) {
        std::vector<int> attribs;
        setAttribs(major, minor, coreProfile, attribs);
        _handle = wglInfo->CreateAssociatedContextAttribsAMD( (UINT)rendererID.renderID, share, &attribs[0] );
    } else if (wglInfo->ARB_create_context) {
        std::vector<int> attribs;
        setAttribs(major, minor, coreProfile, attribs);
        _handle = wglInfo->CreateContextAttribsARB(_dc, share, &attribs[0]);
    } else {
        _handle = wglInfo->CreateContext(_dc);
    }

    if (!_handle) {
        throw std::runtime_error("WGL: Failed to create OpenGL context");
    }
} // createGLContext

#undef setWGLattrib


// Analyzes the specified context for possible recreation
//
bool
OSGLContext_win::analyzeContextWGL(const FramebufferConfig& pixelFormatAttrs,
                                   int major,
                                   int minor)
{
    bool required = false;
    OSGLContext_wgl_data* wglInfo = const_cast<OSGLContext_wgl_data*>( appPTR->getWGLData() );

    assert(wglInfo);

    makeContextCurrent(this);
    if ( !loadWGLExtensions(wglInfo) ) {
        makeContextCurrent(nullptr);
        return false;
    }


/*
    if (ctxconfig->forward)
    {
        if (!_glfw.wgl.ARB_create_context)
        {
            _glfwInputError(GLFW_VERSION_UNAVAILABLE,
                            "WGL: A forward compatible OpenGL context requested but WGL_ARB_create_context is unavailable");
            return _GLFW_RECREATION_IMPOSSIBLE;
        }

        required = GLFW_TRUE;
    }

    if (ctxconfig->profile)
    {
        if (!_glfw.wgl.ARB_create_context_profile)
        {
            _glfwInputError(GLFW_VERSION_UNAVAILABLE,
                            "WGL: OpenGL profile requested but WGL_ARB_create_context_profile is unavailable");
            return _GLFW_RECREATION_IMPOSSIBLE;
        }

        required = GLFW_TRUE;
    }

    if (ctxconfig->release)
    {
        if (_glfw.wgl.ARB_context_flush_control)
            required = GLFW_TRUE;
    }
 */


    if ( (major != 1) || (minor != 0) ) {
        if (wglInfo->ARB_create_context) {
            required = true;
        }
    }

    /*if (ctxconfig->debug)
       {
        if (_glfw.wgl.ARB_create_context)
            required = GLFW_TRUE;
       }*/

    if (pixelFormatAttrs.samples > 0) {
        // MSAA is not a hard constraint, so do nothing if it's not supported
        if (wglInfo->ARB_multisample && wglInfo->ARB_pixel_format) {
            required = true;
        }
    }

    if (pixelFormatAttrs.sRGB) {
        // sRGB is not a hard constraint, so do nothing if it's not supported
        if ( (wglInfo->ARB_framebuffer_sRGB ||
              wglInfo->EXT_framebuffer_sRGB) &&
             wglInfo->ARB_pixel_format ) {
            required = true;
        }
    }

    makeContextCurrent(nullptr);
    return required;
}

// Destroys the window and rendering context
//
void
OSGLContext_win::destroyWindow()
{
    if (_windowHandle) {
        // DestroyWindow only works from the thread that created the window.
        HWND w = _windowHandle;
        GLWindowOwner::instance()->run([w]() {
            DestroyWindow(w);
        });
        _windowHandle = 0;
        _windowDc = 0;   // a private DC goes with its window
    }
}

void
OSGLContext_win::destroyContext()
{
    if (_handle) {
        const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();
        assert(wglInfo);
        wglInfo->DeleteContext(_handle);
        _handle = 0;
    }
}

OSGLContext_win::OSGLContext_win(const FramebufferConfig& pixelFormatAttrs,
                                 int major,
                                 int minor,
                                 bool coreProfile,
                                 const GLRendererID &rendererID,
                                 const OSGLContext_win* shareContext)
    : _dc(0)
    , _handle(0)
    , _interval(0)
    , _windowHandle(0)
    , _windowDc(0)
{
    if ( !createWindow(&_windowHandle, &_windowDc) ) {
        throw std::runtime_error("WGL: Failed to create window");
    }

    createGLContext(pixelFormatAttrs, major, minor, coreProfile, rendererID, shareContext);

    if ( analyzeContextWGL(pixelFormatAttrs, major, minor) ) {
        // Some window hints require us to re-create the context using WGL
        // extensions retrieved through the current context, as we cannot
        // check for WGL extensions or retrieve WGL entry points before we
        // have a current context (actually until we have implicitly loaded
        // the vendor ICD)

        // Yes, this is strange, and yes, this is the proper way on WGL

        // As Windows only allows you to set the pixel format once for
        // a window, we need to destroy the current window and create a new
        // one to be able to use the new pixel format

        // Technically, it may be possible to keep the old window around if
        // we're just creating an OpenGL 3.0+ context with the same pixel
        // format, but it's not worth the added code complexity

        // First we clear the current context (the one we just created)
        // This is usually done by glfwDestroyWindow, but as we're not doing
        // full GLFW window destruction, it's duplicated here
        makeContextCurrent(nullptr);

        // Next destroy the Win32 window and WGL context (without resetting
        // or destroying the GLFW window object)
        destroyContext();
        destroyWindow();

        // ...and then create them again, this time with better APIs
        if ( !createWindow(&_windowHandle, &_windowDc) ) {
            throw std::runtime_error("WGL: Failed to create window");
        }

        createGLContext(pixelFormatAttrs, major, minor, coreProfile, rendererID, shareContext);
    }
}

OSGLContext_win::~OSGLContext_win()
{
}

bool
OSGLContext_win::makeContextCurrent(const OSGLContext_win* context)
{
    if (context) {
        return makeDCAndContextCurrent(context->_dc, context->_handle);
    } else {
        return makeDCAndContextCurrent(0, 0);
    }
}

bool
OSGLContext_win::threadHasACurrentContext()
{
    const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();
    assert(wglInfo);
    return wglInfo->GetCurrentContext() != nullptr;
}

void
OSGLContext_win::swapBuffers()
{
    SwapBuffers(_dc);
}

void
OSGLContext_win::swapInterval(int interval)
{
    _interval = interval;

    const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();
    assert(wglInfo);
    if (wglInfo->EXT_swap_control) {
        wglInfo->SwapIntervalEXT(interval);
    }
}

// A valid context must be bound
// http://developer.download.nvidia.com/opengl/specs/GL_NVX_gpu_memory_info.txt
static int
nvx_get_GPU_mem_info()
{
    const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();

    assert(wglInfo);
    if (!wglInfo->NVX_gpu_memory_info) {
        return 0;
    }

    int v = 0;
    // Clear error
    GLenum _glerror_ = glGetError();
    glGetIntegerv(GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &v);

    // If error, return 0
    _glerror_ = glGetError();
    if (_glerror_ != GL_NO_ERROR) {
        return 0;
    }

    return v * 1e3;
}

static std::string GetGPUInfoAMDInternal_string(const OSGLContext_wgl_data* wglInfo, UINT gpuID, int info)
{
    std::vector<char> data;
    int totalSize = 0;

    INT numVals;
    int safeCounter = 0;
    do {
        totalSize += 1024;
        if ((int)data.size() < totalSize) {
            data.resize(totalSize);
        }

        numVals = wglInfo->GetGPUInfoAMD(gpuID, info, GL_UNSIGNED_BYTE, data.size(), &data[0]);
        ++safeCounter;
    } while (numVals > 0 && numVals == (INT)data.size() && safeCounter < 1000);
    assert(numVals > 0);
    if (numVals <= 0) {
        return std::string();
    }

    return std::string(&data[0], numVals);
}

static bool GetGPUInfoAMDInternal_int(const OSGLContext_wgl_data* wglInfo, UINT gpuID, int info, int* value)
{
    std::vector<unsigned int> data;
    int totalSize = 0;

    INT numVals;
    int safeCounter = 0;
    do {
        totalSize += 10;
        if ((int)data.size() < totalSize) {
            data.resize(totalSize);
        }

        // AMD drivers are f*** up in 32 bits, they read a wrong buffer size.
        // It works fine in 64 bits mode
        int arrayDepth = data.size();
        numVals = wglInfo->GetGPUInfoAMD(gpuID, info, GL_UNSIGNED_INT, arrayDepth, &data[0]);
        ++safeCounter;
    } while (numVals > 0 && numVals == (INT)data.size() && safeCounter < 1000);
    assert(numVals > 0);
    if (numVals <= 0) {
        return false;
    }
    *value = (int)data[0];
    return true;
}

namespace {
class ScopedGLContext {
public:
    ScopedGLContext(const GLRendererID& gid) {
        const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();
        assert(wglInfo);
        _oldDc = wglInfo->GetCurrentDC();
        _oldContext = wglInfo->GetCurrentContext();

        try {
            _context = std::make_unique<OSGLContext_win>(FramebufferConfig(), GLVersion.major, GLVersion.minor, false, gid, nullptr);
        } catch (const std::exception& e) {
            std::cerr << "Failed to create GL context : " << e.what() << std::endl;
            return;
        }

        if (!OSGLContext_win::makeContextCurrent(_context.get())) {
            _context.reset();
        }
    }

    ~ScopedGLContext() {
        if (_context) {
            makeDCAndContextCurrent(_oldDc, _oldContext);
        }
    }

    explicit operator bool() const { return (bool)_context; }

private:
    std::unique_ptr<OSGLContext_win> _context;
    HDC _oldDc = nullptr;
    HGLRC _oldContext = nullptr;
};
} // namespace

void
OSGLContext_win::getGPUInfos(std::list<OpenGLRendererInfo>& renderers)
{

    const OSGLContext_wgl_data* wglInfo = appPTR->getWGLData();
    assert(wglInfo);
    if (!wglInfo) {
        return;
    }
    bool defaultFallback = false;
    if (wglInfo->NV_gpu_affinity) {
        // https://www.opengl.org/registry/specs/NV/gpu_affinity.txt
        std::vector<HGPUNV> gpuHandles;
        int gpuIndex = 0;
        HGPUNV gpuHandle;
        bool gotGPU = true;
        do {
            gotGPU = wglInfo->EnumGpusNV(gpuIndex, &gpuHandle);
            if (gotGPU) {
                gpuHandles.push_back(gpuHandle);
            }
            ++gpuIndex;
        } while (gotGPU);

        if (gpuHandles.empty()) {
            defaultFallback = true;
        }
        for (std::size_t i = 0; i < gpuHandles.size(); ++i) {
            OpenGLRendererInfo info;
            info.rendererID.rendererHandle = (void*)gpuHandles[i];
            {
                ScopedGLContext scopedContext(info.rendererID);
                if (!scopedContext) {
                    continue;
                }

                try {
                    OSGLContext::checkOpenGLVersion();
                } catch (const std::exception& e) {
                    std::cerr << e.what() << std::endl;
                    continue;
                }

                info.vendorName = std::string( (const char *) glGetString(GL_VENDOR) );
                info.rendererName = std::string( (const char *) glGetString(GL_RENDERER) );
                info.glVersionString = std::string( (const char *) glGetString(GL_VERSION) );
                info.glslVersionString = std::string( (const char*)glGetString (GL_SHADING_LANGUAGE_VERSION) );
                info.maxMemBytes = nvx_get_GPU_mem_info();
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
                renderers.push_back(info);
            }
        }
    } else if (wglInfo->AMD_gpu_association && !isApplication32Bits()) {
        //https://www.opengl.org/registry/specs/AMD/wgl_gpu_association.txt
        UINT getGpuIDMaxCount = wglInfo->GetGpuIDAMD(0, 0);
        UINT maxCount = getGpuIDMaxCount;
        std::vector<UINT> gpuIDs(maxCount);
        if (maxCount == 0) {
            defaultFallback = true;
        } else {
            UINT gpuCount = wglInfo->GetGpuIDAMD(maxCount, &gpuIDs[0]);
            if (gpuCount > maxCount) {
                gpuIDs.resize(gpuCount);
            }
            for (UINT index = 0; index < gpuCount; ++index) {
                assert(index < gpuIDs.size());
                UINT gpuID = gpuIDs[index];

                OpenGLRendererInfo info;
                info.rendererID.renderID = gpuID;

                {
                    ScopedGLContext scopedContext(info.rendererID);
                    if (!scopedContext) {
                        continue;
                    }

                    info.rendererName = GetGPUInfoAMDInternal_string(wglInfo, gpuID, WGL_GPU_RENDERER_STRING_AMD);
                    if (info.rendererName.empty()) {
                        continue;
                    }

                    info.vendorName = GetGPUInfoAMDInternal_string(wglInfo, gpuID, WGL_GPU_VENDOR_AMD);
                    if (info.vendorName.empty()) {
                        continue;
                    }

                    info.glVersionString = GetGPUInfoAMDInternal_string(wglInfo, gpuID, WGL_GPU_OPENGL_VERSION_STRING_AMD);
                    if (info.glVersionString.empty()) {
                        continue;
                    }

                    // note: cannot retrieve GL_SHADING_LANGUAGE_VERSION

                    info.maxMemBytes = 0;
                    if (!isApplication32Bits()) {
                        int ramMB = 0;
                        // AMD drivers are f*** up in 32 bits, they read a wrong buffer size.
                        // It works fine in 64 bits mode
                        if (!GetGPUInfoAMDInternal_int(wglInfo, gpuID, WGL_GPU_RAM_AMD, &ramMB)) {
                            continue;
                        }
                        info.maxMemBytes = ramMB * 1e6;
                    }

                    try {
                        OSGLContext::checkOpenGLVersion();
                    } catch (const std::exception& e) {
                        std::cerr << e.what() << std::endl;
                        continue;
                    }

                    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
                    renderers.push_back(info);
                }
            }
        }
    }

    if (renderers.empty()) {
        defaultFallback = true;
    }
    if (defaultFallback) {
        // No extension, use default
        {
            ScopedGLContext scopedContext{GLRendererID()};
            if (!scopedContext) {
                return;
            }

            try {
                OSGLContext::checkOpenGLVersion();
            } catch (const std::exception& e) {
                std::cerr << e.what() << std::endl;
                return;
            }

            OpenGLRendererInfo info;
            info.vendorName = std::string( (const char *) glGetString(GL_VENDOR) );
            info.rendererName = std::string( (const char *) glGetString(GL_RENDERER) );
            info.glVersionString = std::string( (const char *) glGetString(GL_VERSION) );
            info.glslVersionString = std::string( (const char *) glGetString (GL_SHADING_LANGUAGE_VERSION) );
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
            // We don't have any way to get memory size, set it to 0
            info.maxMemBytes = nvx_get_GPU_mem_info();
            renderers.push_back(info);
        }
    }
} // OSGLContext_win::getGPUInfos

NATRON_NAMESPACE_EXIT

#endif // __NATRON_WIN32__
