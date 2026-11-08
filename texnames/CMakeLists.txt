# Texture pack names from other N64 renderers, for rt64 (aki-roadmap#93). See README.md. The module only needs rt64's
# interface header; the application links both.
add_library(wcw_texnames STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wcw_jabo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wcw_provider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wcw_rice.cpp"
)
set(WCW_TEXNAMES_RT64_INCLUDE "$<TARGET_PROPERTY:rt64,SOURCE_DIR>/src")
target_include_directories(wcw_texnames PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${WCW_TEXNAMES_RT64_INCLUDE}"
)
target_include_directories(wcw_texnames PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Plain C++ tests, run on every build; a failing test fails the game's build, as the other wcw suites do.
option(WCW_TEXNAMES_TESTS "Build and run the texnames unit tests" ON)
if (WCW_TEXNAMES_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(wcw_texnames_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wcw_rice_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wcw_jabo_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/wcw_provider_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wcw_jabo.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wcw_provider.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wcw_rice.cpp"
    )
    target_include_directories(wcw_texnames_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${WCW_TEXNAMES_RT64_INCLUDE}"
    )
    set(WCW_TEXNAMES_TESTS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/wcw_texnames_tests.passed")
    add_custom_command(
        OUTPUT "${WCW_TEXNAMES_TESTS_STAMP}"
        COMMAND wcw_texnames_tests
        COMMAND ${CMAKE_COMMAND} -E touch "${WCW_TEXNAMES_TESTS_STAMP}"
        DEPENDS wcw_texnames_tests
        COMMENT "Running texnames unit tests"
        VERBATIM
    )
    add_custom_target(wcw_texnames_tests_run DEPENDS "${WCW_TEXNAMES_TESTS_STAMP}")
    add_dependencies(wcw_texnames wcw_texnames_tests_run)
endif()
