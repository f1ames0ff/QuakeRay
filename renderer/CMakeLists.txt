# ============================================================================
# qray renderer, vendored into QuakeRay. Built as a STATIC library and
# linked into QuakeRay. The public QR_* API stays an internal interface
# (plain function calls).
# ============================================================================
cmake_minimum_required(VERSION 3.20)

set(renderer_CXX_STANDARD 20)

# The renderer source + public headers + shaders live in this folder.
set(renderer_PUBLIC_HEADERS
    Include/qray/qray.h)

set(renderer_SOURCES
    Source/qray.cpp
    Source/VulkanDevice.cpp
    Source/VulkanDevice_Init.cpp
    Source/Buffer.cpp
    Source/Scene.cpp
    Source/PhysicalDevice.cpp
    Source/Queues.cpp
    Source/Swapchain.cpp
    Source/GlobalUniform.cpp
    Source/CommandBufferManager.cpp
    Source/ShaderManager.cpp
    Source/VertexCollector.cpp
    Source/ASManager.cpp
    Source/VertexCollectorFilter.cpp
    Source/ASBuilder.cpp
    Source/ScratchBuffer.cpp
    Source/Utils.cpp
    Source/Common.cpp
    Source/Matrix.cpp
    Source/RasterizedDataCollector.cpp
    Source/Vma/vk_mem_alloc_imp.cpp
    Source/ImageLoader.cpp
    Source/TextureManager.cpp
    Source/MemoryAllocator.cpp
    Source/SamplerManager.cpp
    Source/TextureOverrides.cpp
    Source/TextureDescriptors.cpp
    Source/TextureUploader.cpp
    Source/VertexCollectorFilterType.cpp
    Source/Generated/ShaderCommonCFramebuf.cpp
    Source/Framebuffers.cpp
    Source/BlueNoise.cpp
    Source/Tonemapping.cpp
    Source/ClusterLightLists.cpp
    Source/LightManager.cpp
    Source/AutoBuffer.cpp
    Source/ASComponent.cpp
    Source/CubemapManager.cpp
    Source/CubemapUploader.cpp
    Source/GeomInfoManager.cpp
    Source/VertexPreprocessing.cpp
    Source/UserFunction.cpp
    Source/QrException.cpp
    Source/DLSS.cpp
    Source/HaltonSequence.cpp
    Source/DecalManager.cpp
    Source/Stb/stb_image.cpp
    Source/ImageLoaderDev.cpp
    Source/PortalList.cpp
    Source/TextureObserver.cpp
    Source/RayStats.cpp
    Source/RHI/NvrhiContext.cpp
    Source/RHI/NvrhiFrameSkeleton.cpp
    Source/RHI/NvrhiRequirements.cpp
    Source/RHI/RhiAccelStructs.cpp
    Source/RHI/RhiCloudsPass.cpp
    Source/RHI/RhiCloudShadowBinding.cpp
    Source/RHI/RhiDecalPass.cpp
    Source/RHI/RhiDescriptors.cpp
    Source/RHI/RhiFrameContext.cpp
    Source/RHI/RhiFsrPass.cpp
    Source/RHI/RhiPipeline.cpp
    Source/RHI/RhiPostEffectPass.cpp
    Source/RHI/RhiProceduralSkyPass.cpp
    Source/RHI/RhiRasterSkyPass.cpp
    Source/RHI/RhiRasterOverlayPass.cpp
    Source/RHI/RhiRtComposePass.cpp
    Source/RHI/RhiRtDirectPass.cpp
    Source/RHI/RhiRtGodRaysPass.cpp
    Source/RHI/RhiRtIndirectPass.cpp
    Source/RHI/RhiRtPrimaryPass.cpp
    Source/RHI/RhiRtReflRefrPass.cpp
    Source/RHI/RhiShadowMapPass.cpp
    Source/RHI/RhiSkyPass.cpp
    Source/RHI/RhiResources.cpp
    Source/RHI/RhiTextureSource.cpp
    Source/RHI/RhiTextureTable.cpp
    Source/RHI/RhiUiPass.cpp
    Source/WorldLights.cpp
)

set(renderer_KTX_FOLDER Source/KTX/lib)
set(renderer_KTX_SOURCES
    "${renderer_KTX_FOLDER}/texture.c"
    "${renderer_KTX_FOLDER}/texture1.c"
    "${renderer_KTX_FOLDER}/texture2.c"
    "${renderer_KTX_FOLDER}/hashlist.c"
    "${renderer_KTX_FOLDER}/checkheader.c"
    "${renderer_KTX_FOLDER}/swap.c"
    "${renderer_KTX_FOLDER}/memstream.c"
    "${renderer_KTX_FOLDER}/filestream.c"
    "${renderer_KTX_FOLDER}/vkloader.c"
    "${renderer_KTX_FOLDER}/vk_funcs.c"
    "${renderer_KTX_FOLDER}/basisu/zstd/zstd.c"
    "${renderer_KTX_FOLDER}/dfdutils/createdfd.c"
    "${renderer_KTX_FOLDER}/dfdutils/interpretdfd.c"
    "${renderer_KTX_FOLDER}/dfdutils/queries.c"
    "${renderer_KTX_FOLDER}/dfdutils/vk2dfd.c"
)

