#pragma once

#if defined(__clang__)
#define BME_COMPILER_CLANG 1
#elif defined(_MSC_VER)
#define BME_COMPILER_MSVC 1
#elif defined(__GNUC__)
#define BME_COMPILER_GCC 1
#else
#error "`bme` requires Clang, MSVC, or GCC"
#endif

#if defined(_WIN32)
#define BME_OS_WINDOWS 1
#define BME_OS_LINUX   0
#elif defined(__linux__)
#define BME_OS_WINDOWS 0
#define BME_OS_LINUX   1
#else
#error "`bme` requires Windows or Linux"
#endif

#if !defined(_M_X64) && !defined(__x86_64__)
#error "`bme` requires x86-64"
#endif
