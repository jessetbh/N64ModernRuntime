cmake_minimum_required(VERSION 3.20)

project(librecomp)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

# Define the library
add_library(librecomp STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cont.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config_option.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eep.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/euc-jp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/extensions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/files.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flash.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/heap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/math_routines.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mods.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_events.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_hooks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_config_api.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/overlays.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pak.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/patcher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/print.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rsp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/save_backup_rules.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/save_backups.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/si.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultra_stubs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultra_translation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/o1heap/o1heap/o1heap.c"
)

target_include_directories(librecomp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/../ultramodern/include"
    "${PROJECT_SOURCE_DIR}/../thirdparty"
    "${PROJECT_SOURCE_DIR}/../thirdparty/concurrentqueue"
    "${PROJECT_SOURCE_DIR}/../thirdparty/o1heap"
)
target_include_directories(librecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/librecomp"
)

target_compile_options(librecomp PRIVATE
#    -Wall
#    -Wextra
    -Wno-unused-parameter
)

if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

add_subdirectory(${PROJECT_SOURCE_DIR}/../thirdparty/miniz ${CMAKE_CURRENT_BINARY_DIR}/miniz)
add_subdirectory(${PROJECT_SOURCE_DIR}/../N64Recomp ${CMAKE_CURRENT_BINARY_DIR}/N64Recomp EXCLUDE_FROM_ALL)

target_link_libraries(librecomp PRIVATE ultramodern N64Recomp LiveRecomp)
target_link_libraries(librecomp PUBLIC miniz)

# [wcw] Unit tests for librecomp's own decision logic (save backup rules: aki-roadmap#50). Plain C++ (no game, no ROM),
# built and run as part of every librecomp build, so a failing test fails the game's build (as rt64's and recompui's
# wcw tests do). LIBRECOMP_WCW_TESTS=OFF skips them.
option(LIBRECOMP_WCW_TESTS "Build and run the wcw unit tests with librecomp" ON)
if (LIBRECOMP_WCW_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(librecomp_wcw_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/save_backup_rules_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/save_backup_rules.cpp"
    )
    target_include_directories(librecomp_wcw_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set(LIBRECOMP_WCW_TESTS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/librecomp_wcw_tests.passed")
    add_custom_command(
        OUTPUT "${LIBRECOMP_WCW_TESTS_STAMP}"
        COMMAND librecomp_wcw_tests
        COMMAND ${CMAKE_COMMAND} -E touch "${LIBRECOMP_WCW_TESTS_STAMP}"
        DEPENDS librecomp_wcw_tests
        COMMENT "Running librecomp wcw unit tests"
        VERBATIM
    )
    add_custom_target(librecomp_wcw_tests_run DEPENDS "${LIBRECOMP_WCW_TESTS_STAMP}")
    add_dependencies(librecomp librecomp_wcw_tests_run)
endif()
