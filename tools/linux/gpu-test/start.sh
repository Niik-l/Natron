#!/bin/bash
# Container entry point: an Xfce desktop on TurboVNC, served to the browser
# through noVNC on port 6080. Then keeps running so the pod stays up.
set -eo pipefail

GEOMETRY=${GEOMETRY:-1920x1080}
export PATH=/opt/TurboVNC/bin:/opt/VirtualGL/bin:$PATH

echo "== GPU as the container sees it"
nvidia-smi -L 2>/dev/null || echo "nvidia-smi not available: is the pod started with --gpus / the NVIDIA runtime?"
echo "== Vulkan ICDs"
ls /etc/vulkan/icd.d/ /usr/share/vulkan/icd.d/ 2>/dev/null || true

# No password: the pod's port is only reachable through the provider's proxy
# (RunPod) or the port you chose to expose (Vast); add -securitytypes VNC if
# you expose it publicly.
vncserver :1 -geometry "$GEOMETRY" -depth 24 -securitytypes None -wm xfce -fg &
sleep 3
websockify --web /usr/share/novnc 6080 localhost:5901 &

cat <<EOF

Desktop: open the pod's port 6080 in a browser (RunPod: "Connect" -> HTTP 6080;
Vast: the mapped port), then vnc.html -> Connect.

In a terminal on that desktop:
  gh auth login                # device flow, once
  gh run download <run-id> -R Niik-l/Natron -n Natron-portable-rocky8-RelWithDebInfo-AppImage
  chmod +x Natron-*.AppImage
  vglrun -d egl ./Natron-*.AppImage --appimage-extract-and-run

vglrun -d egl routes Natron's OpenGL to the NVIDIA GPU; without it the
viewports run on Mesa software GL. --appimage-extract-and-run is needed
because containers have no FUSE.
EOF

wait
