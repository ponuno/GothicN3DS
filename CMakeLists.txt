# Host build: asset baker, PC runtime with the software backend, tests.
# The 3DS binary is built with the devkitARM Makefile in this directory.
cmake_minimum_required(VERSION 3.16)
project(gothic3ds LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

option(G3DS_WITH_ZENKIT "Build the ZenGin importer (fetches ZenKit)" ON)
option(G3DS_BUILD_TESTS "Build the tests" ON)

set(G3DS_WARNINGS -Wall -Wextra -Wshadow)
find_package(Threads REQUIRED)

# ---- third party (vendored, permissive licences) ----
add_library(lz4 STATIC third_party/lz4/lz4.c)
target_include_directories(lz4 PUBLIC third_party/lz4)
add_library(lz4hc STATIC third_party/lz4/lz4hc.c)
target_link_libraries(lz4hc PUBLIC lz4)
add_library(rg_etc1 STATIC third_party/rg_etc1/rg_etc1.cpp)
target_include_directories(rg_etc1 PUBLIC third_party/rg_etc1)
target_compile_options(rg_etc1 PRIVATE -w)

# ---- runtime: the same sources the 3DS Makefile compiles ----
add_library(g3core STATIC core/src/pak.cpp core/src/tex.cpp)
target_include_directories(g3core PUBLIC core/include)
target_link_libraries(g3core PUBLIC lz4)
target_compile_options(g3core PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

add_library(g3engine STATIC engine/world.cpp engine/app.cpp)
target_include_directories(g3engine PUBLIC engine)
target_link_libraries(g3engine PUBLIC g3core)
target_compile_options(g3engine PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

add_library(g3soft STATIC platform/host/gfx_soft.cpp)
target_include_directories(g3soft PUBLIC platform/host)
target_link_libraries(g3soft PUBLIC g3engine m)
target_compile_options(g3soft PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

add_executable(g3ds-host platform/host/main.cpp)
target_include_directories(g3ds-host PRIVATE third_party/stb)
target_link_libraries(g3ds-host PRIVATE g3soft)

# ---- tools ----
set(G3DS_BAKE_SOURCES
    tools/common/pak_writer.cpp
    tools/bake/texture_bake.cpp
    tools/bake/chunker.cpp
    tools/bake/synth.cpp)

if (G3DS_WITH_ZENKIT)
    include(FetchContent)
    set(ZK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZK_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
    set(ZK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    # Pinned: the importer is written against this revision's API.
    # Offline: pass -DFETCHCONTENT_SOURCE_DIR_ZENKIT=/path/to/ZenKit (with submodules).
    FetchContent_Declare(zenkit
        GIT_REPOSITORY https://github.com/GothicKit/ZenKit.git
        GIT_TAG ddf27decd5eeb48ec715e6e66e5f5072c51d88ee)
    FetchContent_MakeAvailable(zenkit)
    list(APPEND G3DS_BAKE_SOURCES tools/bake/import_zen.cpp)
else ()
    list(APPEND G3DS_BAKE_SOURCES tools/bake/import_zen_stub.cpp)
endif ()

add_library(g3tools STATIC ${G3DS_BAKE_SOURCES})
target_include_directories(g3tools PUBLIC tools/common tools/bake)
target_link_libraries(g3tools PUBLIC g3core lz4hc rg_etc1 Threads::Threads)
target_compile_options(g3tools PRIVATE ${G3DS_WARNINGS})
# The tools are C++20 because ZenKit's headers are; the runtime stays C++17.
set_target_properties(g3tools PROPERTIES CXX_STANDARD 20)
if (G3DS_WITH_ZENKIT)
    target_link_libraries(g3tools PRIVATE zenkit)
    # Keep ZenKit's own header warnings out of our build log.
    target_include_directories(g3tools SYSTEM PRIVATE $<TARGET_PROPERTY:zenkit,INTERFACE_INCLUDE_DIRECTORIES>)
endif ()

add_executable(g3ds-bake tools/bake/main.cpp)
target_link_libraries(g3ds-bake PRIVATE g3tools)
target_compile_options(g3ds-bake PRIVATE ${G3DS_WARNINGS})

if (G3DS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
