cmake_minimum_required(VERSION 3.20)
project(BunCancel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(buncancel-dsp STATIC src/dsp/aec.cpp)
target_include_directories(buncancel-dsp PUBLIC src/dsp)
set_target_properties(buncancel-dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(buncancel-offline tools/offline.cpp)
target_link_libraries(buncancel-offline PRIVATE buncancel-dsp)

include(CTest)
if(BUILD_TESTING)
    add_executable(buncancel-dsp-tests tests/dsp/test_aec.cpp)
    target_link_libraries(buncancel-dsp-tests PRIVATE buncancel-dsp)
    add_test(NAME dsp COMMAND buncancel-dsp-tests)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME offline-cli COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/dsp/test_offline.py $<TARGET_FILE:buncancel-offline>)
endif()

option(BUNCANCEL_BUILD_VST3 "Build the experimental Windows x64 VST3" OFF)
if(BUNCANCEL_BUILD_VST3)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The reference capture plugin requires 64-bit Windows")
    endif()
    add_subdirectory(src/capture/windows)
    add_subdirectory(src/plugin)
endif()
