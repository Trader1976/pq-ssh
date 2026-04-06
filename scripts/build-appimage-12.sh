#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

APP="pq-ssh"
APP_DISPLAY="CPUNK PQ-SSH"
ARCH="x86_64"

LIBSSH_PREFIX_DEFAULT="/home/timo/opt/libssh-0.12"
LIBSSH_PREFIX="${LIBSSH_PREFIX:-$LIBSSH_PREFIX_DEFAULT}"

export PKG_CONFIG_PATH="${LIBSSH_PREFIX}/lib/pkgconfig:${LIBSSH_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

LINUXDEPLOY="${ROOT_DIR}/tools/linuxdeploy-x86_64.AppImage"
if [[ ! -f "${LINUXDEPLOY}" ]]; then
  echo "Missing: ${LINUXDEPLOY}"
  exit 1
fi

chmod +x "${LINUXDEPLOY}" || true

BUILD_DIR="${ROOT_DIR}/cmake-build-release-libssh012"
APPDIR="${ROOT_DIR}/AppDir"
DIST_DIR="${ROOT_DIR}/dist"
DESKTOP_SRC="${ROOT_DIR}/packaging/${APP}.desktop"

mkdir -p "${DIST_DIR}"

echo "[0/7] Clean packaging outputs"
rm -rf "${APPDIR}"
rm -f "${ROOT_DIR}"/*.AppImage
rm -f "${DIST_DIR}"/*.AppImage
rm -rf "${ROOT_DIR}/squashfs-root"

echo "[0.5/7] Sanity-check libssh 0.12 prefix"
if [[ ! -d "${LIBSSH_PREFIX}" ]]; then
  echo "ERROR: LIBSSH_PREFIX does not exist: ${LIBSSH_PREFIX}"
  exit 1
fi

if [[ ! -f "${DESKTOP_SRC}" ]]; then
  echo "ERROR: Desktop file not found: ${DESKTOP_SRC}"
  exit 1
fi

echo "[1/7] Configure + build"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${LIBSSH_PREFIX}" \
  -DPQSSH_LIBSSH_PREFIX="${LIBSSH_PREFIX}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"

BIN_SRC="${BUILD_DIR}/bin/${APP}"
if [[ ! -f "${BIN_SRC}" ]]; then
  echo "ERROR: Built binary not found: ${BIN_SRC}"
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

echo "[VERIFY] RUNPATH/RPATH:"
readelf -d "${BIN_SRC}" | grep -E 'RPATH|RUNPATH' || true
echo

echo "[2/7] Prepare AppDir"
mkdir -p "${APPDIR}/usr/bin"
mkdir -p "${APPDIR}/usr/share/applications"

cp -f "${BIN_SRC}" "${APPDIR}/usr/bin/${APP}"
cp -f "${DESKTOP_SRC}" "${APPDIR}/usr/share/applications/${APP}.desktop"

echo
echo "[VERIFY] AppDir binary:"
sha256sum "${APPDIR}/usr/bin/${APP}"
echo

REL_HASH="$(sha256sum "${BIN_SRC}" | awk '{print $1}')"
APPDIR_HASH="$(sha256sum "${APPDIR}/usr/bin/${APP}" | awk '{print $1}')"
if [[ "${REL_HASH}" != "${APPDIR_HASH}" ]]; then
  echo "ERROR: Release binary hash != AppDir binary hash"
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
  exit 1
fi

mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"
cp -f "${ICON_SRC}" "${APPDIR}/usr/share/icons/hicolor/256x256/apps/${APP}.png"
cp -f "${ICON_SRC}" "${APPDIR}/${APP}.png" || true

echo "[4/7] Run linuxdeploy"
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
  exit 1
fi

FINAL_NAME="${APP_DISPLAY// /-}-${APP_VERSION}-${ARCH}.AppImage"
FINAL_PATH="${DIST_DIR}/${FINAL_NAME}"
mv -f "${OUT_FILE}" "${FINAL_PATH}"

echo
echo "AppImage created:"
echo "  ${FINAL_PATH}"
echo

echo "[6/7] Verify extracted AppImage"
"${FINAL_PATH}" --appimage-extract >/dev/null

echo "[VERIFY] Extracted binary:"
sha256sum "squashfs-root/usr/bin/${APP}"
echo

echo "[VERIFY] Linked libssh inside extracted AppImage:"
ldd "squashfs-root/usr/bin/${APP}" | grep -i ssh || true
echo

echo "[VERIFY] RPATH/RUNPATH inside extracted AppImage:"
readelf -d "squashfs-root/usr/bin/${APP}" | grep -E 'RPATH|RUNPATH' || true
echo

rm -rf squashfs-root

echo "[7/7] Enforce bundled libssh"
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
  rm -rf squashfs-root
  exit 1
fi

rm -rf squashfs-root

echo
echo "Build OK."
echo "Release binary hash: ${REL_HASH}"
echo "AppImage: ${FINAL_PATH}"