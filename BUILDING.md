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
  mingw-w64-x86_64-python-qtpy \
  mingw-w64-x86_64-boost \
  mingw-w64-x86_64-cairo \
  mingw-w64-x86_64-expat \
  mingw-w64-x86_64-openvdb

# For OFX plugins (Read/Write/Blur nodes)
pacman -S --noconfirm \
  mingw-w64-x86_64-openimageio \
  mingw-w64-x86_64-openexr \
  mingw-w64-x86_64-opencolorio \
  mingw-w64-x86_64-ffmpeg \
  mingw-w64-x86_64-libraw \
  mingw-w64-x86_64-libpng
```

> **Note:** `mingw-w64-x86_64-openvdb` is required by Cycles' VDB image loader.
> Without it, the Natron + Cycles link step fails with `undefined reference to
> ccl::VDBImageLoader::VDBImageLoader(...)`. Pull it in even if you don't plan
> on building Cycles immediately — installing it doesn't add runtime cost.

---

## 3. Clone the Repository

Pick a parent directory for your projects and stash it in `NATRON_ROOT` — the rest of this doc refers to `$NATRON_ROOT` rather than any hardcoded path. Add the `export` line to your `~/.bashrc` if you want it persistent across MSYS2 sessions.

```bash
# Wherever you keep source trees — adjust to your preference.
# Examples: /d/projects   /c/Users/$USER/code   /e/dev   ~/projects
export NATRON_ROOT=/d/projects
mkdir -p $NATRON_ROOT
cd $NATRON_ROOT

# Clone Natron
git clone https://github.com/Niik-l/Natron.git
cd Natron
git checkout RB-2.6

# Initialize submodules (OpenFX, SequenceParsing, google-test, etc.)
git submodule update --init --recursive
```

---

## 4. Configure with CMake

> **Build order tip.** If you want Cycles, **build Cycles first** (§10) and then
> configure Natron once with `-DNATRON_CYCLES=ON` (lines shown below). Doing
> Natron first then re-running `cmake ..` with Cycles on later forces an
> AUTOMOC cache reset (`rm -rf Engine/NatronEngine_autogen`) — avoidable.

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
  # -DNATRON_CYCLES_DIR=$NATRON_ROOT/cycles \
  # -DNATRON_CYCLES_BUILD_DIR=$NATRON_ROOT/cycles/build
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

Two executables are produced:
- `build-qt6/App/Natron.exe` — interactive GUI
- `build-qt6/Renderer/NatronRenderer.exe` — CLI batch renderer (headless, no Qt window)

---

## 6. Build OFX Plugins (optional but recommended)

Without plugins, Natron has no Read/Write/Blur/Merge nodes. You need two plugin repos:

### openfx-misc (Blur, Merge, Transform, etc.)

```bash
cd $NATRON_ROOT  # same parent directory as Natron
git clone https://github.com/NatronGitHub/openfx-misc.git
cd openfx-misc
git submodule update --init --recursive

# Fetch the pinned CImg.h + patched inpaint.h required by the CImg.ofx
# target. The version pin (CIMGVERSION) + the dtschump/CImg URL live in
# openfx-misc's own CImg/Makefile — this fetch mechanism is upstream
# (NatronGitHub/openfx-misc), not anything our fork adds. Future openfx-misc
# clones will pick up newer pins automatically.
#
# Skipping this step makes `mingw32-make` exit with code 2 after `Misc.ofx`
# builds — and any community PyPlug that uses an `eu.cimg.*` / `net.sf.cimg.*`
# node (e.g. zDefocus) then fails at instantiation. Only safe to skip if
# you're NOT installing the community PyPlug pack in §7.5.
#
# CImg 2.9.9 (commit b33dcc8f9f1acf1f276ded92c04f8231f6c23fcd) is verified to
# build cleanly with GCC 15.2.0 — no extra compiler workaround needed.
(cd CImg && mingw32-make CImg.h)

mkdir build && cd build

cmake .. -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/mingw32-make.exe \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