# Win32 Vulkan surface is required by the host (SDL window).
add_compile_definitions(
    QR_USE_SURFACE_WIN32
    VK_USE_PLATFORM_WIN32_KHR
    NOMINMAX
    # Static build: the QR_* functions are plain internal calls, not exports.
    # qray.h turns QRAPI into an empty macro when QR_STATIC is defined.
    QR_STATIC
    # KTX-Software
    KHRONOS_STATIC
    LIBKTX
    # FSR 3.1 via prebuilt AMD-signed FidelityFX API DLL
    QR_USE_FSR3
)

if(WIN32)
    # DLSS stays optional (off by default, matching the current qray build)
    option(QR_WITH_NVIDIA_DLSS "Build qray with Nvidia DLSS" OFF)
    if(QR_WITH_NVIDIA_DLSS)
        add_compile_definitions(QR_USE_NVIDIA_DLSS)
        if(DEFINED ENV{DLSS_SDK_PATH})
            target_include_directories(renderer PRIVATE "$ENV{DLSS_SDK_PATH}/include")
            add_compile_definitions(NV_WINDOWS)
            if(CMAKE_BUILD_TYPE STREQUAL "Debug")
                target_link_libraries(renderer PRIVATE "$ENV{DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d_dbg.lib")
            else()
                target_link_libraries(renderer PRIVATE "$ENV{DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d.lib")
            endif()
        else()
            message(FATAL_ERROR "QR_WITH_NVIDIA_DLSS is ON but DLSS_SDK_PATH is not set")
        endif()
    endif()
endif()

option(QR_BUILD_TESTS "Build headless renderer GPU regression tests" OFF)
if(QR_BUILD_TESTS)
    get_filename_component(QR_VULKAN_BIN "${Vulkan_GLSLC_EXECUTABLE}" DIRECTORY)
    find_program(QR_DXC NAMES dxc HINTS "${QR_VULKAN_BIN}" REQUIRED)
    set(QR_PROBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/Tests")
    set(QR_PROBE_DEPENDENCIES
        Tests/CloudsProbe.comp.hlsl
        Source/Shaders/SkyMotion.hlsli
        Source/Shaders/CloudShadowWorld.hlsli
        Source/Shaders/CloudShadowMap.hlsli
        Source/Generated/ShaderCommonHLSL.hlsli)
    add_custom_command(OUTPUT "${QR_PROBE_DIR}/CloudsProbe.comp.spv" "${QR_PROBE_DIR}/CloudsPush.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.3 -fvk-use-gl-layout -T cs_6_6 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CloudsProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/CloudsProbe.comp.spv"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.3 -fvk-use-gl-layout -T cs_6_6 -E main
            -DDESC_SET_CLOUD_SHADOW=5 -DWITH_PUSH_CONSTANTS=1
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CloudsProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/CloudsPush.comp.spv"
        DEPENDS ${QR_PROBE_DEPENDENCIES}
        VERBATIM)
    add_executable(qray_clouds_smoke Tests/CloudsSmoke.cpp
        "${QR_PROBE_DIR}/CloudsProbe.comp.spv" "${QR_PROBE_DIR}/CloudsPush.comp.spv")
    target_compile_features(qray_clouds_smoke PRIVATE cxx_std_20)
    target_include_directories(qray_clouds_smoke PRIVATE Source)
    target_link_libraries(qray_clouds_smoke PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_clouds_smoke PRIVATE user32)
    endif()
    add_test(NAME qray_clouds_gpu COMMAND qray_clouds_smoke "${CMAKE_CURRENT_SOURCE_DIR}/Build" "${QR_PROBE_DIR}")
    set_tests_properties(qray_clouds_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endif()

add_library(renderer STATIC
    ${renderer_SOURCES}
    ${renderer_KTX_SOURCES}
    ${renderer_PUBLIC_HEADERS}
)

target_compile_features(renderer PRIVATE cxx_std_20)

# Keep the vendored renderer warnings under control (the host build uses /WX).
if(MSVC)
    target_compile_options(renderer PRIVATE /W3 /EHsc /WX- /MP)
    target_compile_options(renderer PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /wd4996 /wd4100 /wd4505 /wd5287 /wd4005)
else()
    target_compile_options(renderer PRIVATE -w)
endif()

# KTX includes
target_include_directories(renderer PRIVATE
    "Source/KTX/include"
    "Source/KTX/other_include"
    "Source/KTX/lib/basisu/zstd"
)

# Public include dir (qray/qray.h) — needed by the host and by qray sources
target_include_directories(renderer PUBLIC "Include")

# Vulkan (found in the parent scope)
target_link_libraries(renderer PUBLIC Vulkan::Vulkan)

# RHI layer: NVRHI built by the root CMakeLists (target nvrhi_vk is the static
# Vulkan backend, nvrhi holds the shared interface and the validation layer).
target_link_libraries(renderer PRIVATE nvrhi_vk nvrhi)

# FSR 3.1 — prebuilt AMD-signed FidelityFX API DLL
if(WIN32)
    set(renderer_FFX_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/FidelityFX_SDK_1.1.4/src")
    target_sources(renderer PRIVATE "Source/FSR.cpp")
    target_include_directories(renderer PRIVATE "${renderer_FFX_API_DIR}/ffx-api/include")
    target_link_libraries(renderer PUBLIC "${renderer_FFX_API_DIR}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib")
    # amd_fidelityfx_vk.dll is copied next to quakeray.exe in the root CMakeLists
endif()
