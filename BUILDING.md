# Building Natron from Source (Windows / MSYS2)

A step-by-step guide to building Natron from source with Qt6, PySide6, and Python 3.14 on Windows using MSYS2. Includes the 3D system, Cycles renderer, and OFX plugins.

**Disk space:** Expect ~2-3GB for the full build directory (Natron.exe alone is ~819MB with debug info).

---

## 0. Pre-flight: Strawberry Perl Check

**Do this first.** If Strawberry Perl is installed, its bundled GCC will silently conflict with MSYS2's GCC and cause confusing build failures (shiboken crashes, moc crashes, wrong headers).

Pick one of:
- **Uninstall Strawberry Perl**
- **Remove it from Windows PATH** before building: Settings → Environment Variables → remove any `C:\Strawberry\...` entries from `Path`
- **Leave it installed and on PATH, but always prepend `/c/msys64/mingw64/bin` to `PATH` inside the MINGW64 shell** so MSYS2's GCC wins lookup. Step 4 already does this with `export PATH="/c/msys64/mingw64/bin:$PATH"` — extend the same pattern to any extra build scripts you write.

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

# Clone Natron
git clone https://github.com/Niik-l/Natron.git
cd Natron
git checkout RB-2.6

# Initialize submodules (OpenFX, SequenceParsing, google-test, etc.)
git submodule update --init --recursive
```

---

## 4. Configure with CMake

```bash
# Create build directory
mkdir build-qt6
cd build-qt6

# Set environment variables (needed in every new terminal session)
# If using MSYS2 MINGW64 terminal, PATH is already set — but LLVM_INSTALL_DIR is still needed
export PATH="/c/msys64/mingw64/bin:$PATH"
export LLVM_INSTALL_DIR=C:/msys64/mingw64

# Configure
# To enable Cycles, append the three -DNATRON_CYCLES_* lines below
# (requires Cycles built first — see step 10):
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DNATRON_QT6=ON \
  -DNATRON_BUILD_TESTS=OFF \
  -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe \
  -DPython3_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe \
  -DPython_EXECUTABLE=/c/msys64/mingw64/bin/python3.exe \
  -DPython3_ROOT_DIR=/c/msys64/mingw64 \
  -DPython_ROOT_DIR=/c/msys64/mingw64 \
  -DNATRON_LLVM_INSTALL_DIR=C:/msys64/mingw64
  # Optional — enable Cycles:
  # -DNATRON_CYCLES=ON \
  # -DNATRON_CYCLES_DIR=/d/projects/cycles \
  # -DNATRON_CYCLES_BUILD_DIR=/d/projects/cycles/build
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

# If you hit "ar: malformed archive" or race conditions, drop to single-threaded:
# mingw32-make -j1
```

This will take 5-15 minutes depending on your machine (`-j1` is slower but always reliable). When complete you'll see:
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

## 8. Install OpenColorIO Configs (optional)

Natron uses OpenColorIO for color management. The default config presets (nuke-default, blender, natron) ship as a separate tarball. Without them, Natron launches fine — color knobs still work — but you'll see OCIO warnings and lose the named presets.

At runtime, Natron searches `<binary>/../Resources/OpenColorIO-Configs/` (see `Engine/Settings.cpp`). For a dev build run from `build-qt6/App/Natron.exe`, that resolves to `build-qt6/Resources/OpenColorIO-Configs/`.

```bash
cd /d/projects/Natron
curl -L https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.4.tar.gz \
  -o ocio-configs.tar.gz
tar xzf ocio-configs.tar.gz
mkdir -p build-qt6/Resources
mv OpenColorIO-Configs-Natron-v2.4 build-qt6/Resources/OpenColorIO-Configs
rm ocio-configs.tar.gz
```

> **Note:** If MSYS2's `curl` fails with a TLS or connection error, fall back to PowerShell `Invoke-WebRequest` or `git clone https://github.com/NatronGitHub/OpenColorIO-Configs.git`.

**Alternative — `OCIO` environment variable.** If you have a `config.ocio` elsewhere, set it before launching to override the search path entirely:
```bash
export OCIO=/path/to/your/config.ocio
./Natron.exe
```

---

## 9. Bundle DLLs and Python (standalone launch)

Natron needs MSYS2's DLLs and Python standard library to run. You can either launch from the MSYS2 terminal (step 9a) or bundle everything so it runs standalone from Windows Explorer (step 9b).

### 9a. Run from MSYS2 terminal (quick)

```bash
cd /d/projects/Natron/build-qt6/App
export PATH="/c/msys64/mingw64/bin:$PATH"
./Natron.exe
```

### 9b. Bundle for standalone launch (double-click from Explorer)

