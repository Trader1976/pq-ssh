#!/usr/bin/env bash
set -euo pipefail

# scripts/build-appimage-12.sh
#
# Builds pq-ssh (Release) against bundled libssh 0.12 (private prefix),
# then packages it as an AppImage using linuxdeploy.
#
# Requirements:
#   - tools/linuxdeploy-x86_64.AppImage   (download manually into ./tools/)
#   - Your libssh 0.12 install prefix (default below):
#       /home/timo/opt/libssh-0.12
#
# Expected repo layout bits:
#   packaging/pq-ssh.desktop
#   packaging/icons/hicolor/256x256/apps/pq-ssh.png   (preferred)
#     OR
#   packaging/icons/pq-ssh.png                        (fallback)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

APP="pq-ssh"
APP_DISPLAY="CPUNK PQ-SSH"
ARCH="x86_64"

# ---- libssh 0.12 prefix (adjust if yours differs) ----
LIBSSH_PREFIX_DEFAULT="/home/timo/opt/libssh-0.12"
LIBSSH_PREFIX="${LIBSSH_PREFIX:-$LIBSSH_PREFIX_DEFAULT}"

# Make pkg-config prefer libssh 0.12 from the prefix
export PKG_CONFIG_PATH="${LIBSSH_PREFIX}/lib/pkgconfig:${LIBSSH_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

LINUXDEPLOY="${ROOT_DIR}/tools/linuxdeploy-x86_64.AppImage"
if [[ ! -f "${LINUXDEPLOY}" ]]; then
  echo "Missing: ${LINUXDEPLOY}"
  echo "Put linuxdeploy-x86_64.AppImage into ./tools/"
  exit 1
fi

chmod +x "${LINUXDEPLOY}" || true

# Dedicated build dir for libssh 0.12 build
BUILD_DIR="${ROOT_DIR}/cmake-build-release-libssh012"
APPDIR="${ROOT_DIR}/AppDir"
DIST_DIR="${ROOT_DIR}/dist"

DESKTOP_SRC="${ROOT_DIR}/packaging/${APP}.desktop"

mkdir -p "${DIST_DIR}"

