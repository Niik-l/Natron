# Rocky Linux 9 desktop for testing the portable Natron AppImage on a rented
# GPU (RunPod, Vast.ai or any host with the NVIDIA container toolkit).
# You get an Xfce desktop in the browser (noVNC) with the host's NVIDIA driver
# passed through, so Cycles (CUDA/OptiX), the GL viewports (via VirtualGL) and
# FastVolumeRender (Vulkan) all run for real. See README.md.
#
#   docker build -t natron-gpu-test tools/linux/gpu-test
#   docker run --gpus all -p 6080:6080 natron-gpu-test
#
# Untested as written on 2026-09-20: expect a package name or two to need
# adjusting on the first build.

FROM rockylinux/rockylinux:9

# The NVIDIA container toolkit only mounts the driver's GL/Vulkan/EGL
# libraries when these capabilities are requested; compute alone is not enough.
ENV NVIDIA_VISIBLE_DEVICES=all \
    NVIDIA_DRIVER_CAPABILITIES=all \
    LANG=C.UTF-8

RUN dnf -y install epel-release dnf-plugins-core \
 && dnf config-manager --set-enabled crb \
 && dnf -y groupinstall "Xfce" \
 && dnf -y install --setopt=install_weak_deps=False --allowerasing \
      xterm xorg-x11-fonts-misc dejavu-sans-fonts fontconfig \
      novnc python3-websockify \
      libglvnd-glx libglvnd-egl libglvnd-opengl mesa-libGL mesa-libEGL mesa-dri-drivers \
      vulkan-loader vulkan-tools mesa-vulkan-drivers \
      libxkbcommon-x11 xcb-util-wm xcb-util-image xcb-util-keysyms xcb-util-renderutil \
      xcb-util-cursor libSM libICE fribidi alsa-lib \
      fuse fuse-libs file binutils gdb procps-ng curl wget tar gzip unzip which \
 && dnf clean all

# TurboVNC (the X server the desktop runs on) and VirtualGL (routes Natron's
# OpenGL to the NVIDIA GPU through EGL, no GPU-attached X server needed).
RUN curl -fsSL https://raw.githubusercontent.com/TurboVNC/repo/main/TurboVNC.repo \
      -o /etc/yum.repos.d/TurboVNC.repo \
 && curl -fsSL https://raw.githubusercontent.com/VirtualGL/repo/main/VirtualGL.repo \
      -o /etc/yum.repos.d/VirtualGL.repo \
 && dnf -y install turbovnc virtualgl \
 && dnf clean all

# GitHub CLI, to download the AppImage artifact from a workflow run.
RUN dnf -y config-manager --add-repo https://cli.github.com/packages/rpm/gh-cli.repo \
 && dnf -y install gh && dnf clean all

RUN useradd -m -s /bin/bash tester && echo "tester ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/tester
COPY start.sh /usr/local/bin/start.sh
RUN chmod +x /usr/local/bin/start.sh

USER tester
WORKDIR /home/tester
EXPOSE 6080
CMD ["/usr/local/bin/start.sh"]
