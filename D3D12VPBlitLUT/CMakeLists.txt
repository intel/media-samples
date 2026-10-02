#=======================================================================================
# D3D12VPBlitLUT
#
# Builds the sample and takes care of the DirectX 12 Agility SDK dependency: the 1.620
# NuGet package is downloaded at configure time (or taken from AGILITY_SDK_ROOT), its
# headers are placed ahead of the
# Windows SDK headers, and D3D12Core.dll / d3d12SDKLayers.dll are staged into the
# "D3D12" subfolder next to the executable, where the loader expects them.
#
#   cmake -S . -B build
#   cmake --build build --config Debug
#
# Everything the sample needs to run ends up in build/<config>/.
#=======================================================================================

cmake_minimum_required(VERSION 3.20)

project(D3D12VPBlitLUT
    VERSION     1.0.0
    DESCRIPTION "D3D12 video processing with CSC, 1D LUT and 3D LUT transforms"
    LANGUAGES   CXX)

if(NOT WIN32)
    message(FATAL_ERROR "D3D12VPBlitLUT is a Direct3D 12 sample and only builds on Windows.")
endif()

#---------------------------------------------------------------------------------------
# DirectX 12 Agility SDK
#
# The LUT Compound API this sample uses was introduced in Agility SDK 1.620, so the
# version is pinned rather than "whatever the Windows SDK happens to ship". The sample
# exports D3D12_SDK_VERSION, so the package's D3D12Core.dll must report that same
# version (620). A preview runtime reports D3D12_PREVIEW_SDK_VERSION instead, and device
# creation then fails with D3D12_ERROR_INVALID_REDIST.
#
# Set AGILITY_SDK_ROOT to an extracted package to skip the download entirely. Otherwise
# AGILITY_SDK_VERSION is fetched from NuGet; set AGILITY_SDK_SHA256 to the package hash
# (printed by `cmake -E sha256sum`) to verify it.
#---------------------------------------------------------------------------------------

set(AGILITY_SDK_VERSION "1.620.0" CACHE STRING
    "Microsoft.Direct3D.D3D12 NuGet package version to build against")
set(AGILITY_SDK_SHA256 "" CACHE STRING
    "SHA-256 of the Agility SDK package; leave empty to skip verification")
set(AGILITY_SDK_ROOT "" CACHE PATH
    "Root of an extracted Agility SDK package; leave empty to download AGILITY_SDK_VERSION")

if(NOT AGILITY_SDK_ROOT)
    set(agility_dir "${CMAKE_BINARY_DIR}/agility-sdk/${AGILITY_SDK_VERSION}")
    set(agility_archive "${CMAKE_BINARY_DIR}/agility-sdk/Microsoft.Direct3D.D3D12.${AGILITY_SDK_VERSION}.zip")
    set(agility_url "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${AGILITY_SDK_VERSION}")

    if(NOT EXISTS "${agility_dir}/build/native/include/d3d12video.h")
        message(STATUS "Downloading DirectX 12 Agility SDK ${AGILITY_SDK_VERSION}")

        set(download_args "${agility_url}" "${agility_archive}" STATUS download_status)
        if(AGILITY_SDK_SHA256)
            list(APPEND download_args EXPECTED_HASH "SHA256=${AGILITY_SDK_SHA256}")
        endif()
        file(DOWNLOAD ${download_args})

        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            list(GET download_status 1 download_message)
            message(FATAL_ERROR
                "Failed to download the Agility SDK from ${agility_url}
"
                "  ${download_message}
"
                "If the package is not on NuGet yet, or this machine has no internet access, "
                "extract the package manually and point AGILITY_SDK_ROOT at the extracted folder.")
        endif()

        file(ARCHIVE_EXTRACT INPUT "${agility_archive}" DESTINATION "${agility_dir}")
    endif()

    set(agility_root "${agility_dir}")
else()
    set(agility_root "${AGILITY_SDK_ROOT}")
endif()

# The package ships one redist folder per architecture.
if(CMAKE_VS_PLATFORM_NAME)
    string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" agility_arch)
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" agility_arch)
endif()

if(agility_arch MATCHES "arm64|aarch64")
    set(agility_bin_dir "arm64")
elseif(agility_arch MATCHES "x64|amd64")
    set(agility_bin_dir "x64")
elseif(agility_arch MATCHES "win32|x86|i686")
    set(agility_bin_dir "win32")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(agility_bin_dir "x64")
else()
    set(agility_bin_dir "win32")
