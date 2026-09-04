cmake_minimum_required(VERSION 3.25)

project(LightLaunch VERSION 0.1.1 LANGUAGES CXX RC)

option(LIGHTLAUNCH_BUILD_TESTS "Build LightLaunch tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(LIGHTLAUNCH_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LIGHTLAUNCH_GENERATED_DIRECTORY}")
configure_file(
    resources/LightLaunchVersion.h.in
    "${LIGHTLAUNCH_GENERATED_DIRECTORY}/LightLaunchVersion.h"
    @ONLY
)

add_library(lightlaunch_core STATIC
    src/config_store.cpp
)

target_include_directories(lightlaunch_core PUBLIC src)
target_compile_definitions(lightlaunch_core PUBLIC
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(lightlaunch_core PUBLIC shell32 shlwapi ole32 uuid)

add_executable(LightLaunch WIN32
    src/main.cpp
    src/application.cpp
    src/background_dialog.cpp
    src/background_image.cpp
    src/input_dialog.cpp
    src/rail_appearance_dialog.cpp
    resources/LightLaunch.rc
)

target_include_directories(LightLaunch PRIVATE
    src
    "${LIGHTLAUNCH_GENERATED_DIRECTORY}"
)
target_link_libraries(LightLaunch PRIVATE
    lightlaunch_core
    comctl32
    comdlg32
    msimg32
    uxtheme
    windowscodecs
)

install(TARGETS LightLaunch RUNTIME DESTINATION .)

if(MSVC)
    foreach(target lightlaunch_core LightLaunch)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O1;/GL;/Gy;/Gw>)
    endforeach()
    target_link_options(LightLaunch PRIVATE
        /MANIFEST:NO
        $<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>
    )
else()
    foreach(target lightlaunch_core LightLaunch)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-Os;-ffunction-sections;-fdata-sections>)
    endforeach()
    target_link_options(LightLaunch PRIVATE
        -municode
        -mwindows
        -static-libgcc
        -static-libstdc++
        $<$<CONFIG:Release>:-Wl,--gc-sections>
    )
endif()

if(LIGHTLAUNCH_BUILD_TESTS)
    enable_testing()
    add_executable(lightlaunch_config_tests tests/config_store_tests.cpp)
    target_link_libraries(lightlaunch_config_tests PRIVATE lightlaunch_core)
    if(MSVC)
        target_compile_options(lightlaunch_config_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
        target_link_options(lightlaunch_config_tests PRIVATE $<$<CONFIG:Release>:/LTCG>)
    else()
        target_compile_options(lightlaunch_config_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(lightlaunch_config_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME config_store_roundtrip COMMAND lightlaunch_config_tests)
endif()
