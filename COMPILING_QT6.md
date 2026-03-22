# Building Natron with Qt6 + Python 3.14 on Windows (MSYS2)

A step-by-step guide to building Natron from source with Qt6, PySide6, and Python 3.14 on Windows using MSYS2.

**Disk space:** Expect ~2-3GB for the full build directory (Natron.exe alone is ~819MB with debug info).

---

## 0. Pre-flight: Strawberry Perl Check

**Do this first.** If Strawberry Perl is installed, its bundled GCC will silently conflict with MSYS2's GCC and cause confusing build failures (shiboken crashes, moc crashes, wrong headers).

Either:
- **Uninstall Strawberry Perl**, or
- **Remove it from PATH** before building: Settings → Environment Variables → remove any `C:\Strawberry\...` entries from `Path`

---

## 1. Install MSYS2

Download and install MSYS2 from https://www.msys2.org/

Default install path: `C:\msys64`

After installing, open **MSYS2 MINGW64** terminal (not the regular MSYS2 terminal).

---

## 2. Install Dependencies

Run these commands in the MSYS2 MINGW64 terminal:

```bash
# Update MSYS2 first
pacman -Syu

# Core build tools
pacman -S --noconfirm \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  git

# Qt6 + Python bindings
pacman -S --noconfirm \
  mingw-w64-x86_64-qt6-base \
  mingw-w64-x86_64-pyside6 \
  mingw-w64-x86_64-shiboken6

# Required libraries
pacman -S --noconfirm \
  mingw-w64-x86_64-python \
  mingw-w64-x86_64-boost \
  mingw-w64-x86_64-cairo \
  mingw-w64-x86_64-expat

# For OFX plugins (Read/Write/Blur nodes)
pacman -S --noconfirm \
  mingw-w64-x86_64-openimageio \
  mingw-w64-x86_64-openexr \
  mingw-w64-x86_64-opencolorio \
  mingw-w64-x86_64-ffmpeg \
  mingw-w64-x86_64-libraw \
  mingw-w64-x86_64-libpng
```

---

## 3. Clone the Repository

```bash
cd /d/projects  # or wherever you want to put it

# Clone Natron (use the Qt6 migration branch/fork)
git clone https://github.com/YOUR_USERNAME/Natron.git
cd Natron

# Initialize submodules (OpenFX, SequenceParsing, google-test, etc.)
git submodule update --init --recursive
```

---

## 4. Configure with CMake

```bash
# Create build directory
mkdir build-qt6
cd build-qt6

# Set environment variables
export PATH="/c/msys64/mingw64/bin:$PATH"
export LLVM_INSTALL_DIR=C:/msys64/mingw64

# Configure
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DNATRON_QT6=ON \
  -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe \
  -DPython3_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe \
  -DPython_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe \
  -DPython3_ROOT_DIR=/c/msys64/mingw64 \
  -DPython_ROOT_DIR=/c/msys64/mingw64 \
  -DNATRON_LLVM_INSTALL_DIR=C:/msys64/mingw64
```

You should see output ending with:
```
-- Configuring done
-- Generating done
-- Build files have been written to: ...
```

---

## 5. Build Natron

```bash
# Make sure LLVM_INSTALL_DIR is set (needed for shiboken6)
export LLVM_INSTALL_DIR=C:/msys64/mingw64

# Build (use -j2 to avoid parallel build issues)
mingw32-make -j2
```

This will take 5-15 minutes depending on your machine. When complete you'll see:
```
[100%] Built target Natron
```

The executable is at: `build-qt6/App/Natron.exe`

---

## 6. Build OFX Plugins (optional but recommended)

Without plugins, Natron has no Read/Write/Blur/Merge nodes. You need two plugin repos:

### openfx-misc (Blur, Merge, Transform, etc.)

```bash
cd /d/projects  # same parent directory as Natron
git clone https://github.com/NatronGitHub/openfx-misc.git
cd openfx-misc
git submodule update --init --recursive
mkdir build && cd build

cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

mingw32-make -j2
```

