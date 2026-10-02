#!/usr/bin/env bash
#
# build_appimage.sh — build LinuxBench (Release) and package it as an
# AppImage for x86_64 Linux using linuxdeploy.
#
# Usage:  ./build_appimage.sh
# Output: LinuxBench-x86_64.AppImage (in the project root)
#         (linuxdeploy appends a version suffix if it can find one)
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths / constants
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

BUILD_DIR="${SCRIPT_DIR}/build"
APP_DIR="${SCRIPT_DIR}/AppDir"
APP_NAME="LinuxBench"
DESKTOP_FILE="${APP_NAME,,}.desktop"   # linuxbench.desktop
ICON_BASENAME="linuxbench"
LINUXDEPLOY_BIN="linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
JOBS="$(nproc 2>/dev/null || echo 4)"

log() { printf '\033[1;34m[appimage]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[appimage] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || die "cmake not found in PATH"
command -v wget  >/dev/null 2>&1 || die "wget not found in PATH"
command -v g++   >/dev/null 2>&1 || die "g++ (a C++ compiler) not found in PATH"

# ---------------------------------------------------------------------------
# 1. Build the CMake project (Release)
# ---------------------------------------------------------------------------
log "Configuring CMake build (Release)..."
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release

log "Building (Release, -j${JOBS})..."
cmake --build "${BUILD_DIR}" --config Release -j "${JOBS}"

BINARY="${BUILD_DIR}/${APP_NAME}"
[[ -f "${BINARY}" ]] || die "Expected binary not found: ${BINARY}"

# ---------------------------------------------------------------------------
# 2. Fetch linuxdeploy (if not already present)
# ---------------------------------------------------------------------------
if [[ ! -f "${SCRIPT_DIR}/${LINUXDEPLOY_BIN}" ]]; then
    log "Downloading linuxdeploy (${LINUXDEPLOY_URL})..."
    wget -O "${LINUXDEPLOY_BIN}" "${LINUXDEPLOY_URL}"
else
    log "linuxdeploy already present, skipping download."
fi
chmod +x "${SCRIPT_DIR}/${LINUXDEPLOY_BIN}"

# ---------------------------------------------------------------------------
# 3. Assemble the AppDir
# ---------------------------------------------------------------------------
log "Creating AppDir structure..."
rm -rf "${APP_DIR}"
mkdir -p \
    "${APP_DIR}/usr/bin" \
    "${APP_DIR}/usr/share/applications" \
    "${APP_DIR}/usr/share/icons/hicolor/256x256/apps"

log "Copying ${APP_NAME} binary into AppDir..."
cp "${BINARY}" "${APP_DIR}/usr/bin/${APP_NAME}"

# --- .desktop entry ---------------------------------------------------------
DESKTOP="${APP_DIR}/usr/share/applications/${DESKTOP_FILE}"
log "Writing ${DESKTOP_FILE}..."
cat > "${DESKTOP}" <<'EOF'
[Desktop Entry]
Version=1.0
Type=Application
Name=LinuxBench
Comment=Native CPU benchmark (Cinebench-style) for Linux
Exec=LinuxBench
Icon=linuxbench
Terminal=false
Categories=Utility;System;
Keywords=benchmark;cpu;raytracing;embree;
EOF

# --- Placeholder icon: blue circle with "LB" --------------------------------
ICON="${APP_DIR}/usr/share/icons/hicolor/256x256/apps/${ICON_BASENAME}.svg"
log "Writing placeholder icon..."
cat > "${ICON}" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <circle cx="128" cy="128" r="120" fill="#2d6cdf"/>
  <circle cx="128" cy="128" r="120" fill="none" stroke="#1b4a9e" stroke-width="8"/>
  <text x="128" y="128" text-anchor="middle" dominant-baseline="central"
        font-family="DejaVu Sans, Arial, sans-serif" font-size="96"
        font-weight="bold" fill="#ffffff">LB</text>
</svg>
EOF

# ---------------------------------------------------------------------------
# 4. Run linuxdeploy to produce the .AppImage
# ---------------------------------------------------------------------------
log "Running linuxdeploy..."
./"${LINUXDEPLOY_BIN}" --appdir "${APP_DIR}" --output appimage

log "Done. AppImage created in ${SCRIPT_DIR}:"
ls -lh "${SCRIPT_DIR}"/*.AppImage 2>/dev/null | grep -v linuxdeploy || true