echo "[0/7] Clean packaging outputs (prevent stale AppImage reuse)"
rm -rf "${APPDIR}"
rm -f "${ROOT_DIR}"/*.AppImage
rm -f "${DIST_DIR}"/*.AppImage
rm -rf "${ROOT_DIR}/squashfs-root"

echo "[0.5/7] Sanity-check libssh 0.12 prefix"
if [[ ! -d "${LIBSSH_PREFIX}" ]]; then
  echo "ERROR: LIBSSH_PREFIX does not exist: ${LIBSSH_PREFIX}"
  echo "Set env var LIBSSH_PREFIX or edit the script."
  exit 1
fi

if [[ ! -f "${DESKTOP_SRC}" ]]; then
  echo "ERROR: Desktop file not found: ${DESKTOP_SRC}"
  exit 1
fi

echo "[1/7] Configure + build (Release, libssh 0.12) -> ${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${LIBSSH_PREFIX}" \
  -DPQSSH_LIBSSH_PREFIX="${LIBSSH_PREFIX}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"

BIN_SRC="${BUILD_DIR}/bin/${APP}"
if [[ ! -f "${BIN_SRC}" ]]; then
  echo "ERROR: Built binary not found: ${BIN_SRC}"
  echo "Check your CMake output path."
  exit 1
fi

echo
echo "[INFO] Detect application version from binary"
APP_VERSION="$("${BIN_SRC}" --version 2>/dev/null | head -n1 | tr -d '\r')"
if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="$(git describe --tags --always 2>/dev/null || echo 1.0)"
fi
APP_VERSION="${APP_VERSION// /-}"
APP_VERSION="${APP_VERSION//\//-}"
echo "[INFO] APP_VERSION=${APP_VERSION}"

echo
echo "[VERIFY] Release binary:"
sha256sum "${BIN_SRC}"
echo

echo "[VERIFY] Linked libssh (Release binary):"
ldd "${BIN_SRC}" | grep -i ssh || true
echo

echo "[VERIFY] RUNPATH/RPATH (Release binary):"
readelf -d "${BIN_SRC}" | grep -E 'RPATH|RUNPATH' || true
echo

echo "[2/7] Prepare AppDir -> ${APPDIR}"
mkdir -p "${APPDIR}/usr/bin"
mkdir -p "${APPDIR}/usr/share/applications"

cp -f "${BIN_SRC}" "${APPDIR}/usr/bin/${APP}"
cp -f "${DESKTOP_SRC}" "${APPDIR}/usr/share/applications/${APP}.desktop"

echo
echo "[VERIFY] AppDir binary (must match Release above):"
sha256sum "${APPDIR}/usr/bin/${APP}"
echo

REL_HASH="$(sha256sum "${BIN_SRC}" | awk '{print $1}')"
APPDIR_HASH="$(sha256sum "${APPDIR}/usr/bin/${APP}" | awk '{print $1}')"
if [[ "${REL_HASH}" != "${APPDIR_HASH}" ]]; then
  echo "ERROR: Release binary hash != AppDir binary hash"
  echo "  Release: ${REL_HASH}"
  echo "  AppDir : ${APPDIR_HASH}"
  exit 1
fi

echo "[3/7] Install icon"
ICON_SRC=""
if [[ -f "${ROOT_DIR}/packaging/icons/hicolor/256x256/apps/${APP}.png" ]]; then
  ICON_SRC="${ROOT_DIR}/packaging/icons/hicolor/256x256/apps/${APP}.png"
elif [[ -f "${ROOT_DIR}/packaging/icons/${APP}.png" ]]; then
  ICON_SRC="${ROOT_DIR}/packaging/icons/${APP}.png"
else
  echo "ERROR: Icon not found."
  echo "Expected one of:"
  echo "  packaging/icons/hicolor/256x256/apps/${APP}.png"
  echo "  packaging/icons/${APP}.png"
  exit 1
fi

mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"
cp -f "${ICON_SRC}" "${APPDIR}/usr/share/icons/hicolor/256x256/apps/${APP}.png"
cp -f "${ICON_SRC}" "${APPDIR}/${APP}.png" || true

echo "[4/7] Run linuxdeploy (cwd: ${ROOT_DIR})"
"${LINUXDEPLOY}" \
  --appdir "${APPDIR}" \
  --desktop-file "${APPDIR}/usr/share/applications/${APP}.desktop" \
  --icon-file "${APPDIR}/${APP}.png" \
  --output appimage

echo "[5/7] Move AppImage to dist/"
OUT_FILE=""
for f in "${ROOT_DIR}/${APP}-"*.AppImage "${ROOT_DIR}"/*.AppImage; do
  if [[ -f "$f" ]]; then
    OUT_FILE="$f"
    break
  fi
done

if [[ -z "${OUT_FILE}" ]]; then
  echo "ERROR: linuxdeploy did not produce an AppImage in repo root."
  echo "Check linuxdeploy output above."
  exit 1
fi

FINAL_NAME="${APP_DISPLAY// /-}-${APP_VERSION}-${ARCH}.AppImage"
FINAL_PATH="${DIST_DIR}/${FINAL_NAME}"
mv -f "${OUT_FILE}" "${FINAL_PATH}"

echo
echo "✅ AppImage created:"
echo "  ${FINAL_PATH}"
echo

echo "[6/7] Verify AppImage contains the same pq-ssh binary (NOTE: may differ after deployment patching)"
"${FINAL_PATH}" --appimage-extract >/dev/null

echo "[VERIFY] Extracted AppImage binary:"
sha256sum "squashfs-root/usr/bin/${APP}"
echo

echo "[VERIFY] Linked libssh inside extracted AppImage:"
ldd "squashfs-root/usr/bin/${APP}" | grep -i ssh || true
echo

echo "[VERIFY] RPATH/RUNPATH inside extracted AppImage:"
readelf -d "squashfs-root/usr/bin/${APP}" | grep -E 'RPATH|RUNPATH' || true
echo

rm -rf squashfs-root

echo "[7/7] Enforce: AppImage must contain and use libssh from payload (not system)"
"${FINAL_PATH}" --appimage-extract >/dev/null

PAYLOAD_LIBSSH="$(find squashfs-root/usr -maxdepth 3 -type f -name 'libssh.so*' | head -n1 || true)"
if [[ -z "${PAYLOAD_LIBSSH}" ]]; then
  echo "ERROR: No libssh.so* found inside AppImage payload."
  rm -rf squashfs-root
  exit 1
fi

echo "[VERIFY] Found payload libssh:"
echo "  ${PAYLOAD_LIBSSH}"
echo

echo "[VERIFY] Payload libssh version string:"
strings "${PAYLOAD_LIBSSH}" | grep -i '^libssh ' || true
echo

echo "[VERIFY] pq-ssh links to:"
ldd "squashfs-root/usr/bin/${APP}" | grep -i libssh || true

if ! ldd "squashfs-root/usr/bin/${APP}" | grep -qi "squashfs-root/usr/.*libssh"; then
  echo "ERROR: Extracted pq-ssh does NOT resolve libssh from payload."
  echo "It may still be pulling system libssh at runtime."
  rm -rf squashfs-root
  exit 1
fi

rm -rf squashfs-root

echo
echo "✅ Build OK (Release+libssh0.12) and AppImage payload contains libssh from the bundle."
echo
echo "Release binary hash (for reference): ${REL_HASH}"
echo
echo "Test:"
echo "  ${FINAL_PATH} --help"
echo "  ${FINAL_PATH}"
echo
echo "Tip: override libssh prefix:"
echo "  LIBSSH_PREFIX=/some/where ./scripts/build-appimage-12.sh"