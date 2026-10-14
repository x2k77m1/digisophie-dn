#!/bin/sh
# Build digisophie-dn for Digitone mk1 OS 1.43 and 1.44: core + machines + digisophie-dn,
# lint each combination, patch test images into out/.
# Usage: sh scripts/build-digisophie-dn.sh STOCK43 [STOCK44 [MODVER]]
# MODVER stamps the build (ELE3 needs exactly 4 characters) and names it:
# out/digisophie-dn-1.43-0.1a.syx. It must match digisophie-dn/mod.json ele3_version.
set -e
STOCK43="${1:?usage: build-digisophie-dn.sh STOCK43 [STOCK44 [MODVER]]}"
STOCK44="${2:-}"
MODVER="${3:-0.1a}"
EL=tools/elekloader
export ELEKLOADER_CROSS="${ELEKLOADER_CROSS:-m68k-elf-}"
export PYTHONPATH="$EL:${PYTHONPATH:-}"
mkdir -p out
echo "== 1.43 =="
python3 -m elekloader.sdk.build "$EL/mods/core-dn1" --stock "$STOCK43"
python3 -m elekloader.sdk.build "$EL/mods/machines-dn1" --stock "$STOCK43"
python3 -m elekloader.sdk.build digisophie-dn --stock "$STOCK43"
CORE43=$(ls -t "$EL/mods/core-dn1/out"/core-*.elemod | grep -v -- '-os' | head -n 1)
MACH43=$(ls -t "$EL/mods/machines-dn1/out"/machines-*.elemod | grep -v -- '-os' | head -n 1)
SOPH43=$(ls -t digisophie-dn/out/digisophie-dn-*.elemod | grep -v -- '-os' | head -n 1)
python3 -m elekloader.lint "$SOPH43" \
  --stock "$STOCK43" \
  --with "$CORE43" \
  --with "$MACH43"
python3 -m elekloader.patch --stock "$STOCK43" \
  --mod "$CORE43" \
  --mod "$MACH43" \
  --mod "$SOPH43" \
  --out "out/digisophie-dn-1.43-$MODVER.syx" --version "$MODVER"
if [ -n "$STOCK44" ]; then
echo "== 1.44 =="
python3 -m elekloader.sdk.build "$EL/mods/core-dn1" --stock "$STOCK44"
python3 -m elekloader.sdk.build "$EL/mods/machines-dn1" --stock "$STOCK44"
python3 -m elekloader.sdk.build digisophie-dn --stock "$STOCK44"
CORE44=$(ls -t "$EL/mods/core-dn1/out"/core-*-os1.44.elemod | head -n 1)
MACH44=$(ls -t "$EL/mods/machines-dn1/out"/machines-*-os1.44.elemod | head -n 1)
SOPH44=$(ls -t digisophie-dn/out/digisophie-dn-*-os1.44.elemod | head -n 1)
python3 -m elekloader.lint "$SOPH44" \
  --stock "$STOCK44" \
  --with "$CORE44" \
  --with "$MACH44"
python3 -m elekloader.patch --stock "$STOCK44" \
  --mod "$CORE44" \
  --mod "$MACH44" \
  --mod "$SOPH44" \
  --out "out/digisophie-dn-1.44-$MODVER.syx" --version "$MODVER"
fi
echo BUILT
