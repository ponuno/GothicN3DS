# Host build: asset baker, PC runtime with the software backend, tests.
# The 3DS binary is built with the devkitARM Makefile in this directory.
cmake_minimum_required(VERSION 3.16)
project(gothic3ds LANGUAGES C CXX)
if (MINGW)
    # printf with %zu, %lld and friends as the standard has them (MinGW's own check otherwise warns on every one).
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

option(G3DS_WITH_ZENKIT "Build the ZenGin importer (fetches ZenKit)" ON)
option(G3DS_BAKER_ONLY "Build only g3ds-bake (no runtime, no tests): for Windows, see windows/README.md" OFF)
if (G3DS_BAKER_ONLY)
    set(G3DS_BUILD_TESTS OFF)
else ()
    option(G3DS_BUILD_TESTS "Build the tests" ON)
endif ()

set(G3DS_WARNINGS -Wall -Wextra -Wshadow)
find_package(Threads REQUIRED)

# ---- third party (vendored, permissive licences) ----
add_library(lz4 STATIC third_party/lz4/lz4.c)
target_include_directories(lz4 PUBLIC third_party/lz4)
add_library(lz4hc STATIC third_party/lz4/lz4hc.c)
target_link_libraries(lz4hc PUBLIC lz4)
add_library(rg_etc1 STATIC third_party/rg_etc1/rg_etc1.cpp)
target_include_directories(rg_etc1 PUBLIC third_party/rg_etc1)
# Third-party code as it is: no warnings, and its unaligned table reads are
# not ours to report in a sanitizer build.
target_compile_options(rg_etc1 PRIVATE -w -fno-sanitize=alignment)

