cmake_minimum_required(VERSION 3.20.0)

include(FetchContent)

FetchContent_Declare (
    asiosdk
    URL      https://www.steinberg.net/asiosdk
)

FetchContent_MakeAvailable(asiosdk)

configure_file(${CMAKE_CURRENT_LIST_DIR}/patch/asiosys.h ${asiosdk_SOURCE_DIR}/common/asiosys.h COPYONLY)

add_library(asio STATIC
    ${asiosdk_SOURCE_DIR}/common/asiosys.h
    ${asiosdk_SOURCE_DIR}/common/asio.cpp
    ${asiosdk_SOURCE_DIR}/common/asio.h
    ${asiosdk_SOURCE_DIR}/common/asiodrvr.cpp
    ${asiosdk_SOURCE_DIR}/common/asiodrvr.h
)

target_include_directories(asio
    PUBLIC
        ${asiosdk_SOURCE_DIR}/common
)
