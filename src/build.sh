#!/usr/bin/env bash
# Build AmdIgpuResetDxe.efi and AmdIgpuReset.rom, and optionally append an AMD GOP option ROM.
#
#   src/build.sh /path/to/edk2                         -> AmdIgpuReset.rom
#   src/build.sh /path/to/edk2 AMDGopDriver_9950x3d.rom  -> also AmdIgpuReset+GOP.rom
#
# edk2 needs: git clone https://github.com/tianocore/edk2 && cd edk2 &&
#   git submodule update --init BaseTools/Source/C/BrotliCompress/brotli \
#     MdeModulePkg/Library/BrotliCustomDecompressLib/brotli MdePkg/Library/MipiSysTLib/mipisyst
# plus gcc, make, nasm, uuid-dev, python3.
set -euo pipefail
EDK2=$(cd "$1" && pwd)
GOP=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(dirname "$HERE")   # ROM goes to the repo root

cd "$EDK2"
[ -x BaseTools/Source/C/bin/EfiRom ] || make -C BaseTools
export PACKAGES_PATH="$EDK2:$HERE"
set +u; set --; . ./edksetup.sh; set -u   # edksetup.sh must not see our arguments
build -p AmdIgpuResetPkg/AmdIgpuResetPkg.dsc -a X64 -t GCC -b RELEASE

# -n: leave the "last image" bit clear, so another ROM image can follow this one.
EfiRom -n -f 0x1002 -i 0x13C0 \
  -e "$EDK2/Build/AmdIgpuResetPkg/RELEASE_GCC/X64/AmdIgpuResetDxe.efi" \
  -o "$OUT/AmdIgpuReset.rom"
echo "built $OUT/AmdIgpuReset.rom"

if [ -n "$GOP" ]; then
  cat "$OUT/AmdIgpuReset.rom" "$GOP" > "$OUT/AmdIgpuReset+GOP.rom"
  echo "built $OUT/AmdIgpuReset+GOP.rom"
  EfiRom -d "$OUT/AmdIgpuReset+GOP.rom" | grep -E "^Image|Code type|Indicator|Subsystem"
fi
