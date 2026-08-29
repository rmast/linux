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

inject_module_metadata() {
  local file="$1"
  local tmp

  if grep -q 'MODULE_INFO(atomisp_hybrid_describe' "$file"; then
    return
  fi

  tmp="$(mktemp)"
  awk -v version="$VERSION" -v commit="$HEAD_COMMIT" -v base_commit="$BASE_COMMIT" -v describe="$HEAD_DESCRIBE" '
    /MODULE_DESCRIPTION\(/ && !done {
      print
      print "MODULE_VERSION(\"" version "\");"
      print "MODULE_INFO(atomisp_hybrid_commit, \"" commit "\");"
      print "MODULE_INFO(atomisp_hybrid_base_commit, \"" base_commit "\");"
      print "MODULE_INFO(atomisp_hybrid_describe, \"" describe "\");"
      done = 1
      next
    }
    { print }
  ' "$file" > "$tmp"
  mv "$tmp" "$file"
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
HEAD_COMMIT="$(git rev-parse HEAD)"
HEAD_DESCRIBE="$(git describe --always --dirty --tags 2>/dev/null || git rev-parse --short HEAD)"
STAMP="$(date +%Y%m%d)"
HEAD_EPOCH="$(git show -s --format=%ct HEAD)"
# Use 'gitz' to sort newer than older 'git<hex>' builds, then compare numerically.
VERSION="${STAMP}gitz${HEAD_EPOCH}.${HEAD_SHORT}"
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

mkdir -p "$PKG_DIR/external/ov9728"
cp -a drivers/media/i2c/ov9728.c "$PKG_DIR/external/ov9728/"

echo "[2/6] Add hybrid build glue"
cp -a outgoing/atomisp-hybrid/templates/source-root/Makefile "$PKG_DIR/Makefile"
cp -a outgoing/atomisp-hybrid/templates/source-root/dkms.conf "$PKG_DIR/dkms.conf"
cp -a outgoing/atomisp-hybrid/templates/source-root/external/ipu-bridge/Makefile \
  "$PKG_DIR/external/ipu-bridge/Makefile"
cp -a outgoing/atomisp-hybrid/templates/source-root/external/mt9m114/Makefile \
  "$PKG_DIR/external/mt9m114/Makefile"
cp -a outgoing/atomisp-hybrid/templates/source-root/external/ov9728/Makefile \
  "$PKG_DIR/external/ov9728/Makefile"
cp -a COPYING "$PKG_DIR/COPYING"

sed -i \
  -e "s/#MODULE_NAME#/${PKG_NAME}/g" \
  -e "s/#MODULE_VERSION#/${VERSION}/g" \
  "$PKG_DIR/dkms.conf"

inject_module_metadata "$PKG_DIR/drivers/staging/media/atomisp/pci/atomisp_v4l2.c"
inject_module_metadata "$PKG_DIR/drivers/staging/media/atomisp/pci/atomisp_gmin_platform.c"
inject_module_metadata "$PKG_DIR/external/ipu-bridge/ipu-bridge.c"
inject_module_metadata "$PKG_DIR/external/mt9m114/mt9m114.c"
inject_module_metadata "$PKG_DIR/external/ov9728/ov9728.c"

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

# Remove stale in-tree build artifacts so akmods rebuilds from clean sources.
# Old .cmd files can embed absolute paths from the original build host.
find "$PKG_DIR" -type f \
  \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' -o -name '.*.cmd' -o -name 'modules.order' -o -name 'Module.symvers' \) \
  -delete
find "$PKG_DIR" -type f -name '*.a' -delete

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