endif()

set(agility_include "${agility_root}/build/native/include")
set(agility_bin "${agility_root}/build/native/bin/${agility_bin_dir}")

foreach(required_file
        "${agility_include}/d3d12video.h"
        "${agility_bin}/D3D12Core.dll"
        "${agility_bin}/d3d12SDKLayers.dll")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Agility SDK is incomplete, missing: ${required_file}")
    endif()
endforeach()

message(STATUS "Agility SDK: ${agility_root} (${agility_bin_dir})")

#---------------------------------------------------------------------------------------
# Sample
#---------------------------------------------------------------------------------------

option(D3D12VPBLITLUT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_executable(D3D12VPBlitLUT D3D12VPBlitLUT.cpp)

set_target_properties(D3D12VPBlitLUT PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:D3D12VPBlitLUT>")

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT D3D12VPBlitLUT)

# BEFORE matters: the Agility SDK headers must win over the Windows SDK copies.
target_include_directories(D3D12VPBlitLUT BEFORE PRIVATE "${agility_include}")

# d3d12.lib and dxgi.lib come from the Windows SDK, as Microsoft recommends; only the
# headers and the runtime DLLs come from the Agility SDK. shell32 is for
# CommandLineToArgvW.
target_link_libraries(D3D12VPBlitLUT PRIVATE d3d12 dxgi shell32)

target_compile_definitions(D3D12VPBlitLUT PRIVATE
    UNICODE _UNICODE            # the sample is UTF-16 throughout; don't leave it to chance
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    $<$<CONFIG:Debug,RelWithDebInfo>:D3D12VPBLITLUT_ENABLE_DEBUG_LAYER>)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl treats -Wall like MSVC's /Wall, which means -Weverything there; /W4 is the
    # clang-cl spelling of clang's -Wall -Wextra.
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(D3D12VPBlitLUT PRIVATE /W4 /utf-8)
    else()
        target_compile_options(D3D12VPBlitLUT PRIVATE -Wall)
    endif()
    target_compile_options(D3D12VPBlitLUT PRIVATE
        -Wextra -Wpedantic
        # Unavoidable when compiling against the Windows SDK / Agility SDK headers.
        -Wno-address-of-temporary
        -Wno-language-extension-token
        -Wno-macro-redefined
        -Wno-gnu-redeclared-enum
        -Wno-microsoft-enum-forward-reference
        -Wno-microsoft-enum-value
        -Wno-nested-anon-types
        -Wno-c++11-narrowing)
    if(D3D12VPBLITLUT_WARNINGS_AS_ERRORS)
        target_compile_options(D3D12VPBlitLUT PRIVATE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(D3D12VPBlitLUT PRIVATE /W4 /permissive- /utf-8)
    if(D3D12VPBLITLUT_WARNINGS_AS_ERRORS)
        target_compile_options(D3D12VPBlitLUT PRIVATE /WX)
    endif()
endif()

#---------------------------------------------------------------------------------------
# Runtime layout
#
# The exported D3D12SDKPath in D3D12VPBlitLUT.cpp is ".\D3D12\", so the redist has
# to live in that subfolder - never next to the .exe, or the loader can pick up a
# mismatched D3D12Core.dll / d3d12SDKLayers.dll pair. The sample LUTs are copied too so
# the example command line in README.md works from the output directory.
#---------------------------------------------------------------------------------------

add_custom_command(TARGET D3D12VPBlitLUT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:D3D12VPBlitLUT>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${agility_bin}/D3D12Core.dll"
            "${agility_bin}/d3d12SDKLayers.dll"
            "$<TARGET_FILE_DIR:D3D12VPBlitLUT>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/lut1d_identity_1024.dat"
            "${CMAKE_CURRENT_SOURCE_DIR}/lut3d_65x65x65.dat"
            "$<TARGET_FILE_DIR:D3D12VPBlitLUT>"
    COMMENT "Staging Agility SDK redistributable and sample LUTs")

# The input clip is not redistributable (see .gitignore), so stage it only when a local
# copy sits next to this file.
set(sample_input_clip "${CMAKE_CURRENT_SOURCE_DIR}/input_1920x1080_p010.bin")
if(EXISTS "${sample_input_clip}")
    add_custom_command(TARGET D3D12VPBlitLUT POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${sample_input_clip}"
                "$<TARGET_FILE_DIR:D3D12VPBlitLUT>"
        COMMENT "Staging sample input clip")
endif()
