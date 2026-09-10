cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Out-of-source builds only.
# Keep the tree clean.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "bme: In-source builds are not supported. Run `cmake -B build`.")
endif ()

project(bme CXX C ASM_MASM)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "${PROJECT_NAME}: Unsupported operating system. Only Windows is supported.")
endif ()

# Supported compilers.
# clang++ (GNU driver, primary/CI), clang-cl, and MSVC cl.
# clang-cl shares MSVC's flag dialect + ABI, so it rides the MSVC path. `BME_IS_CLANG_CL` also flags it an LLVM backend
# (takes `-march` via `/clang:`).
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        set(BME_IS_CLANG ON)
    else ()
        set(BME_IS_MSVC ON)
        set(BME_IS_CLANG_CL ON)
    endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(BME_IS_MSVC ON)
else ()
    message(FATAL_ERROR "${PROJECT_NAME}: Unsupported compiler. Only clang++ (GNU driver), clang-cl, and MSVC are supported.")
endif ()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "${PROJECT_NAME}: Setting build type to `Release` as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
elseif (CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    message(FATAL_ERROR "${PROJECT_NAME}: MinSizeRel is not supported.")
endif ()

# `-Werror` is brittle as a permanent flag.
# A newer compiler's warnings break an otherwise-fine build. So make warnings fatal only in CI, where the toolchain is
# pinned (GitHub sets CI=1). Locally they still show but don't block.
if (DEFINED ENV{CI})
    set(BME_WERROR_DEFAULT ON)
else ()
    set(BME_WERROR_DEFAULT OFF)
endif ()

# Project options live together after their defaults are established.
option(BME_BUILD_TESTS "Build the unit tests" OFF)
option(BME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${BME_WERROR_DEFAULT})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Route git at the tracked pre-commit hook (rejects unformatted commits) for local clones.
find_program(GIT_EXECUTABLE git)
if (GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" config core.hooksPath .githooks
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_QUIET ERROR_QUIET)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)

# MSVC's UCRT marks standard functions (getenv, etc) as "insecure".
# Silence it for our code + the fetched deps.
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

CPMAddPackage(URI "gh:fmtlib/fmt#1be298e1bd68957e4cd352e1f676f00e07dcfb57" # 12.2.0
    OPTIONS
    "FMT_DOC OFF"
    "FMT_INSTALL OFF"
    "FMT_TEST OFF"
    "FMT_MODULE OFF")
CPMAddPackage(URI "gh:p-ranav/argparse#3eda91b2e1ce7d569f84ba295507c4cd8fd96910" # v3.2
    OPTIONS
    "ARGPARSE_INSTALL OFF"
    "ARGPARSE_BUILD_TESTS OFF"
    "ARGPARSE_BUILD_SAMPLES OFF")
CPMAddPackage(URI "gh:ArthurSonzogni/FTXUI#f921fad208912747c17d129a8ef75ec7624b6eec" # v7.0.3
    OPTIONS
    "FTXUI_ENABLE_INSTALL OFF"
    "FTXUI_ENABLE_CCACHE OFF")
CPMAddPackage(URI "gh:zyantific/zydis#a95bb71013436547f689fc0b380f48edf663066e" # master
    OPTIONS
    "ZYDIS_FEATURE_ENCODER OFF"
    "ZYDIS_BUILD_EXAMPLES OFF"
    "ZYDIS_BUILD_TOOLS OFF"
    "ZYDIS_BUILD_DOXYGEN OFF"
    "ZYDIS_BUILD_TESTS OFF")
CPMAddPackage(URI "gh:bitdefender/bddisasm#ba6011004839be7098f0aaae3e084f0f646ab3ba" # v3.0.1
    OPTIONS
    "BDD_INCLUDE_TOOL OFF"
    "BDD_INCLUDE_ISAGENERATOR_X86 OFF")
CPMAddPackage(URI "gh:capstone-engine/capstone#022575848782a4801fd150fdbc927effcbca0864" # 5.0.9
    OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "BUILD_STATIC_LIBS ON"
    "BUILD_STATIC_RUNTIME ON"
    "CAPSTONE_BUILD_TESTS OFF"
    "CAPSTONE_BUILD_CSTOOL OFF"
    "CAPSTONE_ARCHITECTURE_DEFAULT OFF"
    "CAPSTONE_INSTALL OFF"
    "CAPSTONE_X86_SUPPORT ON")

# Intel XED decoder backend. No native CMake, built via `mfile.py` through ExternalProject (see `cmake/XED.cmake`).
include(XED)

# MASM assembly target.
add_library(bme_asm OBJECT src/st80.asm)

