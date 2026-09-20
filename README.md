# Natron fork — 3D / Cycles / Deep / Particles / Volumes

[![GPL2 License](http://img.shields.io/:license-gpl2-blue.svg?)](LICENSE.txt)

A fork of [Natron](https://github.com/NatronGitHub/Natron), the open-source node-based compositor, that adds a 3D system, the Cycles renderer + Cycles deep, a deep-compositing pipeline, a particle system, volumes, and an early 3D camera tracker — **69 new built-in nodes** — on a Qt6 / Python 3.14 / OpenGL 4.3 base. Windows and Linux builds.

- **Download:** [Releases](https://github.com/Niik-l/Natron/releases) — Windows portable archive (numbered releases), Linux AppImage ([`linux-portable-latest`](https://github.com/Niik-l/Natron/releases/tag/linux-portable-latest), rebuilt from every green CI run); no installer either way
- **Node list:** [wiki](https://github.com/Niik-l/Natron/wiki) · developer notes in [NODE_REGISTRY.md](NODE_REGISTRY.md)
- **Bugs / ideas:** [Issues](https://github.com/Niik-l/Natron/issues)
- **Build it yourself:** [BUILDING.md](BUILDING.md) · change history in [DEV_3D_SYSTEM_CHANGELOG.md](DEV_3D_SYSTEM_CHANGELOG.md)

## A disclaimer

This project is 100% vibe coded. I don't have a background in computer science or programming, and I don't consider myself a TD. I'm an artist who wanted a free way to clean up and grade HDRIs, and it grew from there. Expect rough edges, and please report them.

If you've found this fork useful, consider buying me a beer :) **[Ko-fi](https://ko-fi.com/niikl)** · **[PayPal](https://paypal.me/niikl)**

## What the fork adds

| Area | Nodes | Highlights |
|---|---|---|
| 3D scene | 9 geometry + 9 scene/render + 4 camera/light + 3 material | 3D viewport with gizmos, Card / Sphere / Cube / Cylinder, ReadGeo (.obj / .abc), Alembic archives and cameras, **GeoBuilder** (draw grids over the plate, projected through the camera), Project3D / UVProject / MergeMat, PBR Material3D with texture maps, Megascans loader |
| Cycles | in the scene/render set | Blender's Cycles path tracer as a node: materials, lights with **light-group AOVs**, HDRI domes, motion blur, DOF, AOV passes, shadow catcher and holdout, **CyclesRenderPass** that renders passes to versioned multi-layer EXR (deep EXR too) and builds the comp tree |
| ScanlineRender | in the scene/render set | GLSL scanline renderer with AOVs and particle modes, for fast previews |
| Deep compositing | 17 (+19 unreleased) | DeepRead / Write / Merge / Recolor / Flatten / Reformat / Expression …, deep to point cloud and back (**Blast** editing), Cycles native deep output |
| Particles | 18 | Emitter, solver, forces, spawn, collisions, ParticleMaterial, instancing on geo, rendered in ScanlineRender and Cycles |
| Volumes | in the geometry/render set | ReadVDB (fire / smoke), procedural Volume3D, **FastVolumeRender** real-time GPU preview |
| Matchmove | 2 | CameraTracker (detect / track / solve) and PointCloudGenerator |
| Colour + transform | 5 | GPU OCIO ACES views in the viewer (tested on the ACES 2.0 reference config), ColorChartMatch, SphericalTransform (8 projections), LensWarp |

Templates menu: ready-made graphs for a basic 3D scene, the Megascans loader, Fast Volume Render, particle presets, and an HDRI face-edit rig.

**Underlying upgrades:** Qt5 → Qt6 + PySide6, Python 3.14, OpenGL 2.0 → 4.3 (compatibility profile), Eigen 3.4, C++17.

## Requirements

- Windows 10 or 11, 64-bit; or 64-bit Linux with glibc 2.28 or newer (any distribution from 2018 on — verified on Rocky 8/9, AlmaLinux 9, Fedora, Ubuntu 22.04/24.04, Debian 12, Arch and openSUSE Leap 15.6). The OpenGL 4.3 requirement rules out macOS.
- A GPU with OpenGL 4.3 for the 3D viewport, ScanlineRender and FastVolumeRender; FastVolumeRender additionally needs a Vulkan driver.
- Cycles renders on the CPU in the shipped builds.

## Installing

**Windows.** Download the `.7z` from the [Releases page](https://github.com/Niik-l/Natron/releases), extract it anywhere, and run `bin/Natron.exe`. The archive contains Natron, the bundled OpenFX plugins (openfx-io, openfx-misc, openfx-arena, openfx-gmic) and `NatronRenderer.exe` for command-line rendering. Nothing is written outside the folder except Natron's own settings.

**Linux.** Download the AppImage from the [`linux-portable-latest`](https://github.com/Niik-l/Natron/releases/tag/linux-portable-latest) pre-release, make it executable and run it:

```sh
chmod +x Natron-portable-rocky8-RelWithDebInfo-x86_64.AppImage
./Natron-portable-rocky8-RelWithDebInfo-x86_64.AppImage
```

It bundles Qt, Python, OpenImageIO/OpenColorIO, OpenVDB, Cycles, FFmpeg (with x264/x265) and the openfx-io and openfx-misc plugins; openfx-arena and openfx-gmic are not built for Linux yet. It needs only what a desktop provides (X11, Mesa or a vendor GL driver, fontconfig). On a Wayland desktop it runs through XWayland. Inside a container without FUSE, add `--appimage-extract-and-run`. `NatronRenderer` for command-line rendering is inside the AppImage: `./Natron-*.AppImage --appimage-extract` and use `squashfs-root/usr/bin/NatronRenderer`. The pre-release is rewritten by every green CI run and its notes name the exact commit; a matching debug-symbols tarball is attached for crash backtraces.

Verified on Rocky 8/9, AlmaLinux 9, Fedora, Ubuntu 22.04/24.04, Debian 12, Arch and openSUSE Leap 15.6 (CI, software GL) and on an NVIDIA RTX 4090 (GUI, 3D viewport, FastVolumeRender on Vulkan, Cycles). Not yet tried: a native Wayland session, AMD or Intel GPUs. NixOS needs its AppImage wrapper; Alpine (musl) is not supported.

**Optional, but recommended:**

- **An ACES OCIO config.** The archive bundles Natron's classic configs (nuke-default, blender, natron, spi, aces 0.x). For a modern ACES pipeline — the one this fork is tested on — download the ACES 2.0 studio config from the [OpenColorIO-Config-ACES releases](https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/releases) and point Natron at it: Preferences → Color → OpenColorIO config → Custom, then choose the `.ocio` file. (Setting the `OCIO` environment variable works too, but it also affects other apps such as Blender.)
- **OpenRV** for the *Open in RV* buttons on Write and CyclesRenderPass. Get a build from the [OpenRV releases](https://github.com/AcademySoftwareFoundation/OpenRV/releases), then either set the `NATRON_RV_PATH` environment variable to the executable (`rv.exe` on Windows, `rv` on Linux) or fill in the RV Executable knob on the node. On Linux the OpenRV project publishes no binaries: build it from source (Rocky 8/9 are the supported targets) or use a distribution package, then point `NATRON_RV_PATH` at the resulting `rv`. The buttons themselves are platform-neutral.

To build from source, see [BUILDING.md](BUILDING.md) (Windows: MSYS2 / MinGW with an automated script set under `tools/win-build/`; Linux: the GitHub Actions workflows, documented in the same file).

## Contributing

Bugs and feature requests go to this fork's [issue tracker](https://github.com/Niik-l/Natron/issues) — the forms ask for the build version, the area involved and your OCIO config, which is usually what's needed to reproduce a report. Bugs that also happen in official Natron belong [upstream](https://github.com/NatronGitHub/Natron/issues).

Development happens on `RB-2.6`; see [GIT_WORKFLOW.md](GIT_WORKFLOW.md) for the branch and commit conventions. Pull requests are welcome against `RB-2.6`.

There's a `.git-hooks` directory in the root with a `pre-commit` hook that checks code style (`astyle`):

```shell
cd Natron
mkdir .git/hooks
ln -s ../../.git-hooks/pre-commit .git/hooks/pre-commit
```

---

## About Natron

Natron is a free, open-source (GPLv2) video compositor, similar in functionality to Adobe After Effects, Foundry's Nuke, or Blackmagic Fusion. This fork is based on the NatronGitHub project; everything below is theirs.

- Website: https://natrongithub.github.io
- Source code: https://github.com/NatronGitHub/Natron
- Forum: https://discuss.pixls.us/c/software/natron
- Discord: https://discord.gg/cpMj5p3Fv5
- User documentation: https://natron.readthedocs.io/

### Features

- 32-bit floating-point linear color processing pipeline.
- Color management handled by [OpenColorIO](https://opencolorio.org/).
- Dozens of video and image formats supported such as: H264, DNxHR, EXR, DPX, TIFF, JPG, PNG through [OpenImageIO](https://github.com/OpenImageIO/oiio) and [FFmpeg](https://ffmpeg.org/).
- Support for many free, open-source, and commercial OpenFX plugins—currently almost all features of OpenFX v1.4 are supported.
  - [OpenFX-IO](https://github.com/NatronGitHub/openfx-io), [OpenFX-Misc](https://github.com/NatronGitHub/openfx-misc), [OpenFX-G'MIC](https://github.com/NatronGitHub/openfx-gmic), [OpenFX-Arena](https://github.com/NatronGitHub/openfx-arena) (all bundled in the releases)
  - [All OFX products from RevisionFX](http://www.revisionfx.com), [Boris FX](https://borisfx.com/) OpenFX plugins including Sapphire, [Furnace by The Foundry](http://www.thefoundry.co.uk/products/furnace/), and many more.
- Intuitive user interface: Natron aims not to break habits by providing an intuitive and familiar user interface. It is possible to customize and separate the graphical user interface on any number of screens. You can re-use your layouts and share your layout files (.nl).
- Performance: In Natron, anything you do produces real-time feedback in the viewer thanks to the optimized multi-threaded rendering pipeline and support for proxy rendering (computing at a lower resolution to speed up rendering).
- Multi-task: Natron can render multiple graphs at the same time. It can also be used as a background process in headless mode.
- Recover easily from bugs: Natron's auto-save system detects inactivity and saves your work for yourself. Natron is also able to render frames in a separate process, meaning that any crash in the main application would not crash the ongoing render (and the other way around).
- Project files saved in XML and easily editable by humans.
- Fast & interactive viewer - Smooth & accurate zooming/panning even for very large image sizes (tested on 27k x 30k images).
- Real-time playback: Natron offers real-time playback with excellent performance thanks to its RAM/Disk cache. Once a frame is rendered it can be reproduced instantly afterward, even for large image sizes.
- Animate your visual effects: Natron offers a simple and efficient way to deal with keyframes with a very accurate and intuitive Curve Editor as well as a Dope Sheet to quickly edit your motion graphics.
- Command-line rendering: Natron is capable of running without a GUI for batch rendering with scripts or on a render farm.
- Rotoscoping, rotopainting, and tracking support
- Multi-view workflow: Natron saves time by keeping all the views in the same stream. You can separate the views at any time with the OneView node.
- Python scripting integration: parameter expressions, user-defined parameters, node groups as Python scripts, a script editor, Python callbacks on internal events, and PySide integration so the interface is extensible with new menus and windows.
- Multi-channel compositing: Natron can manipulate multi-layered EXR files thanks to OpenImageIO. Users can choose to work with any layer or channel on any node, new custom layers can also be created.