# ---- runtime: the same sources the 3DS Makefile compiles ----
add_library(g3core STATIC core/src/pak.cpp core/src/tex.cpp)
target_include_directories(g3core PUBLIC core/include)
target_link_libraries(g3core PUBLIC lz4)
target_compile_options(g3core PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

if (NOT G3DS_BAKER_ONLY)
# Threads, locks and the clock for this platform (the 3DS has platform/3ds/sys_3ds.cpp).
add_library(g3sys STATIC platform/host/sys_host.cpp)
target_include_directories(g3sys PUBLIC engine core/include)
target_link_libraries(g3sys PUBLIC Threads::Threads)
target_compile_options(g3sys PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)
endif ()

# The script machine and the game's rules. The baker runs them too: it has to
# know what the scripts put into the world to bake what that needs.
add_library(g3script STATIC engine/script.cpp engine/game.cpp engine/game_dialog.cpp engine/game_doc.cpp engine/game_extra.cpp engine/game_save.cpp engine/game_items.cpp engine/fight.cpp engine/game_magic.cpp)
target_include_directories(g3script PUBLIC engine)
target_link_libraries(g3script PUBLIC g3core)
target_compile_options(g3script PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

if (NOT G3DS_BAKER_ONLY)
add_library(g3engine STATIC engine/world.cpp engine/point_grid.cpp engine/app.cpp engine/collision.cpp engine/player.cpp engine/sky.cpp engine/barrier.cpp engine/weather.cpp
            engine/loader.cpp engine/readahead.cpp engine/pacer.cpp engine/model.cpp engine/hero.cpp engine/npcs.cpp engine/waynet.cpp engine/mobs.cpp engine/rooms.cpp engine/npc_world.cpp engine/triggers.cpp engine/npc_ai.cpp engine/npc_perc.cpp engine/npc_dialog.cpp engine/dialog_text.cpp engine/app_talk.cpp engine/app_saves.cpp engine/app_doc.cpp engine/app_magic.cpp engine/app_ranged.cpp engine/recorder.cpp
            engine/npc_fight.cpp engine/npc_items.cpp engine/ui.cpp engine/menu.cpp engine/start_menu.cpp engine/saves.cpp engine/settings.cpp engine/hud.cpp engine/inventory.cpp engine/panel.cpp engine/loot.cpp engine/particles.cpp engine/shadows.cpp engine/puddles.cpp engine/blood_stains.cpp engine/rain.cpp engine/ripples.cpp engine/weather_sfx.cpp engine/godrays.cpp engine/clouds.cpp engine/grass.cpp engine/lamps.cpp engine/chests.cpp engine/containers.cpp engine/interact.cpp engine/trade.cpp engine/lang.cpp engine/trace.cpp engine/sound.cpp engine/music.cpp engine/speech.cpp)
target_include_directories(g3engine PUBLIC engine)
target_link_libraries(g3engine PUBLIC g3core g3sys g3script)
target_compile_options(g3engine PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

add_library(g3soft STATIC platform/host/gfx_soft.cpp platform/host/audio_host.cpp)
target_include_directories(g3soft PUBLIC platform/host)
target_link_libraries(g3soft PUBLIC g3engine m)
target_compile_options(g3soft PRIVATE ${G3DS_WARNINGS} -fno-exceptions -fno-rtti)

add_executable(g3ds-host platform/host/main.cpp)
target_include_directories(g3ds-host PRIVATE third_party/stb)
target_link_libraries(g3ds-host PRIVATE g3soft)
endif ()

# ---- tools ----
set(G3DS_BAKE_SOURCES
    tools/common/pak_writer.cpp
    tools/bake/texture_bake.cpp
    tools/bake/bake.cpp
    tools/bake/chunker.cpp
    tools/bake/model_bake.cpp
    tools/bake/script_bake.cpp
    tools/bake/populate.cpp
    tools/bake/sound_bake.cpp
    tools/bake/item_icon.cpp
    tools/bake/fight_ai.cpp
    tools/bake/dialogue_bake.cpp
    tools/bake/font_bake.cpp
    tools/bake/daedalus.cpp
    tools/bake/synth_script.cpp
    tools/bake/synth_actor.cpp
    tools/bake/ground.cpp
    tools/bake/vertex_fx.cpp
    tools/bake/light_bake.cpp
    tools/bake/sun_bake.cpp
    tools/bake/far_lod.cpp
    tools/bake/visibility.cpp
    tools/bake/water_grid.cpp
    tools/bake/water_texture.cpp
    tools/bake/water_fx.cpp
    tools/bake/grass_report.cpp
    tools/bake/grass_map.cpp
    tools/bake/grass_bake.cpp
    tools/bake/weather_sfx_bake.cpp
    tools/bake/ground_fx.cpp
    tools/bake/lod.cpp
    tools/bake/vcache.cpp
    tools/bake/model_simplify.cpp
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
    # Third-party code as it is: reading nothing into no buffer (a file cut
    # short) is harmless there and not ours to report in a sanitizer build.
    target_compile_options(zenkit PRIVATE -fno-sanitize=nonnull-attribute)
    list(APPEND G3DS_BAKE_SOURCES tools/bake/import_zen.cpp tools/bake/script_check.cpp)
else ()
    list(APPEND G3DS_BAKE_SOURCES tools/bake/import_zen_stub.cpp)
endif ()

add_library(g3tools STATIC ${G3DS_BAKE_SOURCES})
# The baker's own rain and thunder (tools/bake/weather_sfx/*.wav) are built into it (weather_sfx_bake.cpp, .incbin).
set(G3DS_WEATHER_SFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/bake/weather_sfx)
set_source_files_properties(tools/bake/weather_sfx_bake.cpp PROPERTIES
    COMPILE_DEFINITIONS "G3DS_WEATHER_SFX_DIR=\"${G3DS_WEATHER_SFX_DIR}\""
    OBJECT_DEPENDS "${G3DS_WEATHER_SFX_DIR}/G3_RAIN.wav;${G3DS_WEATHER_SFX_DIR}/G3_THUNDER_NEAR.wav;${G3DS_WEATHER_SFX_DIR}/G3_THUNDER_ROLL.wav;${G3DS_WEATHER_SFX_DIR}/G3_THUNDER_FAR.wav")
target_include_directories(g3tools PUBLIC tools/common tools/bake)
target_link_libraries(g3tools PUBLIC g3core g3script lz4hc rg_etc1 Threads::Threads)
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
if (WIN32)
    # One .exe that needs no DLLs next to it.
    target_link_options(g3ds-bake PRIVATE -static)
endif ()

if (WIN32 AND G3DS_BAKER_ONLY)
    # The window the player uses (windows/launcher.cpp) and the music renderer it calls.
    add_executable(Gothic3DS-Baker WIN32 windows/launcher.cpp)
    set_target_properties(Gothic3DS-Baker PROPERTIES CXX_STANDARD 17)
    target_link_options(Gothic3DS-Baker PRIVATE -municode -static)
    target_link_libraries(Gothic3DS-Baker PRIVATE comctl32 ole32 shell32 uuid)
    add_subdirectory(tools/dmrender)
    target_link_options(dmrender PRIVATE -static)
endif ()

if (G3DS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
