cmake_minimum_required(VERSION 3.28)
project(mic_multi_index LANGUAGES CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
#  Header-only INTERFACE library: target  mic::mic
# ---------------------------------------------------------------------------
add_library(mic INTERFACE)
add_library(mic::mic ALIAS mic)
target_include_directories(mic INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mic INTERFACE cxx_std_23)

# MSVC needs /std:c++latest for the bleeding-edge bits and conformant preprocessor.
if(MSVC)
    target_compile_options(mic INTERFACE /Zc:__cplusplus /permissive-)
endif()

option(MIC_BUILD_TESTS      "Build the mic test suite"       ${PROJECT_IS_TOP_LEVEL})
option(MIC_BUILD_EXAMPLES   "Build the mic example programs" ${PROJECT_IS_TOP_LEVEL})
option(MIC_BUILD_BENCHMARKS "Build the mic benchmarks (optimised)" OFF)

if(MIC_BUILD_TESTS)
    enable_testing()
    add_executable(mic_tests tests/test_main.cpp tests/test_tu2.cpp)
    target_link_libraries(mic_tests PRIVATE mic::mic)
    if(MSVC)
        target_compile_options(mic_tests PRIVATE /W4 /std:c++latest)
    else()
        target_compile_options(mic_tests PRIVATE -Wall -Wextra -std=c++23)
    endif()
    add_test(NAME mic_tests COMMAND mic_tests)
endif()

if(MIC_BUILD_EXAMPLES)
    file(GLOB MIC_EXAMPLES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
    foreach(ex ${MIC_EXAMPLES})
        get_filename_component(name ${ex} NAME_WE)
        add_executable(mic_example_${name} ${ex})
        target_link_libraries(mic_example_${name} PRIVATE mic::mic)
        if(MSVC)
            target_compile_options(mic_example_${name} PRIVATE /W4 /std:c++latest)
        else()
            target_compile_options(mic_example_${name} PRIVATE -Wall -Wextra -std=c++23)
        endif()
    endforeach()
endif()

if(MIC_BUILD_BENCHMARKS)
    file(GLOB MIC_BENCHES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp)
    foreach(b ${MIC_BENCHES})
        get_filename_component(name ${b} NAME_WE)
        add_executable(mic_bench_${name} ${b})
        target_link_libraries(mic_bench_${name} PRIVATE mic::mic)
        if(MSVC)
            target_compile_options(mic_bench_${name} PRIVATE /O2 /DNDEBUG /std:c++latest)
        else()
            target_compile_options(mic_bench_${name} PRIVATE -O3 -DNDEBUG -std=c++23)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
#  Install (header-only)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS mic EXPORT micTargets)
install(EXPORT micTargets NAMESPACE mic:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mic)
