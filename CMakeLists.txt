cmake_minimum_required(VERSION 3.16)

project(covscript-process)
include_directories(include)

if (DEFINED ENV{CS_DEV_PATH})
    message("-- CovScript SDK detected at $ENV{CS_DEV_PATH}")
    include("$ENV{CS_DEV_PATH}/csbuild.cmake")
    include_directories($ENV{CS_DEV_PATH}/include)
    link_directories($ENV{CS_DEV_PATH}/lib)
else ()
    message(FATAL_ERROR "-- CovScript SDK not detected. Please set environment variable CS_DEV_PATH")
endif ()

aux_source_directory(mozart++ SRCS)
aux_source_directory(src SRCS_IMPL)

add_library(mozart STATIC
        ${SRCS}
        ${SRCS_IMPL}
        )

add_library(process SHARED process.cpp)
target_link_libraries(process mozart covscript)

set_target_properties(process PROPERTIES OUTPUT_NAME process)
set_target_properties(process PROPERTIES PREFIX "")
set_target_properties(process PROPERTIES SUFFIX ".cse")