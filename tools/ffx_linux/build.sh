#!/bin/bash
# Builds libamd_fidelityfx_vk.so, the Linux counterpart of AMD's amd_fidelityfx_vk.dll that runtime/vk_ffx.cpp loads from
# beside the game for FSR 3.1 upscaling. AMD ships that library for Windows only; this builds it from the FidelityFX SDK
# v1.1.4 release zip (MIT, pinned by SHA-256) with the patches in patches/, the FSR 3.1 upscaler and its Vulkan backend
# only (no frame generation). See README.md.
#
#   tools/ffx_linux/build.sh <output folder> [download cache]
#
# Needs curl, unzip, patch, cmake, ninja, a C++17 compiler, glslangValidator and the Vulkan headers and loader
# (Arch: cmake ninja glslang vulkan-headers vulkan-icd-loader; Ubuntu: cmake ninja-build glslang-tools libvulkan-dev).
set -euo pipefail
version=1.1.4
sha256=0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
cache=${2:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
[ -n "$cache" ] || cache=$work
mkdir -p "$cache"
zip="$cache/FidelityFX-SDK-v$version.zip"
if ! { [ -f "$zip" ] && echo "$sha256  $zip" | sha256sum -c --status; }; then
    curl -fsSL -o "$zip.part" "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v$version/FidelityFX-SDK-v$version.zip"
    echo "$sha256  $zip.part" | sha256sum -c --status || { rm -f "$zip.part"; echo "FidelityFX SDK zip: wrong SHA-256" >&2; exit 1; }
    mv "$zip.part" "$zip"
fi
mkdir "$work/sdk"
unzip -q "$zip" common.cmake docs/license.md 'ffx-api/*' 'sdk/*' -x 'ffx-api/bin/*' -d "$work/sdk"
# the zip's text files have CRLF line ends; the patches are LF
find "$work/sdk" -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.glsl' -o -name '*.hpp' -o -name '*.txt' \) \
    -exec sed -i 's/\r$//' {} +
for p in fidelityfx-sdk-v1.1.4-linux.patch fsr-3.1.5-backport.patch nx2navi-ffx-api-linux.patch; do
    patch -s -p1 -d "$work/sdk" < "$here/patches/$p"
done
cmake -S "$here" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ROOT="$work/sdk" >/dev/null
cmake --build "$work/build"
cp "$work/build/libamd_fidelityfx_vk.so" "$out/"
{ cat "$work/sdk/docs/license.md"; echo; echo 'Built with code from xplane-fsr3 (https://github.com/JonathanOrr/xplane-fsr3):'; echo
  cat "$here/LICENSE.xplane-fsr3"; } > "$out/libamd_fidelityfx_vk-LICENSE.txt"
echo "built $out/libamd_fidelityfx_vk.so (AMD FidelityFX SDK $version, FSR 3.1 upscaler)"
