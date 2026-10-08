add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE g3tools g3soft)
target_compile_options(test_core PRIVATE ${G3DS_WARNINGS})
add_test(NAME core COMMAND test_core ${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_script test_script.cpp)
target_link_libraries(test_script PRIVATE g3tools)
target_compile_options(test_script PRIVATE ${G3DS_WARNINGS})
if (G3DS_WITH_ZENKIT)
    target_compile_definitions(test_script PRIVATE G3DS_WITH_ZENKIT=1)
endif ()
add_test(NAME script COMMAND test_script ${CMAKE_CURRENT_BINARY_DIR})

# The console backend (platform/3ds/gfx_c3d.cpp) against a stand-in for libctru
# and citro3d that checks which buffer and which memory the GPU owns when.
add_executable(test_frames test_frames.cpp mock3ds/mock3ds.cpp ${CMAKE_SOURCE_DIR}/platform/3ds/gfx_c3d.cpp)
target_include_directories(test_frames PRIVATE mock3ds ${CMAKE_SOURCE_DIR}/platform/3ds)
target_link_libraries(test_frames PRIVATE g3tools g3engine)
target_compile_options(test_frames PRIVATE ${G3DS_WARNINGS})
add_test(NAME frames COMMAND test_frames ${CMAKE_CURRENT_BINARY_DIR})

# Full chain through the command-line tools: bake -> data.pak -> runtime -> frame.
add_test(NAME e2e_synth
         COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/e2e.sh synth $<TARGET_FILE:g3ds-bake> $<TARGET_FILE:g3ds-host>
                 ${CMAKE_CURRENT_BINARY_DIR}/e2e_synth)

if (G3DS_WITH_ZENKIT)
    # Writes two small worlds in ZenGin's own formats (.ZEN, .MSH, .MRM, .MDH,
    # .MDM, .MDL, -C.TEX) and checks that the importer reads them back correctly.
    add_executable(test_zen test_zen.cpp)
    set_target_properties(test_zen PROPERTIES CXX_STANDARD 20)
    target_link_libraries(test_zen PRIVATE g3tools zenkit)
    target_include_directories(test_zen SYSTEM PRIVATE $<TARGET_PROPERTY:zenkit,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_options(test_zen PRIVATE ${G3DS_WARNINGS})
    # ZenKit's checkout carries a few genuine game files for its own tests; the
    # importer is run on them too. They are read in place, never copied here.
    add_test(NAME zen_import COMMAND test_zen ${CMAKE_CURRENT_BINARY_DIR}/zen_fixture ${zenkit_SOURCE_DIR}/tests/samples)

    add_test(NAME e2e_zen
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/e2e.sh zen $<TARGET_FILE:g3ds-bake> $<TARGET_FILE:g3ds-host>
                     ${CMAKE_CURRENT_BINARY_DIR}/e2e_zen ${CMAKE_CURRENT_BINARY_DIR}/zen_fixture)
    set_tests_properties(e2e_zen PROPERTIES DEPENDS zen_import)
endif ()
