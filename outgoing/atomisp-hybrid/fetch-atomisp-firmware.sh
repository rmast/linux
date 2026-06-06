#!/usr/bin/env bash
# fetch-atomisp-firmware.sh
# Haalt de drie atomisp firmware-blobs op uit linux-firmware.git en legt ze
# klaar zodat rpmbuild ze als Source0/Source1/Source2 kan oppakken.
#
# Gebruik: ./fetch-atomisp-firmware.sh [--dest DIR]
#   --dest DIR  map waar de .bin-bestanden worden neergezet (default: ~/rpmbuild/SOURCES)
set -euo pipefail

DEST="${HOME}/rpmbuild/SOURCES"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dest) DEST="$2"; shift 2 ;;
    *) echo "Onbekend argument: $1" >&2; exit 2 ;;
  esac
done

BASEURL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/intel/ipu"

BLOBS=(
  "shisp_2400b0_v21.bin"
  "shisp_2401a0_v21.bin"
  "shisp_2401a0_legacy_v21.bin"
)

mkdir -p "$DEST"
for blob in "${BLOBS[@]}"; do
  dest_file="$DEST/$blob"
  if [[ -f "$dest_file" ]]; then
    echo "[skip] $blob  (al aanwezig in $DEST)"
    continue
  fi
  echo "[download] $blob"
  curl -fL --retry 3 "$BASEURL/$blob" -o "$dest_file"
  echo "[ok] $blob -> $dest_file"
done

echo ""
echo "Alle firmware-blobs aanwezig in $DEST"
echo "Bouw nu de firmware-RPM met:"
echo "  cp outgoing/atomisp-hybrid/akmod/atomisp-firmware.spec ~/rpmbuild/SPECS/"
echo "  rpmbuild -ba ~/rpmbuild/SPECS/atomisp-firmware.spec"
echo "  sudo dnf install ~/rpmbuild/RPMS/noarch/atomisp-firmware-*.noarch.rpm

Installeer ook op de T100ta en HP x2 210:
  scp ~/rpmbuild/RPMS/noarch/atomisp-firmware-*.noarch.rpm user@t100ta:~
  ssh user@t100ta 'sudo dnf install atomisp-firmware-*.noarch.rpm'"
