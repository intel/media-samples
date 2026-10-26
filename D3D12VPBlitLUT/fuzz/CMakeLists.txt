#=======================================================================================
# libFuzzer targets for the sample's input handling; see "Fuzzing" in README.md.
#
# Each harness #includes ../D3D12VPBlitLUT.cpp with D3D12VPBLITLUT_FUZZING defined, which
# leaves out main() and the Agility SDK exports, so the code under test is the sample's
# own rather than a copy. Builds with MSVC (/fsanitize=fuzzer) or clang-cl, x64 only.
#=======================================================================================

if(NOT agility_bin_dir STREQUAL "x64")
    message(FATAL_ERROR "The fuzz targets build for x64 only.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(fuzz_flags /fsanitize=address /fsanitize=fuzzer)
    # C4505: a harness doesn't call every function in the file it includes.
    set(fuzz_warnings /W4 /wd4505)
    get_filename_component(asan_runtime_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(fuzz_flags -fsanitize=address,fuzzer)
    set(fuzz_warnings -Wno-unused-function)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
                    OUTPUT_VARIABLE clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(TO_CMAKE_PATH "${clang_resource_dir}" clang_resource_dir)
    set(asan_runtime_dir "${clang_resource_dir}/lib/windows")
else()
    message(FATAL_ERROR "The fuzz targets need MSVC or clang-cl, the compilers that ship libFuzzer on Windows.")
endif()

# ASan links its runtime as a DLL, which has to sit next to the fuzzer to run it outside
# Visual Studio. Debug builds with MSVC use the _dbg variant.
file(GLOB asan_runtime_dlls "${asan_runtime_dir}/clang_rt.asan*_dynamic-x86_64.dll")
if(NOT asan_runtime_dlls)
    message(WARNING "No ASan runtime DLL found in ${asan_runtime_dir}; add it to PATH to run the fuzzers.")
endif()

# CMake's Debug flags include /RTC1, which ASan rejects. Flags set here only apply to the
# targets in this directory.
string(REGEX REPLACE "[/-]RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

foreach(fuzzer fuzz_command_line fuzz_input_files)
    add_executable(${fuzzer} ${fuzzer}.cpp)

    set_target_properties(${fuzzer} PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    target_include_directories(${fuzzer} BEFORE PRIVATE "${agility_include}")
    target_link_libraries(${fuzzer} PRIVATE d3d12 dxgi shell32)
    target_compile_definitions(${fuzzer} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${fuzzer} PRIVATE ${fuzz_flags} ${fuzz_warnings} /utf-8)

    # ASan needs symbols for readable reports, in every configuration, and rules out
    # incremental linking.
    target_compile_options(${fuzzer} PRIVATE /Zi)
    target_link_options(${fuzzer} PRIVATE /DEBUG /INCREMENTAL:NO)

    # The libFuzzer runtime exports symbols; as for the sample, skip the unused import
    # library and .exp file.
    target_link_options(${fuzzer} PRIVATE /NOIMPLIB)
    if(NOT CMAKE_LINKER MATCHES "lld-link")
        target_link_options(${fuzzer} PRIVATE /NOEXP)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # LLVM's libFuzzer library is built against the release static CRT, in every
        # configuration, and without the STL's ASan container annotations; the linker
        # insists both match. The MSVC build keeps the annotations, so it alone catches
        # overflows within a string's or vector's spare capacity.
        set_target_properties(${fuzzer} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
        target_compile_definitions(${fuzzer} PRIVATE
            _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)

        # MSVC's objects name the libFuzzer and ASan libraries for the linker; clang-cl's
        # don't, as clang expects to drive the link itself. These are the libraries it
        # would pass (see `clang-cl -fsanitize=address,fuzzer -MT -###`).
        target_link_options(${fuzzer} PRIVATE
            "/WHOLEARCHIVE:${asan_runtime_dir}/clang_rt.fuzzer-x86_64.lib"
            "${asan_runtime_dir}/clang_rt.asan_dynamic-x86_64.lib"
            "/WHOLEARCHIVE:${asan_runtime_dir}/clang_rt.asan_static_runtime_thunk-x86_64.lib")
    endif()

    if(asan_runtime_dlls)
        add_custom_command(TARGET ${fuzzer} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${asan_runtime_dlls} "$<TARGET_FILE_DIR:${fuzzer}>"
            COMMENT "Staging the ASan runtime for ${fuzzer}")
    endif()
endforeach()
