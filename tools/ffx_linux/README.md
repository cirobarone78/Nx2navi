# FSR 3.1 on Linux: libamd_fidelityfx_vk.so

On Windows the game loads AMD's `amd_fidelityfx_vk.dll` from beside `nx2navi.exe` for FSR 3.1 (`runtime/vk_ffx.cpp`).
AMD ships that library for Windows only, so on Linux FSR 3.1 was off. `build.sh` builds the same library for Linux,
`libamd_fidelityfx_vk.so`, from the FidelityFX SDK v1.1.4 release (MIT): the same five entry points (`ffxCreateContext`,
`ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`) and only them, with the **FSR 3.1 upscaler**, **FSR 3.1
frame generation** (frame interpolation, optical flow and the frame interpolation swapchain `runtime/vk_fg.cpp` presents
through) and their Vulkan backend.

The Linux AppImage carries the library and copies it beside the game after each build.

## What comes from where

- AMD FidelityFX SDK v1.1.4 (MIT), downloaded by `build.sh` and checked against its SHA-256.
- From [xplane-fsr3](https://github.com/JonathanOrr/xplane-fsr3) (MIT, `LICENSE.xplane-fsr3`): the shader compiler
  port `ffx_sc/`, `gen_shaders.sh`, `ffx_linux_compat.h`, `ffx_fg_stub.cpp`, `patches/fidelityfx-sdk-v1.1.4-linux.patch`
  (context size, scratch alignment, descriptor pool, unified memory) and `patches/fsr-3.1.5-backport.patch`.
- This folder: `CMakeLists.txt`, `ffx_provider_linux.cpp` (the provider registry: the FSR 3.1 upscaler, frame generation
  and its Vulkan swapchain, without AMD's D3D12 driver lookup), `exports.map`, `win32/` and the patches
  `patches/nx2navi-ffx-api-linux.patch`:
  - the API entry points get default visibility instead of `__declspec(dllexport)`;
  - an MSVC `ui64` literal becomes `ull`;
  - the backend offers wave32 and 32-bit shaders only: the game enables neither `subgroupSizeControl` nor
    `shaderFloat16`/`shaderInt16`, and wave64 blurs the output on RADV (xplane-fsr3's `PORTING.md`). Frame
    interpolation and optical flow get the same shaders.

  and `patches/fidelityfx-sdk-v1.1.4-framegeneration-linux.patch`, which lets the frame generation sources build with GCC:
  - MSVC `ui64` literals become `ull`, and `%s` of wide strings in `swprintf` becomes `%ls`;
  - a function pointer cast with `reinterpret_cast`, and two temporaries passed by non-const reference made named;
  - the swapchain's pass-through of the Windows-only full-screen-exclusive structures is left out on Linux.
- `win32/Windows.h`: the Win32 calls of the frame interpolation swapchain (critical sections, events, threads,
  `QueryPerformanceCounter`) on POSIX threads and `CLOCK_MONOTONIC`, so AMD's swapchain source stays as it is. Thread
  priorities are left to the system, since raising them needs privileges on Linux.

libstdc++ is linked statically and not exported, so the library needs only glibc and `libvulkan.so.1`.
