cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Out-of-source builds only.
# Keep the tree clean.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "bme: In-source builds are not supported. Run `cmake -B build`.")
endif ()

# Resolve Git metadata and derive the numeric package version from an exact release tag.
find_program(GIT_EXECUTABLE git)
set(BME_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/bme_version.hpp")
set(BME_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake")
set(BME_VERSION_ARGS
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/cmake/bme_version.hpp.in"
    "-DOUT=${BME_VERSION_HEADER}")

block(PROPAGATE BME_GIT_TAG)
    set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(IN "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bme_version.hpp.in")
    set(OUT "${BME_VERSION_HEADER}")
    include("${BME_VERSION_SCRIPT}")
endblock()

set(BME_PROJECT_VERSION "1.0.0")
if (BME_GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
    set(BME_PROJECT_VERSION "${CMAKE_MATCH_1}")
endif ()

project(bme VERSION "${BME_PROJECT_VERSION}" LANGUAGES C CXX)

# Native engine backends are available only on Windows and Linux.
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(BME_OS_WINDOWS ON)
    enable_language(ASM_MASM)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BME_OS_LINUX ON)
    enable_language(ASM)
else ()
    message(FATAL_ERROR "${PROJECT_NAME}: Unsupported operating system. Only Windows and Linux are supported.")
endif ()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    message(FATAL_ERROR "${PROJECT_NAME}: Only x86-64 is supported.")
endif ()

# clang++ is primary.
# clang-cl and MSVC use the MSVC ABI path.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        set(BME_IS_CLANG ON)
    else ()
        set(BME_IS_MSVC ON)
        set(BME_IS_CLANG_CL ON)
    endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(BME_IS_GCC ON)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(BME_IS_MSVC ON)
else ()
    message(FATAL_ERROR "${PROJECT_NAME}: Unsupported compiler. Only clang++, clang-cl, MSVC, and GCC are supported.")
endif ()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "${PROJECT_NAME}: Setting build type to `Release` as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
elseif (CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    message(FATAL_ERROR "${PROJECT_NAME}: MinSizeRel is not supported.")
endif ()

# CI uses a pinned compiler, so only CI promotes warnings to errors.
if (DEFINED ENV{CI})
    set(BME_WERROR_DEFAULT ON)
else ()
    set(BME_WERROR_DEFAULT OFF)
endif ()

# Project options follow their derived defaults.
option(BME_BUILD_TESTS "Build the unit tests" OFF)
option(BME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${BME_WERROR_DEFAULT})

# Emit compilation metadata and build dependencies statically.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)

# Match BME's Windows targets to the static CRT.
if (BME_OS_WINDOWS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

# Configure local clones to use the tracked formatting hook.
if (GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" config core.hooksPath .githooks
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_QUIET ERROR_QUIET)
endif ()

# Load third-party and project CMake modules.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cmake" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)

if (BME_OS_WINDOWS)
    # MSVC's UCRT marks standard functions as insecure.
    # Silence it for our code and fetched dependencies.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()

# Runtime, UI, and decoder dependencies.
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

# XED builds through mfile.py because it has no native CMake target.
include(XED)

# Platform-specific x87 conversion leaves.
if (BME_OS_WINDOWS)
    add_library(bme_asm OBJECT src/st80.asm)
else ()
    add_library(bme_asm OBJECT src/st80.S)
endif ()

# One OS backend supplies VM, clipboard, and single-step support.
if (BME_OS_WINDOWS)
    set(BME_PLATFORM_SOURCE src/os.windows.cpp)
else ()
    set(BME_PLATFORM_SOURCE src/os.linux.cpp)
endif ()

# Refresh Git metadata before targets build.
# configure_file rewrites the header only when it changes.
add_custom_target(bme_version
    BYPRODUCTS "${BME_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}" ${BME_VERSION_ARGS} -P "${BME_VERSION_SCRIPT}"
    COMMENT "Resolving git version"
    VERBATIM)

# Shared language, generated-header, and baseline codegen settings.
function(bme_apply_common_config target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_definitions(${target}
        PRIVATE
        $<IF:$<CONFIG:Debug>,BME_DEBUG=1,BME_DEBUG=0>)
    target_include_directories(${target} PRIVATE src "${CMAKE_BINARY_DIR}/generated")
    add_dependencies(${target} bme_version)

    if (BME_OS_WINDOWS)
        target_compile_definitions(${target} PRIVATE NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
    else ()
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endif ()

    if (BME_IS_CLANG OR BME_IS_GCC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-g3 -O0>)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-g0 -O3>)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:-g3 -O3>)

        if (BME_IS_CLANG)
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-fno-ident>)
        endif ()

        # Use baseline x86-64 and SSE2 with no AVX.
        target_compile_options(${target} PRIVATE -march=x86-64 -mtune=generic)
    elseif (BME_IS_MSVC)
        # MSVC's per-config defaults cover the base optimization choices.
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Oi /Gy /Gw>)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:/Oi /Gy /Gw>)

        if (BME_IS_CLANG_CL)
            target_compile_options(${target} PRIVATE /clang:-march=x86-64 /clang:-mtune=generic)
        endif ()
    endif ()

    # Every compiled target waits for XED's external build.
    add_dependencies(${target} xed_build)
endfunction()

# Keep warnings off bme_tests so GoogleTest macros cannot trip CI's -Werror.
function(bme_apply_warnings target)
    if (BME_IS_CLANG OR BME_IS_GCC)
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

# Shared core lets tests reuse BME logic.
# CMake forwards its static dependencies to bme.
add_library(bme_core STATIC src/bme_core.cpp src/os.cpp ${BME_PLATFORM_SOURCE})
bme_apply_common_config(bme_core)
bme_apply_warnings(bme_core)
target_link_libraries(bme_core
    PUBLIC fmt::fmt
    PRIVATE argparse::argparse ftxui::ftxui Zydis::Zydis bddisasm::bddisasm capstone_static xed bme_asm)

# Thin CLI and TUI entry point.
add_executable(bme src/main.cpp)
bme_apply_common_config(bme)
bme_apply_warnings(bme)
target_link_libraries(bme PRIVATE bme_core)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES src/bme_core.cpp src/os.cpp ${BME_PLATFORM_SOURCE} src/main.cpp)

# Only the executable is installed. bme_core remains an internal test seam.
include(GNUInstallDirs)
install(TARGETS bme RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Package native Linux builds as DEB and TGZ.
if (BME_OS_LINUX)
    set(CPACK_PACKAGE_VENDOR "angelfor3v3r")
    set(CPACK_PACKAGE_CONTACT "angelfor3v3r")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Bare-metal x86-64 machine-code viewer")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    include(CPack)
endif ()

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
