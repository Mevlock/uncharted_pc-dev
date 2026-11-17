#!/usr/bin/env bash
# Builds bbport_dlss.dll (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) with
# MSVC and puts it with NVIDIA's nvngx_dlss.dll (Super Resolution) and nvngx_dlssg.dll (Frame
# Generation) next to out/bb-probe.exe; package.sh ships them.
# Needs Visual Studio 2022 (Build Tools) and a checkout of https://github.com/NVIDIA/DLSS:
#   DLSS_SDK_ROOT=F:/sdk/DLSS bash packaging/windows/build_dlss.sh
# NVIDIA Reflex PC Latency Stats (bbport_pclstats.cpp) needs pclstats.h of the Reflex SDK: REFLEX_SDK_ROOT=<a
# checkout of https://github.com/NVIDIA-RTX/REFLEX>, else Reflex_Stats/pclstats.h and the licence are fetched at
# REFLEX_SDK_TAG into out/reflex-sdk (not part of this repository; NVIDIA's SDK licence, NVIDIA-REFLEX-LICENSE.txt).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${DLSS_SDK_ROOT:?Set DLSS_SDK_ROOT to a checkout of github.com/NVIDIA/DLSS}
cmake=${CMAKE_EXE:-cmake}
reflex_tag=${REFLEX_SDK_TAG:-v1.8}
reflex=${REFLEX_SDK_ROOT:-}
if [[ -z $reflex ]]; then
    reflex=out/reflex-sdk
    if [[ ! -f $reflex/Reflex_Stats/pclstats.h || $(git -C "$reflex" describe --tags 2>/dev/null) != "$reflex_tag" ]]; then
        rm -rf out/reflex-sdk
        git clone --quiet --depth 1 --branch "$reflex_tag" --filter=blob:none --sparse \
            https://github.com/NVIDIA-RTX/REFLEX.git out/reflex-sdk
        MSYS_NO_PATHCONV=1 git -C out/reflex-sdk sparse-checkout set --no-cone /Reflex_Stats/pclstats.h /license.txt
    fi
fi
[[ -f $reflex/Reflex_Stats/pclstats.h && -f $reflex/license.txt ]] || { echo "No Reflex_Stats/pclstats.h in $reflex" >&2; exit 1; }
# Only the Vulkan headers: MSYS2's C headers must not reach MSVC.
rm -rf out/vkinclude && mkdir -p out/vkinclude
cp -r /c/msys64/clang64/include/vulkan /c/msys64/clang64/include/vk_video out/vkinclude/
# DLSS_GENERATOR: another Visual Studio (CI images move on, e.g. "Visual Studio 18 2026").
"$cmake" -S gpu/dlss_bridge -B out/dlss-bridge -G "${DLSS_GENERATOR:-Visual Studio 17 2022}" -A x64 \
    -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/vkinclude")" \
    -DREFLEX_SDK_ROOT="$(cygpath -m "$reflex")"
"$cmake" --build out/dlss-bridge --config Release
cp out/dlss-bridge/Release/bbport_dlss.dll "$sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" \
    "$sdk/lib/Windows_x86_64/rel/nvngx_dlssg.dll" out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
cp "$reflex/license.txt" out/NVIDIA-REFLEX-LICENSE.txt
echo "DLSS bridge ready: out/bbport_dlss.dll, out/nvngx_dlss.dll, out/nvngx_dlssg.dll (PCLStats: Reflex SDK $reflex)"
