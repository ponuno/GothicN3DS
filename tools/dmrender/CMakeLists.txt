# A separate little program, not part of the main build (it needs GothicKit's
# dmusic, https://github.com/GothicKit/dmusic):
#   cmake -S tools/dmrender -B build-dmrender -DDMUSIC_DIR=/path/to/dmusic
#   cmake --build build-dmrender
# Without DMUSIC_DIR the library is fetched from GitHub.
cmake_minimum_required(VERSION 3.14)
project(dmrender LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(DM_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(DM_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
if (DMUSIC_DIR)
  add_subdirectory(${DMUSIC_DIR} dmusic EXCLUDE_FROM_ALL)
else ()
  include(FetchContent)
  FetchContent_Declare(dmusic GIT_REPOSITORY https://github.com/GothicKit/dmusic.git GIT_TAG main)
  FetchContent_MakeAvailable(dmusic)
endif ()
add_executable(dmrender dmrender.c)
target_link_libraries(dmrender PRIVATE dmusic m)