After bundling, you can double-click `Natron.exe` from Windows Explorer without needing the MSYS2 terminal. Two ways to bundle the DLLs — pick one:

#### Option A — Copy all MSYS2 DLLs (brute-force, ~700 DLLs / ~1.5 GB)

Simplest and most reliable — no extra tooling required. Has deep dependency chains (Qt6, OIIO, FFmpeg, Python, etc.) covered by default.

```bash
cd /d/projects/Natron/build-qt6/App
cp /c/msys64/mingw64/bin/*.dll .
```

#### Option B — Trace only required DLLs (leaner)

Use `ntldd` to walk Natron's actual dependency tree and copy only what's needed:

```bash
pacman -S mingw-w64-x86_64-ntldd
cd /d/projects/Natron/build-qt6/App
ntldd -R Natron.exe | grep mingw64 | awk '{print $3}' | xargs -I {} cp {} .
```

This produces a much smaller bundle, but may miss DLLs loaded dynamically at runtime (Qt plugins, OCIO config plugins, OFX plugins, etc.). If launching fails with a missing-DLL error, fall back to Option A or copy the specific missing DLL by hand.

#### Common steps (both options)

```bash
# Qt6 platform plugin (required for windowing)
mkdir -p platforms
cp /c/msys64/mingw64/share/qt6/plugins/platforms/qwindows.dll platforms/

# Python standard library (required for scripting)
# Check your Python version with: python3 --version
cp -r /c/msys64/mingw64/lib/python3.14 ../lib/python3.14
```

> **Note:** The Python version (3.14) may change when MSYS2 updates. Check with `python3 --version` and adjust the path accordingly.

### Verify it works:
1. **Python:** Open Script Editor, type `import NatronEngine; print(NatronEngine.natron.getNatronVersionString())`
2. **Nodes:** Press Tab in the node graph, type "Blur" — should find DirBlur, GodRays, etc.
3. **Read files:** Create a Read node and load an EXR or PNG image

> See `NODE_REGISTRY.md` for a full list of registered plugin IDs (including Dev nodes like ReadVDB, ParticleSolver, CyclesRender, etc.).

---

## 10. Enable Cycles Renderer (optional)

Natron includes an optional Cycles path tracer integration (the same renderer used by Blender). It's disabled by default.

### Install Cycles dependencies

```bash
pacman -S --noconfirm \
  mingw-w64-x86_64-embree \
  mingw-w64-x86_64-openimagedenoise \
  mingw-w64-x86_64-openpgl \
  mingw-w64-x86_64-opensubdiv \
  mingw-w64-x86_64-openvdb \
  mingw-w64-x86_64-pugixml \
  mingw-w64-x86_64-libepoxy
```

`mingw-w64-x86_64-openvdb` is required: Natron's `Engine/Dev/Cycles/` code calls `ccl::VDBImageLoader` unconditionally, so Cycles must be built **with** OpenVDB for the Natron link to succeed (see `WITH_CYCLES_OPENVDB` below).

### Build Cycles standalone

```bash
cd /d/projects
git clone https://github.com/blender/cycles.git
cd cycles
git checkout v5.0.0

# Apply MinGW compatibility patch (from Natron repo)
git apply /d/projects/Natron/patches/cycles-mingw.patch

mkdir build && cd build
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_CYCLES_STANDALONE=ON \
  -DWITH_CYCLES_STANDALONE_GUI=OFF \
  -DWITH_CYCLES_USD=OFF \
  -DWITH_USD=OFF \
  -DWITH_CYCLES_OSL=OFF \
  -DWITH_CYCLES_DEVICE_CUDA=OFF \
  -DWITH_CYCLES_DEVICE_OPTIX=OFF \
  -DWITH_CYCLES_DEVICE_HIP=OFF \
  -DWITH_CYCLES_DEVICE_ONEAPI=OFF \
  -DWITH_CYCLES_EMBREE=ON \
  -DWITH_CYCLES_OPENIMAGEDENOISE=ON \
  -DWITH_CYCLES_OPENCOLORIO=ON \
  -DWITH_CYCLES_OPENSUBDIV=ON \
  -DWITH_CYCLES_OPENVDB=ON \
  -DWITH_CYCLES_ALEMBIC=OFF \
  -DWITH_CYCLES_LOGGING=OFF \
  -DWITH_LIBS_PRECOMPILED=OFF \
  -DTBB_ROOT_DIR=/c/msys64/mingw64

mingw32-make -j2
```

