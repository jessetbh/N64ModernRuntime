cmake_minimum_required(VERSION 3.20)

project(N64ModernRuntime)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(ultramodern)
add_subdirectory(librecomp)

# Texture pack names from other N64 renderers, for rt64 (texnames/README.md). Needs rt64's interface header.
if (TARGET rt64)
    add_subdirectory(texnames)
endif()
