cmake_minimum_required(VERSION 3.20.0)

project("PipeWire 2 ASIO")

add_subdirectory(extern)

target_sources(asio
    PRIVATE 
        source/pipewire_asio.cpp
        source/utils.cpp
        source/utils.h
        source/types.h
)

target_link_libraries(asio 
    ${PIPEWIRE_LIBRARIES}
    m # Math library
)

target_include_directories(asio 
    PUBLIC
        ${PIPEWIRE_INCLUDE_DIRS}
)

target_compile_options(asio 
    PUBLIC
        ${PIPEWIRE_CFLAGS_OTHER}
        -fpermissive
)

target_compile_definitions(asio
    PRIVATE
        APP_NAME="ASIO Host"
)

add_executable(pipewire2asio_tester 
    test/main.cpp
)

target_link_libraries(pipewire2asio_tester 
    PRIVATE
        asio
)

set_property(
    TARGET 
        asio
    PROPERTY 
        POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(pipewire2asio_tester
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()