> **CMake error about CMAKE_SYSTEM_PROCESSOR?** If you get an error about empty `CMAKE_SYSTEM_PROCESSOR`,
> edit the plugin's `CMakeLists.txt` and quote it: change `${CMAKE_SYSTEM_PROCESSOR}` to `"${CMAKE_SYSTEM_PROCESSOR}"`.

### openfx-io (Read, Write — EXR, PNG, FFmpeg, etc.)

```bash
cd /d/projects
git clone https://github.com/NatronGitHub/openfx-io.git
cd openfx-io
git submodule update --init --recursive
mkdir build && cd build

cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

mingw32-make -j2
```

### Known patches for openfx-io

openfx-io has not been updated for OpenImageIO 3.x and FFmpeg 8.x. You will need these fixes:

**1. OIIO 3.x — ImageCache returns shared_ptr** (`OIIO/ReadOIIO.cpp`):
```cpp
// Change: ImageCache* _cache;
// To:     std::shared_ptr<ImageCache> _cache;
// Also change: ImageCache* sharedcache = ImageCache::create(true);
// To:          std::shared_ptr<ImageCache> sharedcache = ImageCache::create(true);
// And: , _cache(NULL)  →  , _cache(nullptr)
```

**2. OIIO 3.x — ImageIOParameterList removed** (`OIIO/ReadOIIO.cpp`):
```cpp
// Change: for (ImageIOParameterList::const_iterator p = ...
// To:     for (auto p = subImages[sIt].extra_attribs.cbegin(); p != ...
```

**3. OIIO 3.x — read_scanlines/read_tiles need subimage, miplevel** (`OIIO/ReadOIIO.cpp`):
```cpp
// Add subimage and miplevel (0) as the first two arguments:
// Change: img->read_scanlines(ybegin, yend, z, chbegin, chend, ...
// To:     img->read_scanlines(subImageIndex, 0, ybegin, yend, z, chbegin, chend, ...
// Same for read_tiles — add subImageIndex, 0 as first two args.
```

**4. OIIO 3.x — OIIO_NAMESPACE removed** (`OIIO/WriteOIIO.cpp`):
```cpp
// Change: OIIO_NAMESPACE::TypeDesc oiioBitDepth;
// To:     OIIO::TypeDesc oiioBitDepth;
```

**5. FFmpeg 8.x — pkt_duration renamed** (`FFmpeg/FFmpegFile.cpp`):
```cpp
// Change: decodedFrame->pkt_duration
// To:     decodedFrame->duration
// (two occurrences)
```

**6. OpenImageIO_Util linking** (`cmake/Modules/FindOpenImageIO.cmake`):
```cmake
# After: set (OPENIMAGEIO_LIBRARIES ${OPENIMAGEIO_LIBRARY})
# Add:
find_library(OPENIMAGEIO_UTIL_LIBRARY NAMES OpenImageIO_Util HINTS ${OPENIMAGEIO_LIBRARY_DIRS})
if (OPENIMAGEIO_UTIL_LIBRARY)
    list(APPEND OPENIMAGEIO_LIBRARIES ${OPENIMAGEIO_UTIL_LIBRARY})
endif()
```

---

## 7. Install Plugins

Copy the built `.ofx` files into Natron's plugin directory:

```bash
# Create plugin directories
NATRON_DIR="/d/projects/Natron/build-qt6"
mkdir -p "$NATRON_DIR/Plugins/OFX/Natron/Misc.ofx.bundle/Contents/Win64"
mkdir -p "$NATRON_DIR/Plugins/OFX/Natron/IO.ofx.bundle/Contents/Win64"

# Copy plugins
cp /d/projects/openfx-misc/build/Misc.ofx \
   "$NATRON_DIR/Plugins/OFX/Natron/Misc.ofx.bundle/Contents/Win64/Misc.ofx"

cp /d/projects/openfx-io/build/IO.ofx \
   "$NATRON_DIR/Plugins/OFX/Natron/IO.ofx.bundle/Contents/Win64/IO.ofx"
```

---

## 8. Bundle DLLs and Python (standalone launch)

Natron needs MSYS2's DLLs and Python standard library to run. You can either launch from the MSYS2 terminal (step 8a) or bundle everything so it runs standalone from Windows Explorer (step 8b).

