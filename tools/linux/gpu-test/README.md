# Testing the portable AppImage on a rented GPU

CI proves the AppImage loads, resolves every library and opens its GUI on nine
distros, but only on Mesa's software renderers. What it cannot prove is the
GPU path: the GL viewports on a vendor driver and FastVolumeRender, which needs
Vulkan. (Cycles is CPU-only in every build we make, so it is not a GPU test.) An hour or two on a rented GPU covers
that for a few dollars, on the distro we target (Rocky 9 here; swap the base
image for Rocky 8, Ubuntu or Fedora to compare).

Status 2026-09-20: exercised on a RunPod RTX 4090 pod (Secure Cloud, $0.74/h). The
desktop, noVNC clipboard bridge, GPU pass-through (nvidia-smi, Vulkan lists the
4090, VirtualGL renderer = RTX 4090) and the AppImage download all worked first
time. Natron ran with GPU rendering enabled; the 2D viewer, OCIO views, the 3D
viewport with the camera gizmo, FastVolumeRender on real Vulkan (the procedural
cloud template at UHD) and a Cycles (CPU) sphere render all passed. Paste commands via the noVNC sidebar clipboard,
then Shift+Insert in the pod's terminal.

## Set-up

1. The image is built and pushed by the **GPU Test Image** workflow (Actions tab
   -> Run workflow) to `ghcr.io/niik-l/natron-gpu-test:latest`. Re-run it after
   editing the Dockerfile. (Or build locally: `docker build -t natron-gpu-test
   tools/linux/gpu-test`.)

2. Rent a pod with an RTX-class card (an RTX 3090/4090 is plenty). Roughly
   20 to 60 cents an hour on RunPod or Vast.ai as of 2026-09.
   - **RunPod**: create a template from `ghcr.io/niik-l/natron-gpu-test:latest`, expose HTTP port `6080`,
     environment `NVIDIA_DRIVER_CAPABILITIES=all`, no start command (the
     image's `start.sh` runs). Connect -> HTTP 6080 -> `vnc.html`.
   - **Vast.ai**: same image, map port 6080, `-e NVIDIA_DRIVER_CAPABILITIES=all`.
   - **Any Linux box with an NVIDIA card and the container toolkit**:
     `docker run --gpus all -p 6080:6080 ghcr.io/niik-l/natron-gpu-test:latest`, then open
     `http://localhost:6080/vnc.html`.

3. In a terminal on the desktop, fetch the AppImage of the current numbered release
   (the one with the Latest badge; no login needed):

       base=https://github.com/Niik-l/Natron/releases/latest/download
       curl -fLO $base/Natron-2.6-linux-x86_64.AppImage
       curl -fLO $base/Natron-2.6-linux-x86_64-debug-symbols.tar.gz
       chmod +x Natron-*.AppImage
       vglrun -d egl ./Natron-*.AppImage --appimage-extract-and-run

   (An unreleased CI build instead: `gh auth login`, then `gh run download <run-id>
   -R Niik-l/Natron -n Natron-2.6-linux-x86_64-AppImage`.)

   `vglrun -d egl` sends Natron's OpenGL to the NVIDIA GPU (the VNC X server
   has no GPU of its own); without it everything runs on Mesa software GL,
   which is still a useful comparison. `--appimage-extract-and-run` is needed
   because containers have no FUSE.

Before Natron, sanity-check the pass-through: `nvidia-smi`, `vulkaninfo --summary`
(should list the NVIDIA device, not only llvmpipe) and
`vglrun -d egl glxinfo -B` (renderer should say NVIDIA).

## Checklist

Tick each against the Windows build's behaviour; screenshot anything odd
(the desktop has a screenshot tool under Accessories).

| Area | What to do | Pass when |
|---|---|---|
| Startup | Help -> About | Commit and branch match the run's commit; links open the fork |
| 2D viewer | Read a JPEG and an EXR from `/usr/share` or a download; view RGB, R, alpha; zoom/pan | Image shows, colorspace guessed correctly, no GL errors in the terminal |
| OCIO | Viewer display/view dropdowns; switch between sRGB and ACES views | Display changes, no "could not be found" |
| 3D viewport | Create Camera3D + Card3D + a Sphere3D, open a 3D viewer, orbit, use the gizmo | Draws on the GPU (`nvidia-smi` shows Natron), gizmo drags |
| Cycles | RenderPass / CyclesRender with a light and the sphere | Converges, no crash; denoise (OIDN) works. **CPU only**: every build (Windows, Fedora, portable) configures Cycles with CUDA/OptiX/HIP/oneAPI off, so there is no device to switch. GPU Cycles would need the CUDA toolkit + OptiX SDK in the build image. |
| FastVolumeRender | ReadVDB a small .vdb -> FastVolumeRender with the camera | Renders (needs Vulkan; the terminal shows the adapter it picked) |
| Particles | ParticleEmitter -> ScanlineRender; play 50 frames | Plays without stalling |
| Deep | DeepRead a small deep EXR (or Cycles deep output) -> DeepToImage | Flattens correctly |
| Write | Write a PNG and an EXR to `/tmp`; read them back | Files valid, colorspace round-trips |
| PyPlugs / Python | Script editor: `app.createNode("net.sf.cimg.CImgBlur")`; Templates menu | Node appears; templates load |
| Stability | Leave it open 10 min with playback looping | No crash, memory stable |

If it crashes, rerun under gdb with the symbols next to the binary:

    ./Natron-*.AppImage --appimage-extract
    tar -xzf Natron-*-debug-symbols.tar.gz && cp debug/Natron.debug squashfs-root/usr/bin/
    APPDIR=$PWD/squashfs-root vglrun -d egl gdb -q -ex run -ex 'thread apply all bt 30' \
        --args squashfs-root/usr/bin/Natron

Stop the pod when done; billing is per minute of uptime, not usage.
