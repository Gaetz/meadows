cmake_minimum_required(VERSION 3.25)
project(true-adventurer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies are pinned here and fetched at configure time, identically on
# every OS (§3.1). Set the CPM_SOURCE_CACHE env var to share downloads between
# build directories.
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME SDL3
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_EXAMPLES OFF"
)

CPMAddPackage("gh:gabime/spdlog@1.17.0")
CPMAddPackage("gh:g-truc/glm#1.0.1")

function(meadows_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

add_subdirectory(engine)
add_subdirectory(game)