### 8a. Run from MSYS2 terminal (quick)

```bash
cd /d/projects/Natron/build-qt6/App
export PATH="/c/msys64/mingw64/bin:$PATH"
./Natron.exe
```

### 8b. Bundle for standalone launch (double-click from Explorer)

Copy all MSYS2 DLLs into the App directory. Natron and its dependencies (Qt6, OIIO, FFmpeg, Python, etc.) have deep dependency chains, so the simplest approach is to copy all of them:

```bash
cd /d/projects/Natron/build-qt6/App

# Copy all MSYS2 DLLs (simplest, ~700 DLLs, ~1.5GB)
cp /c/msys64/mingw64/bin/*.dll .

# Qt6 platform plugin (required for windowing)
mkdir -p platforms
cp /c/msys64/mingw64/share/qt6/plugins/platforms/qwindows.dll platforms/

# Python standard library (required for scripting)
cp -r /c/msys64/mingw64/lib/python3.14 ../lib/python3.14
```

> **Note:** Copying all DLLs is brute-force but reliable. For a leaner distribution, you can use `objdump -p Natron.exe` to trace only the required DLLs, but the dependency chain is deep (Qt6 → OIIO → FFmpeg → codecs) and missing even one will cause a launch error.

After this, you can double-click `Natron.exe` from Windows Explorer without needing the MSYS2 terminal.

### Verify it works:
1. **Python:** Open Script Editor, type `import NatronEngine; print(NatronEngine.natron.getNatronVersionString())`
2. **Nodes:** Press Tab in the node graph, type "Blur" — should find DirBlur, GodRays, etc.
3. **Read files:** Create a Read node and load an EXR or PNG image

---

## Troubleshooting

### "libfontconfig-1.dll was not found" (or other DLL errors on double-click)
The MSYS2 DLLs aren't bundled with the exe. Either launch from the MSYS2 terminal (step 8a) or bundle the DLLs (step 8b).

### "Failed to import encodings module" on launch
Python's standard library isn't bundled. Copy it with:
```bash
cp -r /c/msys64/mingw64/lib/python3.14 /d/projects/Natron/build-qt6/lib/python3.14
```

### "Could not find a decoder to read exr file format"
The OFX IO plugin isn't installed. Follow step 7 above.

### No Blur/Merge nodes in the menu
The OFX Misc plugin isn't installed. Follow step 7 above.

### Shiboken fails with "mm_malloc.h file not found"
`LLVM_INSTALL_DIR` is not set. Run:
```bash
export LLVM_INSTALL_DIR=C:/msys64/mingw64
```

### Shiboken fails with "Strawberry" in the error path
Strawberry Perl's GCC is conflicting with MSYS2's. Fix:
```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
```
Make sure MSYS2's bin comes FIRST in PATH, before Strawberry.

### CMake says "Python minor version is not compatible"
CMake found a different Python (e.g. Python 3.12 from a standalone install). Fix by specifying the MSYS2 Python explicitly:
```bash
-DPython3_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe
-DPython_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe
-DPython3_ROOT_DIR=/c/msys64/mingw64
-DPython_ROOT_DIR=/c/msys64/mingw64
```

### moc crashes with exit code 0xC0000005
Parallel build race condition. Try:
```bash
mingw32-make -j2  # usually works (recommended)
mingw32-make -j1  # single-threaded fallback, slower but always reliable
```
Note: `-j4` or higher generally works fine for the main Natron build on 16GB+ machines, but can cause issues during the autogen/moc phase.

---

## What This Build Includes

| Component | Version |
|-----------|---------|
| Natron | 2.6 |
| Qt | 6.10.1 |
| Python | 3.14.3 |
| PySide6 | 6.10.2 |
| Shiboken6 | 6.10.2 |
| Boost | 1.90 |
| Cairo | 1.18.4 |
| OpenImageIO | 3.1.11 |
| OpenEXR | 3.4.6 |
| FFmpeg | 7.x / 8.x (MSYS2 may ship either) |
| GCC | 15.2.0 |

---

## Technical Details

For a complete list of all code changes, API migrations, and architectural decisions,
see `MIGRATION_QT6_TRACKER.md` in the repository root.