mingw32-make -j2     # builds BOTH Misc.ofx (~170 MB) and CImg.ofx (~60 MB)
```

> **CMake error about CMAKE_SYSTEM_PROCESSOR?** If you get an error about empty `CMAKE_SYSTEM_PROCESSOR`,
> edit the plugin's `CMakeLists.txt` and quote it: change `${CMAKE_SYSTEM_PROCESSOR}` to `"${CMAKE_SYSTEM_PROCESSOR}"`.

> **CImg.h fetch fails or you get hundreds of `'cimg_library_suffixed' has not been declared` errors?**
> You're building against the wrong CImg version. The `mingw32-make CImg.h` step above pulls the exact commit `b33dcc8f9f1acf1f276ded92c04f8231f6c23fcd` (CImg 2.9.9) which openfx-misc requires; newer upstream CImg removed the private-namespace machinery this code depends on. If `make CImg.h` fails (e.g. curl TLS hiccup), download manually using the URLs in `CImg/Makefile` — never use CImg `master`.

> **`mingw32-make` exits with code 2 even when `Misc.ofx` built fine?**
> The CImg target is failing while Misc succeeded — usually because the
> CImg.h fetch above was skipped. Re-run the `mingw32-make CImg.h` step,
> then `mingw32-make -j2` again. Only treat this as harmless if you're
> intentionally skipping the community PyPlug pack (§7.5).

### openfx-io (Read, Write — EXR, PNG, FFmpeg, etc.)

```bash
cd $NATRON_ROOT
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

**1. OIIO 3.x — ImageCache returns shared_ptr** (`OIIO/ReadOIIO.cpp`). Three separate edits in the same file — easy to miss any one of them:

- **1a.** Member type:
```cpp
// Change: ImageCache* _cache;
// To:     std::shared_ptr<ImageCache> _cache;
```

- **1b.** Local at `:3148`:
```cpp
// Change: ImageCache* sharedcache = ImageCache::create(true);
// To:     std::shared_ptr<ImageCache> sharedcache = ImageCache::create(true);
```

- **1c.** Constructor initializer list:
```cpp
// Change: , _cache(NULL)
// To:     , _cache(nullptr)
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
NATRON_DIR="$NATRON_ROOT/Natron/build-qt6"
mkdir -p "$NATRON_DIR/Plugins/OFX/Natron/Misc.ofx.bundle/Contents/Win64"
mkdir -p "$NATRON_DIR/Plugins/OFX/Natron/CImg.ofx.bundle/Contents/Win64"
mkdir -p "$NATRON_DIR/Plugins/OFX/Natron/IO.ofx.bundle/Contents/Win64"

# Copy plugins
cp $NATRON_ROOT/openfx-misc/build/Misc.ofx \
   "$NATRON_DIR/Plugins/OFX/Natron/Misc.ofx.bundle/Contents/Win64/Misc.ofx"

cp $NATRON_ROOT/openfx-misc/build/CImg.ofx \
   "$NATRON_DIR/Plugins/OFX/Natron/CImg.ofx.bundle/Contents/Win64/CImg.ofx"

cp $NATRON_ROOT/openfx-io/build/IO.ofx \
   "$NATRON_DIR/Plugins/OFX/Natron/IO.ofx.bundle/Contents/Win64/IO.ofx"
```

> **Built-in PyPlugs:** Natron ships 10 built-in PyPlugs (AngleBlur, DropShadow, EdgeBlur, Fill, Glow, LightWrap, PIKColor, SplitAndJoin, ZMask, ZRemap) at `Gui/Resources/PyPlugs/`. The build's `App/CMakeLists.txt` includes a `POST_BUILD` step that copies them into `build-qt6/Plugins/PyPlugs/` automatically — no manual step needed.
>
> **The `'NoneType' object has no attribute 'setScriptName'` error** when instantiating a community PyPlug has two known causes:
> 1. **Built-in PyPlug missing** — community plugin called `createNode("fr.inria.<built-in name>")` and got `None`. Fixed automatically by the POST_BUILD step above; if it ever recurs, check `build-qt6/Plugins/PyPlugs/` for the source files.
> 2. **`CImg.ofx` missing** — community plugin called `createNode("eu.cimg.*" / "net.sf.cimg.*")` and got `None` because the CImg target was skipped in §6. Build CImg.ofx (the `mingw32-make CImg.h` step in §6) and install it via the `cp` line above.

