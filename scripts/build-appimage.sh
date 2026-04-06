#!/usr/bin/env bash
set -euo pipefail

# scripts/build-appimage.sh
#
# Builds pq-ssh (Release) and packages it as an AppImage using linuxdeploy.
#
# Requirements:
#   - tools/linuxdeploy-x86_64.AppImage   (download manually into ./tools/)
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

# If you want the AppImage filename to match your Qt app version, keep this in sync with main.cpp

ARCH="x86_64"

LINUXDEPLOY="${ROOT_DIR}/tools/linuxdeploy-x86_64.AppImage"
if [[ ! -f "${LINUXDEPLOY}" ]]; then
  echo "Missing: ${LINUXDEPLOY}"
  echo "Put linuxdeploy-x86_64.AppImage into ./tools/"
  exit 1
fi

# Make sure it's executable (some downloads lose +x)
chmod +x "${LINUXDEPLOY}" || true

BUILD_DIR="${ROOT_DIR}/cmake-build-release"
APPDIR="${ROOT_DIR}/AppDir"
DIST_DIR="${ROOT_DIR}/dist"

BIN_SRC="${BUILD_DIR}/bin/${APP}"
DESKTOP_SRC="${ROOT_DIR}/packaging/${APP}.desktop"

mkdir -p "${DIST_DIR}"

echo "[0/6] Clean packaging outputs (prevent stale AppImage reuse)"
rm -rf "${APPDIR}"
rm -f "${ROOT_DIR}"/*.AppImage
rm -f "${DIST_DIR}"/*.AppImage
rm -rf "${ROOT_DIR}/squashfs-root"

echo "[1/6] Configure + build (Release) → ${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[INFO] Detect application version from binary"

APP_VERSION="$("${BIN_SRC}" --version 2>/dev/null | head -n1 | tr -d '\r')"

# Fallback if --version prints nothing
if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="$(git describe --tags --always 2>/dev/null || echo 0.0.0)"
fi

# Sanitize for filenames
APP_VERSION="${APP_VERSION// /-}"
APP_VERSION="${APP_VERSION//\//-}"

echo "[INFO] APP_VERSION=${APP_VERSION}"

echo
echo "[VERIFY] Release binary:"
sha256sum "${BIN_SRC}"
echo

echo "[2/6] Prepare AppDir → ${APPDIR}"
mkdir -p "${APPDIR}/usr/bin"
mkdir -p "${APPDIR}/usr/share/applications"

# Hard-pin copy of the just-built binary
cp -f "${BIN_SRC}" "${APPDIR}/usr/bin/${APP}"
cp -f "${DESKTOP_SRC}" "${APPDIR}/usr/share/applications/${APP}.desktop"

echo
echo "[VERIFY] AppDir binary (must match Release above):"
sha256sum "${APPDIR}/usr/bin/${APP}"
echo

# Fail early if Release != AppDir (should never happen, but prevents ghosts)
REL_HASH="$(sha256sum "${BIN_SRC}" | awk '{print $1}')"
APPDIR_HASH="$(sha256sum "${APPDIR}/usr/bin/${APP}" | awk '{print $1}')"
if [[ "${REL_HASH}" != "${APPDIR_HASH}" ]]; then
  echo "ERROR: Release binary hash != AppDir binary hash"
  echo "  Release: ${REL_HASH}"
  echo "  AppDir : ${APPDIR_HASH}"
  exit 1
fi

# --- Icon handling (robust) ---
echo "[3/6] Install icon"

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

echo "[4/6] Run linuxdeploy (cwd: ${ROOT_DIR})"
"${LINUXDEPLOY}" \
  --appdir "${APPDIR}" \
  --desktop-file "${APPDIR}/usr/share/applications/${APP}.desktop" \
  --icon-file "${APPDIR}/${APP}.png" \
  --output appimage

echo "[5/6] Move AppImage to dist/"
OUT_FILE=""

# linuxdeploy usually outputs into CWD (repo root), named like pq-ssh-x86_64.AppImage
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

echo "[6/6] Verify AppImage contains the same pq-ssh binary"
"${FINAL_PATH}" --appimage-extract >/dev/null

echo "[VERIFY] Extracted AppImage binary:"
sha256sum squashfs-root/usr/bin/${APP}
echo

IMG_HASH="$(sha256sum squashfs-root/usr/bin/${APP} | awk '{print $1}')"
rm -rf squashfs-root

if [[ "${IMG_HASH}" != "${REL_HASH}" ]]; then
  echo "ERROR: AppImage binary hash != Release binary hash"
  echo "  Release : ${REL_HASH}"
  echo "  AppImage: ${IMG_HASH}"
  exit 1
fi

echo "✅ Verified: AppImage payload matches Release build."
echo
echo "Test:"
echo "  ${FINAL_PATH} --help"
