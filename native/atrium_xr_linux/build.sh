#!/usr/bin/env bash
# The Atrium Runtime for Linux: Atrium's build of Monado (BSL-1.0), pinned upstream plus the
# patches in patches/, the same pattern as Atrium OVR on Revive.
#
# What it brings: Monado's Rift CV1/DK2 driver (headset, Touch over the radio), the Rift sensor
# cameras and constellation (LED) tracking for full 6DoF, Rift S, the other headsets Monado
# drives, and its compositor, which takes the headset display directly (no NVIDIA permission
# needed on Linux). SteamVR pieces are left out.
#
# Output (native/out/linux/):
#   libopenxr_atrium.so   the OpenXR runtime games load
#   atrium-service        the headset service (compositor + drivers); the Atrium app runs it
#   atrium_openxr.json    the runtime manifest
#
# Usage: native/atrium_xr_linux/build.sh [build-dir]   (default: ~/.cache/atrium-build)
# Build deps (Ubuntu/Debian): see README in this folder.
set -euo pipefail

MONADO_URL=https://gitlab.freedesktop.org/monado/monado.git
MONADO_COMMIT=5bca1effb30aed2ce68d805cddff1bc87b67005e   # main, 2026-10-20

# Under WSL, PATH carries every Windows folder (/mnt/c/...); CMake's package search walks PATH
# and crawls them over WSL's slow file bridge for minutes per lookup. Linux folders only.
PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd: -)"
export PATH

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../out/linux"
WORK="${1:-$HOME/.cache/atrium-build}"
SRC="$WORK/monado"
BUILD="$WORK/build"

mkdir -p "$WORK" "$OUT"

# Pinned source, patched. Re-applied from a clean tree each run so edits to patches/ take effect.
if [ ! -d "$SRC/.git" ]; then
    git clone --quiet "$MONADO_URL" "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$MONADO_COMMIT" 2>/dev/null || true
git -C "$SRC" checkout --quiet --force "$MONADO_COMMIT"
git -C "$SRC" clean --quiet -fdx
for p in "$HERE"/patches/*.patch; do
    git -C "$SRC" apply --whitespace=nowarn "$p"
done

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DXRT_OXR_RUNTIME_SUFFIX=atrium \
    -DXRT_IPC_MSG_SOCK_FILENAME=atrium_comp_ipc \
    -DXRT_FEATURE_SERVICE=ON \
    -DXRT_FEATURE_SERVICE_SYSTEMD=OFF \
    -DXRT_FEATURE_STEAMVR_PLUGIN=OFF \
    -DXRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE=OFF \
    -DXRT_MODULE_CONSTELLATION_TRACKING=ON \
    -DXRT_BUILD_DRIVER_RIFT=ON \
    -DXRT_BUILD_DRIVER_RIFT_SENSOR=ON \
    -DXRT_BUILD_DRIVER_RIFT_S=ON
cmake --build "$BUILD"

# Stage: the runtime, the service under Atrium's name, and a manifest next to them.
cp -f "$BUILD/src/xrt/targets/openxr/libopenxr_atrium.so" "$OUT/"
cp -f "$BUILD/src/xrt/targets/service/monado-service" "$OUT/atrium-service"
cat > "$OUT/atrium_openxr.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Atrium",
        "library_path": "./libopenxr_atrium.so"
    }
}
JSON
echo "Built: $OUT"