---

## 7.5. Install Community PyPlugs (optional, ~296 nodes)

The [`NatronGitHub/natron-plugins`](https://github.com/NatronGitHub/natron-plugins) repo is the canonical community PyPlug collection. ~296 nodes across 18 categories (Lens flares, Edge tools, Mattes, Light wraps, etc.). Most VFX users expect these to be present.

> **Prerequisites:** `qtpy` (in §2's pacman list) and **`CImg.ofx`** (built + installed in §6 / §7). Many community PyPlugs — including `zDefocus` — depend on `eu.cimg.*` / `net.sf.cimg.*` nodes that ship in `CImg.ofx`. Without it those PyPlugs fail at instantiation with the misleading `'NoneType' object has no attribute 'setScriptName'` error (see §7 troubleshooting note).

```bash
# Clone alongside your Natron build
cd $NATRON_ROOT
git clone https://github.com/NatronGitHub/natron-plugins.git

# Copy into Natron's runtime plugin search path
mkdir -p Natron/build-qt6/Plugins/PyPlugs
cp -r natron-plugins/* Natron/build-qt6/Plugins/PyPlugs/
```

The plugins appear in the Tab menu under their respective category groups on next Natron launch. Requires `qtpy` (the Python Qt abstraction layer) to be installed — included in §2's pacman list.

---

## 8. Install OpenColorIO Configs (optional)

Natron uses OpenColorIO for color management. The default config presets (nuke-default, blender, natron) ship as a separate tarball. Without them, Natron launches fine — color knobs still work — but you'll see OCIO warnings and lose the named presets.

At runtime, Natron searches `<binary>/../Resources/OpenColorIO-Configs/` (see `Engine/Settings.cpp`). For a dev build run from `build-qt6/App/Natron.exe`, that resolves to `build-qt6/Resources/OpenColorIO-Configs/`.

```bash
cd $NATRON_ROOT/Natron
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
cd $NATRON_ROOT/Natron/build-qt6/App
export PATH="/c/msys64/mingw64/bin:$PATH"
./Natron.exe
```

### 9b. Bundle for standalone launch (double-click from Explorer)

After bundling, you can double-click `Natron.exe` from Windows Explorer without needing the MSYS2 terminal. Two ways to bundle the DLLs — **pick Option A if you want a lean bundle**, Option B if you want the easiest "just copy everything" approach.

#### Option A — Trace only required DLLs with `ntldd -R` (recommended, ~50-80 DLLs)

Walks Natron's actual dependency tree and copies only what's needed. Result is roughly 1/10 the size of Option B.

```bash
pacman -S mingw-w64-x86_64-ntldd
cd $NATRON_ROOT/Natron/build-qt6/App
ntldd -R Natron.exe | grep mingw64 | awk '{print $3}' | xargs -I {} cp {} .
```

Caveat: may miss DLLs loaded dynamically at runtime (Qt plugins, OCIO config plugins, OFX plugins). If launching fails with a missing-DLL error, copy the specific missing DLL by hand or fall back to Option B.

#### Option B — Copy all MSYS2 DLLs (brute-force, ~700 DLLs / ~1.5 GB)

Simplest and most reliable — no extra tooling, no risk of missing a dynamically-loaded dependency.

```bash
cd $NATRON_ROOT/Natron/build-qt6/App
cp /c/msys64/mingw64/bin/*.dll .
```

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

## 9c. Optional runtime environment variables

A few env vars are read at runtime — set them once in your shell profile or in Windows' permanent environment so Natron picks them up on every launch (including double-click from Explorer).

| Variable | Purpose | Example |
|----------|---------|---------|
| `OFX_PLUGIN_PATH` | Where Natron looks for OFX plugin bundles (`.ofx.bundle`). Use this if your plugins live outside the default `Plugins/OFX/Natron` install location. | `D:\my\OFX\Plugins` |
| `NATRON_RV_PATH` | Path to the RV / OpenRV executable. When set, every new WriteNode's "RV Executable" knob is pre-filled with this value, and the "Open in RV" button uses it as a runtime fallback when the knob is empty. | `C:\Program Files\RV\bin\rv.exe` |
| `OCIO` | Path to a `config.ocio` file. Overrides the bundled OpenColorIO config search (see step 8). | `C:\colorpipe\aces\config.ocio` |

### Setting them permanently on Windows

Option A — GUI: Start menu → "environment" → "Edit environment variables for your account" → **New...** under **User variables** → enter Name + Value → OK.

Option B — `cmd.exe` one-liner:
```cmd
setx NATRON_RV_PATH "C:\Program Files\RV\bin\rv.exe"
setx OFX_PLUGIN_PATH "D:\my\OFX\Plugins"
```

Option C — PowerShell:
```powershell
[System.Environment]::SetEnvironmentVariable("NATRON_RV_PATH", "C:\Program Files\RV\bin\rv.exe", "User")
```

All three persist into your Windows user profile registry; new processes inherit them. Close + reopen any already-running shells/Natron for the change to take effect.

### Setting them per-session (Linux/macOS or MSYS2)

```bash
export OFX_PLUGIN_PATH="/path/to/ofx/plugins"
export NATRON_RV_PATH="/path/to/rv"
./Natron
```

Add the `export` lines to your `~/.bashrc` / `~/.zshrc` to make them persistent across sessions.

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
cd $NATRON_ROOT
git clone https://github.com/blender/cycles.git
cd cycles
git checkout v5.0.0

# Apply MinGW compatibility patch (from Natron repo)
git apply $NATRON_ROOT/Natron/patches/cycles-mingw.patch

# Patch 1 — FindTBB.cmake doesn't recognize MSYS2's libtbb12. Without this,
# configure fails with "Could NOT find TBB (missing: TBB_LIBRARY)".
sed -i 's/NAMES tbb$/NAMES tbb tbb12/' src/cmake/Modules/FindTBB.cmake

# Patch 2 — MinGW doesn't expose M_PI from <cmath> unless _USE_MATH_DEFINES
# is set first. Otherwise cycles/src/subd/dice.cpp (via OpenSubdiv headers)
# fails with `'M_PI' was not declared in this scope`.
cat <<'EOF' >> CMakeLists.txt

# Niik-l fork: ensure MinGW exposes M_PI for OpenSubdiv-using sources.
if(WIN32)
  add_compile_definitions(_USE_MATH_DEFINES)
endif()
EOF

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
cd $NATRON_ROOT/Natron/build-qt6

cmake .. -G "MinGW Makefiles" \
  -DNATRON_CYCLES=ON \
  -DNATRON_CYCLES_DIR=$NATRON_ROOT/cycles \
  -DNATRON_CYCLES_BUILD_DIR=$NATRON_ROOT/cycles/build

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

### "Failed to import qtpy.QtCore" / "Failed to import qtpy.QtGui" at startup

The error log shows these on Natron launch. Upstream Natron does `import qtpy` during Python init — required for PyPlug scripting. Two preconditions:

1. **qtpy must be installed** — `pacman -S mingw-w64-x86_64-python-qtpy`. It lives in MSYS2's system site-packages, which Natron leaves on `sys.path` (only user site-packages are disabled).
2. **QT_API env var must match the Qt binding actually installed.** Our `AppManager::initPython()` already gates this on `QT_VERSION` so Qt6 builds get `pyside6` (Qt5 builds get `pyside2`). If you've inherited a pre-fix Natron build that hardcodes `pyside2` on Qt6, the symptom is this same error.

Quick verify after fix:
```bash
/c/msys64/mingw64/bin/python3.exe -c "import qtpy; print(qtpy.__version__)"
# → prints a version string (e.g. 2.4.x)
```

### Harmless build warning: `wmain` missing declaration

```
App/NatronApp_main.cpp:65:5: warning: no previous declaration for 'int wmain(int, wchar_t**)' [-Wmissing-declarations]
```

Cosmetic. `wmain` is the wide-char entry point; the warning is GCC asking for a forward declaration. Doesn't affect the build or runtime.

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
cp -r /c/msys64/mingw64/lib/python3.14 $NATRON_ROOT/Natron/build-qt6/lib/python3.14
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
