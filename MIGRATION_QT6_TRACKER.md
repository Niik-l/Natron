# Natron Qt6 + PySide6 Migration Tracker

> This document tracks the Qt5→Qt6 and PySide2→PySide6 migration effort.
> Started: 2026-03-20
> Goal: Enable Natron to build with Qt6/PySide6/Python 3.14+ on MSYS2/Windows

---

## Executive Summary

**Natron is already significantly prepared for Qt6.** The codebase has:
- A `NATRON_QT6` CMake flag with dual Qt5/Qt6 build paths
- Dual PySide2/PySide6 + Shiboken2/Shiboken6 support in CMake
- A `QtCompat.h` compatibility header for Qt5/Qt6 differences
- Already migrated from QGLWidget → QOpenGLWidget (all 6 GL widgets)
- Modern include style (`#include <QWidget>` not `#include <QtGui/QWidget>`)
- GLAD-based OpenGL loader (not Qt's deprecated GL wrappers)
- Custom OS-level GL contexts (WGL/CGL/GLX) independent of Qt

The remaining work is manageable and mostly mechanical.

---

## Codebase Statistics

| Metric | Count |
|--------|-------|
| Total .cpp/.h files | ~1,364 |
| Files touching Qt APIs | ~414 |
| Files with deprecated Qt APIs | ~24 |
| C++ classes exposed to Python | ~98 (90 Engine + 8 GUI) |
| Shiboken typesystem XML files | 3 |
| OpenGL rendering widgets | 6 (all QOpenGLWidget) |

---

## Phase 1: Deprecated API Replacements (MECHANICAL)

### 1A. QDesktopWidget → QScreen (7 files)

QDesktopWidget was removed in Qt6. Replace with QScreen/QGuiApplication::primaryScreen().

| File | Status |
|------|--------|
| `Gui/GuiApplicationManager10.cpp` | [x] DONE — QScreen with version guard |
| `Gui/Histogram.cpp` | [x] DONE — Removed unused include |
| `Gui/MessageBox.cpp` | [x] DONE — Removed unused include |
| `Gui/NodeCreationDialog.cpp` | [x] DONE — Removed unused include |
| `Gui/CurveWidget.cpp` | [x] Already used QScreen |
| `Gui/FloatingWidget.cpp` | [x] Already used QScreen |
| `Gui/ViewerGLPrivate.cpp` | [x] No QDesktopWidget usage |

**Pattern:**
```cpp
// OLD (Qt5)
#include <QDesktopWidget>
QDesktopWidget *dw = QApplication::desktop();
QRect screenRect = dw->screenGeometry();

// NEW (Qt6)
#include <QScreen>
QScreen *screen = QGuiApplication::primaryScreen();
QRect screenRect = screen->geometry();
```

### 1B. QRegExp → QRegularExpression (17 files)

QRegExp was removed in Qt6. Replace with QRegularExpression.

| File | Status |
|------|--------|
| `Gui/ScriptTextEdit.cpp` | [x] DONE — Full migration: struct members, highlightBlock, matchMultiline |
| `Gui/ScriptTextEdit.h` | [x] DONE — Version-guarded method signature |
| `Gui/PreferencesPanel.cpp` | [x] DONE — wildcardToRegexUnanchored helper |
| `Gui/NodeGraph45.cpp` | [x] DONE — wildcardToRegex helper |
| `Gui/FileTypeMainWindow_win.cpp` | [x] DONE — DDE command parsing |
| `Gui/ViewerGL.cpp` | [x] DONE — Text tokenization |
| `Gui/NodeCreationDialog.cpp` | [x] DONE — Wildcard search with matchedLength |
| `Gui/RenderStatsDialog.cpp` | [x] DONE — Stats filtering |
| `Gui/SequenceFileDialog.h` | [x] DONE — Version-guarded include |
| `Engine/Project.cpp` | [x] DONE — Backup file versioning |
| `Engine/OutputEffectInstance.cpp` | [x] DONE — Frame format validation |
| `Engine/Node.cpp` | [x] DONE — Version-guarded include |
| `Engine/NodeDocumentation.cpp` | [x] DONE — URL auto-linking |
| `Engine/Markdown.cpp` | [x] DONE — HTML processing |
| `Engine/FileSystemModel.cpp` | [x] DONE — Member storage + wildcardToRegex |
| `Engine/CLArgs.cpp` | [x] DONE — Frame range validation |

**Pattern:**
```cpp
// OLD (Qt5)
#include <QRegExp>
QRegExp rx("pattern");
if (rx.exactMatch(str)) { ... }
int pos = rx.indexIn(str);
QString cap = rx.cap(1);

// NEW (Qt6)
#include <QRegularExpression>
QRegularExpression rx("pattern");
QRegularExpressionMatch match = rx.match(str);
if (match.hasMatch()) { ... }
// indexIn → use match.capturedStart()
QString cap = match.captured(1);
```

### 1C. Other Deprecated APIs

| API Change | Files | Status |
|------------|-------|--------|
| QEnterEvent typedef (already in QtCompat.h) | 46 files | [x] DONE |
| QGLWidget → QOpenGLWidget | 0 files | [x] DONE (already migrated) |
| Qt6 OpenGLWidgets module in CMake | CMakeLists.txt | [x] DONE |

---

## Phase 2: Build System Verification

### 2A. CMake Qt6 Path (Already Partially Done)

| Task | Status |
|------|--------|
| Root CMakeLists.txt NATRON_QT6 option | [x] EXISTS |
| Qt6 find_package with correct components | [x] EXISTS |
| Qt6::OpenGLWidgets component | [x] EXISTS |
| Shiboken6 find_package | [x] EXISTS |
| PySide6 find_package | [x] EXISTS |
| Engine/CMakeLists.txt shiboken6 generation | [x] EXISTS |
| Gui/CMakeLists.txt shiboken6 generation | [x] EXISTS |
| Windows debug build handling | [x] EXISTS |
| **Test: Actually configure with -DNATRON_QT6=ON** | [ ] TODO |
| **Test: Build NatronEngine with Qt6** | [ ] TODO |
| **Test: Build NatronGui with Qt6** | [ ] TODO |

### 2B. qmake Build System

The legacy qmake (.pro) system does NOT have Qt6 support. This is secondary — CMake is the primary system.

---

## Phase 3: PySide6/Shiboken6 Binding Updates

### 3A. Typesystem XML Files

| File | Status | Notes |
|------|--------|-------|
| `Engine/typesystem_engine.xml` | [ ] REVIEW | Check for PySide2-specific syntax |
| `Gui/typesystem_natronGui.xml` | [ ] REVIEW | Check for PySide2-specific syntax |
| `Shiboken/typesystem_widgets.xml` | [ ] REVIEW | Qt widget type mappings |

### 3B. Python Embedding Code

| File | Status | Notes |
|------|--------|-------|
| `Global/PythonUtils.cpp` | [x] DONE | Full PyConfig rewrite for 3.13+, guarded deprecated APIs |
| `Global/PythonUtils.h` | [x] No changes needed | Version macros still valid |
| `PythonBin/python_main.cpp` | [x] DONE | PyConfig+Py_RunMain for 3.13+, legacy Py_Main for older |
| `Engine/AppManager.cpp` | [x] DONE | Guarded Py_NoUserSiteDirectory |

### 3C. Python 3.14 C API Breaking Changes

Key changes in Python 3.13-3.14 that affect Natron — **ALL ADDRESSED:**
- `PyEval_InitThreads()` — already conditionally compiled for <3.7, removed in 3.13+ path
- `Py_SetPythonHome()` — replaced with `PyConfig.home` in 3.13+ path
- `Py_SetProgramName()` — replaced with `PyConfig.program_name` in 3.13+ path
- `PySys_SetArgv()` — replaced with `PyConfig_SetArgv()` in 3.13+ path
- `Py_Initialize()` — replaced with `Py_InitializeFromConfig()` in 3.13+ path
- `Py_NoUserSiteDirectory` global — guarded with `#if PY_VERSION_HEX < 0x030D0000`
- `Py_DebugFlag` etc. — replaced with `sys.flags` query in 3.13+ debug path
- `distutils.sysconfig` — replaced with `sysconfig` for Python 3.12+
- `Py_Main()` in PythonBin — replaced with `PyConfig` + `Py_RunMain()` for 3.13+

---

## Phase 4: Runtime/Visual Testing

These changes can only be verified by running the application:

| Area | Risk Level | Notes |
|------|-----------|-------|
| Viewer rendering | HIGH | QOpenGLWidget behavior differences in Qt6 |
| Font rendering | MEDIUM | Qt6 changed text rendering pipeline |
| HiDPI scaling | MEDIUM | Qt6 enables HiDPI by default |
| Node graph interactions | LOW | Mostly mouse/keyboard events |
| Script editor | LOW | QTextEdit-based, minimal changes |
| PyPlugs (Python plugins) | MEDIUM | Depend on NatronEngine import |

---

## What's Already Done (Credit to Natron Devs)

The Natron team has already completed significant Qt6 preparation:

1. **QGLWidget → QOpenGLWidget migration** — All 6 OpenGL widgets migrated
2. **CMake dual-build support** — `NATRON_QT6` flag with complete Qt5/Qt6 paths
3. **PySide2/PySide6 dual support** — CMake correctly finds either version
4. **Shiboken2/Shiboken6 generation** — Build pipeline handles both
5. **QtCompat.h** — Compatibility header for QEnterEvent and other differences
6. **Modern include style** — No old-style module includes to fix
7. **GLAD OpenGL loader** — Independent of Qt's GL wrappers
8. **Custom OS GL contexts** — WGL/CGL/GLX code doesn't depend on Qt GL classes

---

## Files Changed Log

| Date | File | Change Description |
|------|------|--------------------|
| 2026-03-20 | `Global/QtCompat.h` | Extended with `wildcardToRegex()` and `wildcardToRegexUnanchored()` helpers, conditional QRegExp/QRegularExpression include |
| 2026-03-20 | `Gui/GuiApplicationManager10.cpp` | QDesktopWidget→QScreen for DPI detection, version-guarded |
| 2026-03-20 | `Gui/Histogram.cpp` | Removed unused `#include <QDesktopWidget>` |
| 2026-03-20 | `Gui/MessageBox.cpp` | Removed unused `#include <QDesktopWidget>` |
| 2026-03-20 | `Gui/NodeCreationDialog.cpp` | Removed QDesktopWidget include, migrated QRegExp wildcard to QtCompat helper |
| 2026-03-20 | `Gui/ScriptTextEdit.h` | Version-guarded matchMultiline signature (QRegExp vs QRegularExpression) |
| 2026-03-20 | `Gui/ScriptTextEdit.cpp` | Full QRegExp→QRegularExpression migration: struct members, highlightBlock, matchMultiline |
| 2026-03-20 | `Gui/FileTypeMainWindow_win.cpp` | QRegExp→QRegularExpression for DDE command parsing |
| 2026-03-20 | `Gui/ViewerGL.cpp` | QRegExp→QRegularExpression for text tokenization |
| 2026-03-20 | `Gui/NodeGraph45.cpp` | QRegExp wildcard→QtCompat::wildcardToRegex for node search |
| 2026-03-20 | `Gui/PreferencesPanel.cpp` | QRegExp wildcard→QtCompat::wildcardToRegexUnanchored for plugin filter |
| 2026-03-20 | `Gui/RenderStatsDialog.cpp` | QRegExp wildcard→QtCompat::wildcardToRegex for stats filter |
| 2026-03-20 | `Gui/SequenceFileDialog.h` | Version-guarded QRegExp/QRegularExpression include |
| 2026-03-20 | `Engine/Project.cpp` | QRegExp→QRegularExpression for backup file versioning |
| 2026-03-20 | `Engine/OutputEffectInstance.cpp` | QRegExp→QRegularExpression for frame format validation |
| 2026-03-20 | `Engine/Markdown.cpp` | QRegExp→QRegularExpression for HTML processing |
| 2026-03-20 | `Engine/NodeDocumentation.cpp` | QRegExp→QRegularExpression for URL auto-linking |
| 2026-03-20 | `Engine/CLArgs.cpp` | QRegExp→QRegularExpression for frame range validation |
| 2026-03-20 | `Engine/Node.cpp` | Version-guarded QRegExp/QRegularExpression include |
| 2026-03-20 | `Engine/FileSystemModel.cpp` | QRegExp→QRegularExpression member storage + wildcardToRegex |
| 2026-03-20 | `Global/PythonUtils.cpp` | Full PyConfig API rewrite for Python 3.13+, guarded Py_NoUserSiteDirectory, distutils→sysconfig |
| 2026-03-20 | `Engine/AppManager.cpp` | Guarded Py_NoUserSiteDirectory for Python 3.13+ |
| 2026-03-20 | `PythonBin/python_main.cpp` | PyConfig+Py_RunMain for Python 3.13+, legacy Py_Main kept for older |

---

## Architecture Notes for Developers

### Module Dependency Chain
```
gflags → glog → ceres → libmv/openMVG
                    ↓
HostSupport → Engine → Gui → App
                  ↓        ↓
              Renderer  PythonBin
```

### Key Compatibility Files
- `Global/QtCompat.h` — Central Qt5/Qt6 compatibility typedefs
- `Global/GLIncludes.h` — OpenGL includes with GLAD, state management helpers
- `Global/PythonUtils.h/cpp` — Python embedding with version-conditional code

### Shiboken Generation Pipeline
```
typesystem_*.xml → shiboken2/6 generator → *_wrapper.cpp files
                                         → sourceCleanup.py (namespace fixes)
                                         → PySide{2,6}_*_Python.h
```

### OpenGL Rendering Architecture
- 6 QOpenGLWidget subclasses handle on-screen rendering
- OSGLContext_* classes handle offscreen rendering (platform-specific, NOT Qt-based)
- GPUContextPool manages thread-safe context sharing
- GLShader wraps shader compilation (custom, not QOpenGLShaderProgram for engine)
- ViewerGL uses QOpenGLShaderProgram for display shaders
