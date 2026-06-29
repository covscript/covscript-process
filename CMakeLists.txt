cmake_minimum_required(VERSION 3.16)

project(covscript-process)

# ---------------------------------------------------------------------------
# libuv (static) – async event loop backend
# ---------------------------------------------------------------------------
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "Build libuv as a static library" FORCE)
set(BUILD_TESTING     OFF CACHE BOOL "Disable libuv unit tests"         FORCE)
add_subdirectory(third_party/libuv EXCLUDE_FROM_ALL)

# libuv v1.52.1 has a const-qualification issue in src/win/util.c that
# GCC ≥ 14 (MinGW) now treats as an error.  Silence it for the libuv target only.
if (MINGW)
    target_compile_options(uv_a PRIVATE -Wno-incompatible-pointer-types -Wno-discarded-qualifiers)
endif ()

if (DEFINED ENV{CS_DEV_PATH})
    message("-- CovScript SDK detected at $ENV{CS_DEV_PATH}")
    include("$ENV{CS_DEV_PATH}/csbuild.cmake")
    # CovScript SDK's csbuild.cmake currently sets CMAKE_CXX_STANDARD=17.
    # Keep an explicit fallback here so standalone/incomplete SDK copies do not
    # silently fall back to compiler-default standards.
    if (NOT DEFINED CMAKE_CXX_STANDARD)
        set(CMAKE_CXX_STANDARD 17)
    endif ()
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(COVSCRIPT_SDK_INCLUDE_DIR "$ENV{CS_DEV_PATH}/include")
    set(COVSCRIPT_SDK_LIB_DIR "$ENV{CS_DEV_PATH}/lib")
else ()
    message(FATAL_ERROR "-- CovScript SDK not detected. Please set environment variable CS_DEV_PATH")
endif ()

set(MOZART_SOURCES
    src/core.cpp
    src/process.cpp
)

if (WIN32)
    list(APPEND MOZART_SOURCES
        src/process_win32.cpp
        src/process_win32_wait.cpp
    )
elseif (UNIX)
    list(APPEND MOZART_SOURCES
        src/process_unix.cpp
        src/process_unix_wait.cpp
    )
endif ()

add_library(mozart STATIC
    ${MOZART_SOURCES}
)

set_target_properties(mozart PROPERTIES POSITION_INDEPENDENT_CODE ON)
# process is a shared module; make sure static deps are PIC-friendly on ELF.
set_target_properties(uv_a PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(mozart
    PUBLIC
        include
        third_party/libuv/include
)

# mozart headers now include uv.h (process.hpp); ensure consistent
# WIN32_LEAN_AND_MEAN on MinGW so that windows.h (pulled in via the
# mozart++/core → io.hpp chain) does not pre-include winsock2.h before uv.h.
if (MINGW)
    target_compile_definitions(mozart PRIVATE WIN32_LEAN_AND_MEAN)
endif ()

target_link_libraries(mozart
    PRIVATE
        uv_a
)

add_library(process SHARED process.cpp)

target_include_directories(process
    PRIVATE
        include
        ${COVSCRIPT_SDK_INCLUDE_DIR}
)

# On MinGW, ensure winsock2.h is not pulled in by windows.h before uv.h loads it.
if (MINGW)
    target_compile_definitions(process PRIVATE WIN32_LEAN_AND_MEAN)
endif ()

target_link_directories(process
    PRIVATE
        ${COVSCRIPT_SDK_LIB_DIR}
)

target_link_libraries(process
    PRIVATE
        mozart
        covscript
        uv_a
)

set_target_properties(process PROPERTIES OUTPUT_NAME process)
set_target_properties(process PROPERTIES PREFIX "")
set_target_properties(process PROPERTIES SUFFIX ".cse")

# ---------------------------------------------------------------------------
# C++ unit tests for UV wrappers (exercise paths unreachable from scripts)
# ---------------------------------------------------------------------------
option(PROCESS_BUILD_TESTING "Build C++ unit tests for the UV async I/O wrappers" OFF)

if (PROCESS_BUILD_TESTING)
    enable_testing()

    add_executable(test_uv_fs tests/cpp/test_uv_fs.cpp)

    target_include_directories(test_uv_fs
        PRIVATE
            third_party/libuv/include
    )

    if (MINGW)
        target_compile_definitions(test_uv_fs PRIVATE WIN32_LEAN_AND_MEAN)
    endif ()

    target_link_libraries(test_uv_fs
        PRIVATE
            uv_a
    )

    add_test(NAME test_uv_fs COMMAND test_uv_fs)
endif ()