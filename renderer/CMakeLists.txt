# ============================================================================
# qray renderer, vendored into QuakeRay. Built as a STATIC library and
# linked into QuakeRay. The public QR_* API stays an internal interface
# (plain function calls).
# ============================================================================
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(qray_renderer VERSION 1.3.0 LANGUAGES C CXX)
    set(QR_RENDERER_STANDALONE ON)
else()
    set(QR_RENDERER_STANDALONE OFF)
endif()

include(GNUInstallDirs)

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
    Source/RHI/RhiBloomPass.cpp
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
if(QR_RENDERER_STANDALONE)
    if(NOT WIN32)
        message(FATAL_ERROR "qray_renderer currently supports Win32 only")
    endif()

    find_package(Vulkan REQUIRED)

    if(NOT TARGET nvrhi)
        if(NOT DEFINED QR_NVRHI_SOURCE_DIR)
            set(QR_NVRHI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nvrhi")
        endif()

        if(NOT EXISTS "${QR_NVRHI_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "NVRHI was not found. Set QR_NVRHI_SOURCE_DIR to the NVRHI source tree "
                "or provide an existing nvrhi target.")
        endif()

        set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
        set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
        set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
        set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
        set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
        set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${QR_NVRHI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/nvrhi")
    endif()
endif()

if(QR_BUILD_TESTS)
    add_executable(qray_swapchain_policy Tests/SwapchainPolicyTests.cpp)
    target_compile_features(qray_swapchain_policy PRIVATE cxx_std_20)
    target_include_directories(qray_swapchain_policy PRIVATE Source ${Vulkan_INCLUDE_DIRS})
    add_test(NAME qray_swapchain_policy COMMAND qray_swapchain_policy)
    set_tests_properties(qray_swapchain_policy PROPERTIES TIMEOUT 30)

    get_filename_component(QR_VULKAN_BIN "${Vulkan_GLSLC_EXECUTABLE}" DIRECTORY)
    find_program(QR_DXC NAMES dxc HINTS "${QR_VULKAN_BIN}" REQUIRED)
    set(QR_PROBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/Tests")
    set(QR_PROBE_DEPENDENCIES
        Tests/CloudsProbe.comp.hlsl
        Source/Shaders/SkyMotion.hlsli
        Source/Shaders/CloudLayer.hlsli
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

    add_custom_command(OUTPUT "${QR_PROBE_DIR}/LocalExposureProbe.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/LocalExposureProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/LocalExposureProbe.comp.spv"
        DEPENDS Tests/LocalExposureProbe.comp.hlsl Source/Shaders/LocalExposure.hlsli
        VERBATIM)
    add_custom_command(OUTPUT "${QR_PROBE_DIR}/ColorCompositingProbe.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ColorCompositingProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/ColorCompositingProbe.comp.spv"
        DEPENDS Tests/ColorCompositingProbe.comp.hlsl Source/Shaders/ColorCompositing.hlsli
        VERBATIM)
    add_custom_command(OUTPUT "${QR_PROBE_DIR}/NearDofProbe.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/NearDofProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/NearDofProbe.comp.spv"
        DEPENDS Tests/NearDofProbe.comp.hlsl Source/Shaders/NearDof.hlsli
            Source/Generated/ShaderCommonHLSL.hlsli
        VERBATIM)
    add_executable(qray_posteffects_smoke Tests/PostEffectsSmoke.cpp
        "${QR_PROBE_DIR}/LocalExposureProbe.comp.spv" "${QR_PROBE_DIR}/ColorCompositingProbe.comp.spv"
        "${QR_PROBE_DIR}/NearDofProbe.comp.spv")
    target_compile_features(qray_posteffects_smoke PRIVATE cxx_std_20)
    target_include_directories(qray_posteffects_smoke PRIVATE Source)
    target_link_libraries(qray_posteffects_smoke PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_posteffects_smoke PRIVATE user32)
    endif()
    add_test(NAME qray_posteffects_gpu COMMAND qray_posteffects_smoke "${CMAKE_CURRENT_SOURCE_DIR}/Build" "${QR_PROBE_DIR}")
    set_tests_properties(qray_posteffects_gpu PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    set(QR_DTAL_PROBE "${QR_PROBE_DIR}/DtalPerf.comp.spv")
    add_custom_command(OUTPUT "${QR_DTAL_PROBE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.3 -fvk-use-gl-layout -T cs_6_6 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/DtalPerf.comp.hlsl"
            -Fo "${QR_DTAL_PROBE}"
        DEPENDS
            Tests/DtalPerf.comp.hlsl
            Source/Shaders/Light.hlsli
            Source/Shaders/Random.hlsli
            Source/Shaders/ShaderCommonHLSLFunc.hlsli
            Source/Shaders/ShaderCommonHLSL.hlsli
            Source/Shaders/Structs.hlsli
            Source/Generated/ShaderCommonHLSL.hlsli
        VERBATIM)
    add_executable(qray_dtal_perf Tests/DtalPerf.cpp "${QR_DTAL_PROBE}")
    target_compile_features(qray_dtal_perf PRIVATE cxx_std_20)
    target_include_directories(qray_dtal_perf PRIVATE Source)
    target_link_libraries(qray_dtal_perf PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_dtal_perf PRIVATE user32)
    endif()
    add_test(NAME qray_dtal_perf COMMAND qray_dtal_perf "${QR_DTAL_PROBE}" "${QR_PROBE_DIR}/DtalPerf.baseline.txt")
    set_tests_properties(qray_dtal_perf PROPERTIES TIMEOUT 300 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")

    if(NOT QR_RENDERER_STANDALONE)
        add_executable(qray_cluster_lists_tests
            Tests/ClusterLightListsTests.cpp
            "${CMAKE_SOURCE_DIR}/Quake/rt_light_admit.c"
        )
        target_compile_features(qray_cluster_lists_tests PRIVATE cxx_std_20)
        target_include_directories(qray_cluster_lists_tests PRIVATE
            Source
            "${CMAKE_SOURCE_DIR}/Quake"
        )
        target_link_libraries(qray_cluster_lists_tests PRIVATE renderer nvrhi nvrhi_vk)
        if(WIN32)
            target_link_libraries(qray_cluster_lists_tests PRIVATE user32)
        endif()
        add_test(NAME qray_cluster_lists_tests COMMAND qray_cluster_lists_tests)
        set_tests_properties(qray_cluster_lists_tests PROPERTIES TIMEOUT 300 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endif()
endif()

add_library(qray_alias OBJECT Source/rt_alias.c)
target_compile_features(qray_alias PRIVATE c_std_11)

add_library(renderer STATIC
    ${renderer_SOURCES}
    ${renderer_KTX_SOURCES}
    ${renderer_PUBLIC_HEADERS}
    $<TARGET_OBJECTS:qray_alias>
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
target_include_directories(renderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Vulkan (found in the parent scope or by the standalone package setup)
target_link_libraries(renderer PUBLIC Vulkan::Vulkan)

# RHI layer: NVRHI built by the root CMakeLists (target nvrhi_vk is the static
# Vulkan backend, nvrhi holds the shared interface and the validation layer).
target_link_libraries(renderer PRIVATE
    $<BUILD_INTERFACE:nvrhi_vk>
    $<BUILD_INTERFACE:nvrhi>
)

if(WIN32)
    if(NOT DEFINED QR_FFX_SDK_ROOT OR QR_FFX_SDK_ROOT STREQUAL "")
        set(QR_FFX_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Source/FidelityFX_SDK_1.1.4/src"
            CACHE PATH "FidelityFX SDK root (contains ffx-api/include and PrebuiltSignedDLL)")
    endif()

    set(QR_FFX_INCLUDE_DIR "${QR_FFX_SDK_ROOT}/ffx-api/include")
    set(QR_FFX_VK_LIBRARY "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib")

    if(NOT EXISTS "${QR_FFX_INCLUDE_DIR}/ffx_api/ffx_api.h" OR
       NOT EXISTS "${QR_FFX_VK_LIBRARY}")
        message(FATAL_ERROR
            "FidelityFX SDK was not found at QR_FFX_SDK_ROOT=${QR_FFX_SDK_ROOT}. "
            "Install the AMD FidelityFX SDK and set QR_FFX_SDK_ROOT, or provide "
            "the ffx-api headers and amd_fidelityfx_vk.lib.")
    endif()

    add_library(qray_ffx INTERFACE)
    target_include_directories(qray_ffx INTERFACE
        $<BUILD_INTERFACE:${QR_FFX_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ffx>
    )
    target_link_libraries(qray_ffx INTERFACE
        $<BUILD_INTERFACE:${QR_FFX_VK_LIBRARY}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/amd_fidelityfx_vk.lib>
    )

    target_sources(renderer PRIVATE "Source/FSR.cpp")
    target_link_libraries(renderer PRIVATE $<BUILD_INTERFACE:qray_ffx>)
endif()

include(CMakePackageConfigHelpers)

set(QRAY_RENDERER_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/qray_renderer")
set(QRAY_RENDERER_VERSION "1.3.0")

if(WIN32)
    install(TARGETS renderer
        EXPORT qray_rendererTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES Include/qray/qray.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qray)
    install(DIRECTORY "${QR_FFX_SDK_ROOT}/ffx-api/include/ffx_api"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ffx)
    install(FILES "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib"
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
        DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(TARGETS renderer
        EXPORT qray_rendererTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES Include/qray/qray.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qray)
endif()

install(EXPORT qray_rendererTargets
    FILE qray_rendererTargets.cmake
    NAMESPACE qray::
    DESTINATION ${QRAY_RENDERER_CONFIG_INSTALL_DIR}
)

configure_package_config_file(
    cmake/qray_rendererConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfig.cmake"
    INSTALL_DESTINATION ${QRAY_RENDERER_CONFIG_INSTALL_DIR}
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfigVersion.cmake"
    VERSION ${QRAY_RENDERER_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfigVersion.cmake"
    DESTINATION ${QRAY_RENDERER_CONFIG_INSTALL_DIR}
)