# `--version` reads a generated header.
# The custom target re-runs every build, but `configure_file` rewrites only on change, so a target recompiles only when
# the tag/hash move. Also generated at configure time so the header exists up front.
set(BME_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/bme_version.hpp")
set(BME_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake")
set(BME_VERSION_ARGS
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/cmake/bme_version.hpp.in"
    "-DOUT=${BME_VERSION_HEADER}")
execute_process(COMMAND "${CMAKE_COMMAND}" ${BME_VERSION_ARGS} -P "${BME_VERSION_SCRIPT}")
add_custom_target(bme_version
    BYPRODUCTS "${BME_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}" ${BME_VERSION_ARGS} -P "${BME_VERSION_SCRIPT}"
    COMMENT "Resolving git version"
    VERBATIM)

# Shared build settings, applied to every bme target so nothing drifts.
# Compile standard, preprocessor definitions, include dirs, the version-header dependency, and the baseline x86-64
# codegen floor. Warnings are separate (see below) so the test target can skip `-Werror`.
function(bme_apply_common_config target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_definitions(${target}
        PRIVATE
        $<IF:$<CONFIG:Debug>,BME_DEBUG=1,BME_DEBUG=0>
        NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
    target_include_directories(${target} PRIVATE src "${CMAKE_BINARY_DIR}/generated")
    add_dependencies(${target} bme_version)

    if (BME_IS_CLANG)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-g3 -O0>)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-fno-ident -g0 -O3>)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:-g3 -O3>)

        # Baseline x86-64 (SSE2, no AVX).
        # Runs on any x86-64 CPU and pins the floor against a stray `-march=native`. Clang already defaults here. This
        # just makes it explicit.
        target_compile_options(${target} PRIVATE -march=x86-64 -mtune=generic)
    elseif (BME_IS_MSVC)
        # MSVC's per-config defaults already cover the base (`/Od /Zi` Debug, `/O2` Release).
        # Add only what they omit. Intrinsics and dead code/data stripping in the optimized configs.
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Oi /Gy /Gw>)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:/Oi /Gy /Gw>)

        # clang-cl is an LLVM backend.
        # Pin the same baseline x86-64 floor via the `/clang:` escape. Plain cl needs nothing. SSE2 is its immutable x64
        # default and there's no baseline `/arch:` knob.
        if (BME_IS_CLANG_CL)
            target_compile_options(${target} PRIVATE /clang:-march=x86-64 /clang:-mtune=generic)
        endif ()
    endif ()

    # XED's static lib comes from the `mfile.py` ExternalProject. Make sure it's built before anything links it.
    add_dependencies(${target} xed_build)
endfunction()

# Warning policy, kept off the test target so GoogleTest's macro expansions can't trip the CI `-Werror` build.
function(bme_apply_warnings target)
    if (BME_IS_CLANG)
        target_compile_options(${target}
            PRIVATE
            -Wall
            -Wextra
            -Wshadow
            -Wpedantic)

        if (BME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif ()
    elseif (BME_IS_MSVC)
        target_compile_options(${target}
            PRIVATE
            /W4
            /permissive-)

        if (BME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif ()
    endif ()
endfunction()

# Core library.
# All of bme's logic except `main`, so the unit tests can link and call it directly. STATIC keeps a single build of the
# translation unit. PRIVATE deps still propagate onto a consumer's link line, so `bme` links ftxui/decoders
# transitively.
add_library(bme_core STATIC src/bme_core.cpp)
bme_apply_common_config(bme_core)
bme_apply_warnings(bme_core)
target_link_libraries(bme_core
    PUBLIC fmt::fmt
    PRIVATE argparse::argparse ftxui::ftxui Zydis::Zydis bddisasm::bddisasm capstone_static xed bme_asm)

# The TUI/CLI executable. A thin `main` over `bme_core`.
add_executable(bme src/main.cpp)
bme_apply_common_config(bme)
bme_apply_warnings(bme)
target_link_libraries(bme PRIVATE bme_core)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES src/bme_core.cpp src/main.cpp)

# Unit tests.
if (BME_BUILD_TESTS)
    CPMAddPackage(URI "gh:google/googletest#063de7e9578f82b369302001269680b4b1553359" # v1.18.0
        OPTIONS
        "INSTALL_GTEST OFF"
        "BUILD_GMOCK OFF"
        "gtest_force_shared_crt OFF")

    enable_testing()
    include(GoogleTest)

    add_executable(bme_tests
        test/main.cpp
        test/parse_test.cpp
        test/environment_test.cpp
        test/seed_test.cpp
        test/cli_test.cpp
        test/engine_test.cpp
        test/quick_test.cpp)
    bme_apply_common_config(bme_tests)
    target_link_libraries(bme_tests PRIVATE bme_core GTest::gtest)
    gtest_discover_tests(bme_tests)
endif ()
