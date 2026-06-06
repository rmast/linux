#!/usr/bin/env bash
set -euo pipefail

BASE_COMMIT="3cb1fb7a56d2"
PKG_NAME="atomisp-hybrid"
OUT_DIR="./outgoing/atomisp-hybrid/dist"

usage() {
  cat <<'EOF'
Usage: export-atomisp-hybrid.sh [--base <commit>] [--name <pkgname>] [--out <dir>]

Defaults:
  --base 3cb1fb7a56d2
  --name atomisp-hybrid
  --out  ./outgoing/atomisp-hybrid/dist
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base)
      BASE_COMMIT="${2:-}"
      shift 2
      ;;
    --name)
      PKG_NAME="${2:-}"
      shift 2
      ;;
    --out)
      OUT_DIR="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Run this script inside your kernel git tree." >&2
  exit 1
fi

if ! git merge-base --is-ancestor "$BASE_COMMIT" HEAD; then
  echo "Base commit $BASE_COMMIT is not an ancestor of HEAD." >&2
  exit 1
fi

HEAD_SHORT="$(git rev-parse --short HEAD)"
STAMP="$(date +%Y%m%d)"
VERSION="${STAMP}git${HEAD_SHORT}"
PKG_DIR_NAME="${PKG_NAME}-${VERSION}"
PKG_DIR="${OUT_DIR}/${PKG_DIR_NAME}"

mkdir -p "$OUT_DIR"
rm -rf "$PKG_DIR"

echo "[1/6] Copy source files"
mkdir -p "$PKG_DIR/drivers/staging/media"
cp -a drivers/staging/media/atomisp "$PKG_DIR/drivers/staging/media/"

mkdir -p "$PKG_DIR/external/ipu-bridge"
cp -a drivers/media/pci/intel/ipu-bridge.c "$PKG_DIR/external/ipu-bridge/"

mkdir -p "$PKG_DIR/external/mt9m114"
cp -a drivers/media/i2c/mt9m114.c "$PKG_DIR/external/mt9m114/"
cp -a drivers/media/i2c/aptina-pll.h "$PKG_DIR/external/mt9m114/"

echo "[2/6] Add hybrid build glue"
cp -a outgoing/atomisp-hybrid/templates/source-root/Makefile "$PKG_DIR/Makefile"
cp -a outgoing/atomisp-hybrid/templates/source-root/dkms.conf "$PKG_DIR/dkms.conf"
cp -a outgoing/atomisp-hybrid/templates/source-root/external/ipu-bridge/Makefile \
  "$PKG_DIR/external/ipu-bridge/Makefile"
cp -a outgoing/atomisp-hybrid/templates/source-root/external/mt9m114/Makefile \
  "$PKG_DIR/external/mt9m114/Makefile"
cp -a COPYING "$PKG_DIR/COPYING"

sed -i \
  -e "s/#MODULE_NAME#/${PKG_NAME}/g" \
  -e "s/#MODULE_VERSION#/${VERSION}/g" \
  "$PKG_DIR/dkms.conf"

# Patch the atomisp Makefile for out-of-tree (DKMS/akmod) builds:
# 1. Replace the hardcoded srctree path with M-based path
# 2. Add pci/ and include/linux to ccflags so trace + local headers are found
ATOMISP_MK="$PKG_DIR/drivers/staging/media/atomisp/Makefile"
sed -i \
  -e 's|^atomisp = \$(srctree)/drivers/staging/media/atomisp/$|ifdef M\natomisp = $(M)/\nelse\natomisp = $(srctree)/drivers/staging/media/atomisp/\nendif|' \
  "$ATOMISP_MK"
sed -i \
  -e 's|^ccflags-y += \$(INCLUDES) \$(DEFINES)|ccflags-y += -I$(M)/pci -I$(M)/include/linux $(INCLUDES) $(DEFINES)|' \
  "$ATOMISP_MK"

echo "[3/6] Generate metadata"
cat > "$PKG_DIR/VERSION" <<EOF
$VERSION
EOF

cat > "$PKG_DIR/BASE_COMMIT" <<EOF
$BASE_COMMIT
EOF

cat > "$PKG_DIR/COMMITS.txt" <<EOF
$(git log --oneline "$BASE_COMMIT"..HEAD)
EOF

echo "[4/6] Generate patchset"
git format-patch --stdout "$BASE_COMMIT"..HEAD > "$OUT_DIR/${PKG_DIR_NAME}.patch"

echo "[5/6] Create source tarball"
tar -C "$OUT_DIR" -czf "$OUT_DIR/${PKG_DIR_NAME}.tar.gz" "$PKG_DIR_NAME"

echo "[6/6] Done"
cat <<EOF
Created:
  $PKG_DIR
  $OUT_DIR/${PKG_DIR_NAME}.tar.gz
  $OUT_DIR/${PKG_DIR_NAME}.patch

For DKMS:
  sudo cp -a "$PKG_DIR" "/usr/src/$PKG_DIR_NAME"
  sudo dkms add -m "$PKG_NAME" -v "$VERSION"
  sudo dkms build -m "$PKG_NAME" -v "$VERSION"
  sudo dkms install -m "$PKG_NAME" -v "$VERSION"

For akmod:
  Use the generated tarball as Source0 with outgoing/atomisp-hybrid/akmod/akmod-atomisp-hybrid.spec
EOF
