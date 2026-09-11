# Intel XED decoder backend.
#
# XED has no native CMake.
# CPM downloads its sources plus `intelxed/mbuild`, an ExternalProject drives
# `mfile.py` to build a static library, and we expose it as the IMPORTED target `xed`. Needs Python 3 at build time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

if (CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Intel XED currently requires a single-config generator. Use Ninja with an explicit CMAKE_BUILD_TYPE.")
endif ()

# Download-only.
# XED has no CMakeLists to add_subdirectory, we just need the sources.
CPMAddPackage(URI "gh:intelxed/xed#0bcb6237345c5066726dcc08b3d87928df3b5b26" DOWNLOAD_ONLY YES) # v2026.08.23
CPMAddPackage(URI "gh:intelxed/mbuild#1b437e409221a2b5703b4d8896baa20d43e4ba1a" DOWNLOAD_ONLY YES) # v2026.08.23

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(XED_JOBS)
if (XED_JOBS EQUAL 0)
    set(XED_JOBS 4)
endif ()

set(XED_KIT "${CMAKE_BINARY_DIR}/xed-kit")
set(XED_INCLUDE_DIR "${XED_KIT}/include")

if (BME_OS_WINDOWS)
    # XED is C.
    # Build it with MSVC so it uses BME's static CRT and MSVC ABI.
    set(XED_LIBRARY "${XED_KIT}/lib/xed.lib")
    set(XED_RUNTIME_FLAG "/MT")
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(XED_RUNTIME_FLAG "/MTd")
    endif ()

    set(XED_BUILD_COMMAND
        "${CMAKE_COMMAND}" -E env "PYTHONPATH=${mbuild_SOURCE_DIR}"
        "${Python3_EXECUTABLE}" "${xed_SOURCE_DIR}/mfile.py"
        "--cc=cl"
        "--cxx=cl"
        "--compiler=ms"
        "--jobs=${XED_JOBS}"
        --no-mscrt
        "--extra-ccflags=${XED_RUNTIME_FLAG}"
        "--extra-cxxflags=${XED_RUNTIME_FLAG}"
        --no-encoder
        "--install-dir=${XED_KIT}"
        install)
else ()
    # The GNU mbuild path emits the normal Unix static library.
    set(XED_LIBRARY "${XED_KIT}/lib/libxed.a")
    set(XED_BUILD_COMMAND
        "${CMAKE_COMMAND}" -E env "PYTHONPATH=${mbuild_SOURCE_DIR}"
        "${Python3_EXECUTABLE}" "${xed_SOURCE_DIR}/mfile.py"
        "--cc=${CMAKE_C_COMPILER}"
        "--cxx=${CMAKE_CXX_COMPILER}"
        "--compiler=gnu"
        "--jobs=${XED_JOBS}"
        --no-encoder
        "--install-dir=${XED_KIT}"
        install)
endif ()

# `install` assembles a kit.
# Headers land under include/xed and the static library under lib/.
ExternalProject_Add(xed_build
    SOURCE_DIR "${xed_SOURCE_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND "${XED_BUILD_COMMAND}"
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${XED_LIBRARY}"
    USES_TERMINAL_BUILD TRUE)

# IMPORTED target.
# Include dirs from IMPORTED targets are treated as SYSTEM automatically, so XED's headers never trip our -Werror.
# The include dir must exist at configure time.
file(MAKE_DIRECTORY "${XED_INCLUDE_DIR}")
add_library(xed STATIC IMPORTED GLOBAL)
set_target_properties(xed PROPERTIES
    IMPORTED_LOCATION "${XED_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${XED_INCLUDE_DIR}")
