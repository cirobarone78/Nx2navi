# libamd_fidelityfx_vk.so for Linux: AMD FidelityFX SDK v1.1.4's ffx-api library (amd_fidelityfx_vk.dll on Windows) with
# the FSR 3.1 upscaler and its Vulkan backend only. AMD ships the library for Windows only. The shader compiler port,
# gen_shaders.sh, ffx_linux_compat.h and the SDK patches come from github.com/JonathanOrr/xplane-fsr3 (MIT).
cmake_minimum_required(VERSION 3.20)
project(amd_fidelityfx_vk_linux CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(FFX_ROOT "" CACHE PATH "the unpacked, patched FidelityFX SDK v1.1.4 (common.cmake, ffx-api/, sdk/)")
set(FFX_SDK "${FFX_ROOT}/sdk")
set(FFX_API "${FFX_ROOT}/ffx-api")
set(FFX_SC_LIBS "${FFX_SDK}/tools/ffx_shader_compiler/libs")
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

add_executable(FidelityFX_SC
    ffx_sc/ffx_sc.cpp ffx_sc/glsl_compiler.cpp ffx_sc/utils.cpp
    "${FFX_SC_LIBS}/MD5/md5.cpp" "${FFX_SC_LIBS}/SPIRV-Reflect/spirv_reflect.c"
    "${FFX_SC_LIBS}/tiny-process-library/process.cpp" "${FFX_SC_LIBS}/tiny-process-library/process_unix.cpp")
target_include_directories(FidelityFX_SC PRIVATE ffx_sc
    "${FFX_SC_LIBS}/MD5" "${FFX_SC_LIBS}/SPIRV-Reflect" "${FFX_SC_LIBS}/tiny-process-library")
target_link_libraries(FidelityFX_SC PRIVATE Threads::Threads)

set(FFX_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_custom_command(OUTPUT "${FFX_SHADER_OUT}/stamp"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gen_shaders.sh" $<TARGET_FILE:FidelityFX_SC> "${FFX_SDK}" "${FFX_SHADER_OUT}"
    COMMAND ${CMAKE_COMMAND} -E touch "${FFX_SHADER_OUT}/stamp"
    DEPENDS FidelityFX_SC gen_shaders.sh COMMENT "FSR 3.1 upscaler Vulkan shader permutations")
add_custom_target(fsr3_shaders DEPENDS "${FFX_SHADER_OUT}/stamp")

add_library(amd_fidelityfx_vk SHARED
    "${FFX_API}/src/ffx_api.cpp" "${FFX_API}/src/backends.cpp" "${FFX_API}/src/validation.cpp"
    "${FFX_API}/src/ffx_provider_fsr3upscale.cpp" ffx_provider_linux.cpp ffx_fg_stub.cpp
    "${FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${FFX_SDK}/src/backends/vk/ffx_vk.cpp"
    "${FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp"
    "${FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${FFX_SDK}/src/shared/ffx_assert.cpp" "${FFX_SDK}/src/shared/ffx_message.cpp"
    "${FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp" "${FFX_SDK}/src/shared/ffx_object_management.cpp")
add_dependencies(amd_fidelityfx_vk fsr3_shaders)
# ffx-api includes <FidelityFx/host/...>: the folder is FidelityFX, which only a case-insensitive file system forgives.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/case")
file(CREATE_LINK "${FFX_SDK}/include/FidelityFX" "${CMAKE_CURRENT_BINARY_DIR}/case/FidelityFx" SYMBOLIC)
target_compile_options(amd_fidelityfx_vk PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/ffx_linux_compat.h" -w
                       -fvisibility=hidden -fvisibility-inlines-hidden)
target_compile_definitions(amd_fidelityfx_vk PRIVATE FFX_GCC FFX_BACKEND_VK FFX_FSR3UPSCALER
    "FFX_SDK_DEFAULT_CONTEXT_SIZE=(1024 * 256)" FFXDLL_VERSION_MAJOR=1 FFXDLL_VERSION_MINOR=0 FFXDLL_VERSION_PATCH=1
    FFXDLL_VERSION_API=VK CI_PIPELINE_ID=0)
target_include_directories(amd_fidelityfx_vk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/case" "${FFX_API}/include" "${FFX_API}/src" "${FFX_SDK}/include"
    "${FFX_SDK}/include/FidelityFX/host" "${FFX_SDK}/src/shared" "${FFX_SDK}/src/backends/shared"
    "${FFX_SDK}/src/components" "${FFX_SHADER_OUT}" "${FFX_SDK}/include/FidelityFX/gpu"
    "${FFX_SDK}/include/FidelityFX/gpu/fsr3upscaler")
target_link_libraries(amd_fidelityfx_vk PRIVATE Vulkan::Vulkan)
target_link_options(amd_fidelityfx_vk PRIVATE -Wl,--no-undefined -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL
                    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
