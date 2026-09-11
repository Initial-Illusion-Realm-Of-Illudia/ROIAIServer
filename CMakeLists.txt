cmake_minimum_required(VERSION 3.24)
project(airoi_server C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# Static everything: a single self-contained server executable.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(FLECS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLECS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG        v4.1.6
    GIT_SHALLOW    TRUE)

FetchContent_Declare(libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG        v1.52.1
    GIT_SHALLOW    TRUE)

# uWebSockets is header-only; we build its uSockets submodule ourselves below.
# Only pull the uSockets submodule (skip boringssl/libdeflate: this build has no TLS).
FetchContent_Declare(uwebsockets
    GIT_REPOSITORY         https://github.com/uNetworking/uWebSockets.git
    GIT_TAG                v20.80.0
    GIT_SHALLOW            TRUE
    GIT_SUBMODULES         uSockets
    GIT_SUBMODULES_RECURSE FALSE)

FetchContent_MakeAvailable(flecs libuv uwebsockets)

# ---------------------------------------------------------------------------
# uSockets (uWebSockets' socket layer) on the libuv event loop, without TLS.
# ---------------------------------------------------------------------------
set(USOCKETS_SRC ${uwebsockets_SOURCE_DIR}/uSockets/src)
add_library(usockets STATIC
    ${USOCKETS_SRC}/bsd.c
    ${USOCKETS_SRC}/context.c
    ${USOCKETS_SRC}/loop.c
    ${USOCKETS_SRC}/socket.c
    ${USOCKETS_SRC}/udp.c
    ${USOCKETS_SRC}/eventing/libuv.c)
target_include_directories(usockets PUBLIC ${USOCKETS_SRC})
target_include_directories(usockets SYSTEM PUBLIC ${libuv_SOURCE_DIR}/include)
target_compile_definitions(usockets PUBLIC LIBUS_USE_LIBUV LIBUS_NO_SSL)

if(TARGET uv::uv)
    target_link_libraries(usockets PUBLIC uv::uv)
elseif(TARGET uv_a)
    target_link_libraries(usockets PUBLIC uv_a)
else()
    target_link_libraries(usockets PUBLIC uv)
endif()

if(WIN32)
    target_link_libraries(usockets PUBLIC ws2_32)
    target_compile_definitions(usockets PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# The MMO server
# ---------------------------------------------------------------------------
add_executable(mmo_server
    src/main.cpp
    src/NetServer.cpp
    src/Simulation.cpp)
target_include_directories(mmo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(mmo_server PRIVATE
    AIROI_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
target_link_libraries(mmo_server PRIVATE usockets)

if(TARGET flecs::flecs)
    target_link_libraries(mmo_server PRIVATE flecs::flecs)
else()
    target_link_libraries(mmo_server PRIVATE flecs)
endif()

if(MSVC)
    target_compile_options(mmo_server PRIVATE /W3 /utf-8)
    target_compile_options(usockets PRIVATE /utf-8)
endif()

set_source_files_properties(src/NetServer.cpp PROPERTIES
    COMPILE_OPTIONS "/wd4324")  # uWS alignment padding warnings

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES
    src/main.cpp src/NetServer.cpp src/Simulation.cpp)
