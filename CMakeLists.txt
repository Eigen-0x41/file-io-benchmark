cmake_minimum_required(VERSION 3.25)
project(file_io_benchmark LANGUAGES CXX)

option(FILE_IO_BENCHMARK_ENABLE_LTO "Enable interprocedural optimization in Release builds" OFF)

add_executable(file_io_benchmark
    src/main.cpp
)

target_compile_features(file_io_benchmark PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(file_io_benchmark PRIVATE /W4 /permissive-)
else()
    target_compile_options(file_io_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FILE_IO_BENCHMARK_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set_property(TARGET file_io_benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "IPO/LTO is unavailable: ${ipo_error}")
    endif()
endif()