> **Why `WITH_CYCLES_OPENVDB=ON`:** Natron's `Engine/Dev/Cycles/CyclesRenderer.cpp` calls `ccl::VDBImageLoader` without `#ifdef` guards (used by ReadVDB → CyclesRender for fire/smoke volumes). Building Cycles with `WITH_CYCLES_OPENVDB=OFF` produces a Cycles library that's missing the symbol, and the subsequent Natron link will fail with `undefined reference to ccl::VDBImageLoader::VDBImageLoader(...)`. Worse, the linker deletes the previous working `Natron.exe` before failing, leaving no binary. Keep `NanoVDB` at its default `ON` — disabling it independently of OpenVDB breaks `cycles/src/util/nanovdb.cpp`.

### Rebuild Natron with Cycles enabled

Go back to your Natron build directory and reconfigure:

```bash
cd /d/projects/Natron/build-qt6

cmake .. -G "MinGW Makefiles" \
  -DNATRON_CYCLES=ON \
  -DNATRON_CYCLES_DIR=/d/projects/cycles \
  -DNATRON_CYCLES_BUILD_DIR=/d/projects/cycles/build

mingw32-make -j2
```

The **CyclesRender** node will now appear in the 3D menu group. Connect Scene3D + Camera3D to it for path-traced output.

> **Note:** If enabling Cycles for the first time on an existing build, you may need to clear the AUTOMOC cache:
> `rm -rf Engine/NatronEngine_autogen && cmake .. -G "MinGW Makefiles"` then rebuild.

---

## Troubleshooting

### GCC 15 + Eigen linker errors (undefined reference to `evaluator_base`, `triangular_assignment_loop`, etc.)

GCC 15 changed how it handles C++ template instantiation at `-O2`, which breaks Eigen (the linear algebra library used by libmv, ceres, and the Tracker). This causes hundreds of `undefined reference` linker errors.

**This is already fixed in the repo** — `libs/libmv/CMakeLists.txt`, `libs/ceres/CMakeLists.txt`, and `Engine/CMakeLists.txt` apply `-O1` to Eigen-consuming targets when GCC >= 15 is detected. If you see these errors on a fresh build, make sure you have the latest code.

### Tests / NatronRenderer fail to link

The test suite (`Tests.exe`) can hit the GCC 15 / Eigen issue described above. Disable tests:
```bash
cmake .. -DNATRON_BUILD_TESTS=OFF
```
This only affects the test binary — the main `Natron.exe` is unaffected.

`NatronRenderer.exe` can fail to link for **two** different reasons. Diagnose by the missing-symbol name:

- **`ccl::VDBImageLoader::VDBImageLoader(...)`** — Cycles was built without OpenVDB. See step 10: `WITH_CYCLES_OPENVDB` must be `ON` for the Natron link to succeed.
- **Eigen symbols (`evaluator_base`, `triangular_assignment_loop`, etc.)** — same GCC 15 / Eigen issue as `Tests.exe`. Should already be patched in the repo.

After fixing either, retest with:
```bash
mingw32-make NatronRenderer -j2
```

### "cmake: command not found" or "mingw32-make: command not found"

You opened the wrong MSYS2 terminal. Make sure you're using **"MSYS2 MINGW64"** (not plain "MSYS2"). The prompt should show `MINGW64`, not `MSYS`:
```
user@host MINGW64 ~    <-- correct
user@host MSYS ~       <-- wrong
```

### Cycles build: `moc_CyclesRender.cpp: No such file or directory`

AUTOMOC hasn't generated the MOC file for CyclesRender. Clear the autogen cache and reconfigure:
```bash
rm -rf Engine/NatronEngine_autogen
cmake .. -DNATRON_BUILD_TESTS=OFF -DNATRON_CYCLES=ON \
  -DNATRON_CYCLES_DIR=/path/to/cycles \
  -DNATRON_CYCLES_BUILD_DIR=/path/to/cycles/build
mingw32-make -j2
```

### "libfontconfig-1.dll was not found" (or other DLL errors on double-click)
The MSYS2 DLLs aren't bundled with the exe. Either launch from the MSYS2 terminal (step 9a) or bundle the DLLs (step 9b).

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

### Natron runs but is extremely slow (10x slower than expected)

Check your C++ optimization flags:
```bash
grep "CMAKE_CXX_FLAGS_RELWITHDEBINFO" build-qt6/CMakeCache.txt
```
If it shows an empty string, optimization was lost. Fix by reconfiguring:
```bash
cmake .. -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG"
mingw32-make clean && mingw32-make -j2
```
This can happen if you previously set custom `CMAKE_CXX_FLAGS_*` and then removed them — CMake caches the empty value.

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
| Eigen | 3.4.0 (bundled, upgraded from 3.3.7 for GCC 15 compat) |

---

## Technical Details

For a complete list of all code changes, API migrations, and architectural decisions,
see `MIGRATION_QT6_TRACKER.md` in the repository root.
