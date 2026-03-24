cmake_minimum_required(VERSION 3.11...3.31)
project(stream2_example)

# Place build artifacts under bin/lib alongside the chosen build directory.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CheckCCompilerFlag)
find_package(Threads REQUIRED)

if(NOT WIN32)
    option(BUILD_LIBZMQ "Build ZeroMQ library from source" OFF)
else()
    option(BUILD_LIBZMQ "Build ZeroMQ library from source" ON)
endif()

if(BUILD_LIBZMQ)
    include(FetchContent)

    # CMake 4.0+ removed compatibility with cmake_minimum_required < 3.5
    # libzmq v4.3.4 uses an older version, so we need this workaround
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()

    # Set options before fetching
    set(BUILD_SHARED OFF CACHE INTERNAL "")
    set(BUILD_STATIC ON CACHE INTERNAL "")
    set(ENABLE_CURVE OFF CACHE BOOL "")
    set(ENABLE_DRAFTS OFF CACHE BOOL "")

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
        # CMake 3.28+ supports EXCLUDE_FROM_ALL in FetchContent_Declare
        FetchContent_Declare(
            libzmq
            GIT_REPOSITORY https://github.com/zeromq/libzmq.git
            GIT_TAG 4097855ddaaa65ed7b5e8cb86d143842a594eebd # v4.3.4
            EXCLUDE_FROM_ALL
            )
        FetchContent_MakeAvailable(libzmq)
    else()
        # Older CMake requires manual FetchContent_Populate + add_subdirectory
        FetchContent_Declare(
            libzmq
            GIT_REPOSITORY https://github.com/zeromq/libzmq.git
            GIT_TAG 4097855ddaaa65ed7b5e8cb86d143842a594eebd # v4.3.4
            )
        FetchContent_GetProperties(libzmq)
        if(NOT libzmq_POPULATED)
            FetchContent_Populate(libzmq)
            add_subdirectory("${libzmq_SOURCE_DIR}" "${libzmq_BINARY_DIR}" EXCLUDE_FROM_ALL)
        endif()
    endif()

    unset(ZEROMQ_LIBRARY CACHE)
    set(LIBZMQ_TARGET libzmq-static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PC_LIBZMQ QUIET libzmq)
    find_library(ZeroMQ_LIBRARY
        NAMES libzmq.so libzmq.dylib libzmq.dll
        PATHS ${PC_LIBZMQ_LIBDIR} ${PC_LIBZMQ_LIBRARY_DIRS}
        REQUIRED
        )
    add_library(libzmq SHARED IMPORTED)
    set_property(TARGET libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${PC_LIBZMQ_INCLUDE_DIRS})
    set_property(TARGET libzmq PROPERTY IMPORTED_LOCATION ${ZeroMQ_LIBRARY})
    set(LIBZMQ_TARGET libzmq)
endif()

add_subdirectory(third_party EXCLUDE_FROM_ALL)

set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    )

if(NOT "${CMAKE_C_COMPILER_ID}" MATCHES "MSVC")
    check_c_compiler_flag("-mf16c" COMPILER_SUPPORTS_MF16C)
    if(COMPILER_SUPPORTS_MF16C)
        add_compile_options(-mf16c)
    endif()
    add_compile_options(-Wall -Wextra)
endif()

add_library(stream2 STATIC
    src/stream2.c
    src/stream2.h
    )
target_link_libraries(stream2 PRIVATE
    tinycbor
    )

# Shared helper library for examples
add_library(stream2_helpers STATIC
    src/stream2_common.c
    src/stream2_common.h
    src/stream2_stats.c
    src/stream2_stats.h
    src/stream2_decompress.c
    src/stream2_decompress.h
    src/stream2_image_buffer.c
    src/stream2_image_buffer.h
    src/tiff_writer.c
    src/tiff_writer.h
    )
target_link_libraries(stream2_helpers PUBLIC
    ${LIBZMQ_TARGET}
    compression
    stream2
    tinycbor
    Threads::Threads
    )

add_executable(stream2_dump stream2_dump.c)
target_link_libraries(stream2_dump stream2_helpers)

add_executable(stream2_buffer stream2_buffer.c)
target_link_libraries(stream2_buffer stream2_helpers)

add_executable(stream2_buffer_decode stream2_buffer_decode.c)
target_link_libraries(stream2_buffer_decode stream2_helpers)

add_executable(DectrisStream2Receiver_linux DectrisStream2Receiver_linux.c)
target_link_libraries(DectrisStream2Receiver_linux stream2_helpers)

add_executable(stream2_bifurcator stream2_bifurcator.c)
target_link_libraries(stream2_bifurcator stream2_helpers)

if(WIN32)
    add_executable(DectrisStream2Demo_windows DectrisStream2Demo_windows.c)
    target_link_libraries(DectrisStream2Demo_windows
        stream2_helpers
        user32
        gdi32
        comctl32
        iphlpapi
        ws2_32
    )
    
    add_executable(start_stream_eigerclient start_stream_eigerclient.c)
    target_link_libraries(start_stream_eigerclient
        winhttp
    )
endif()

# Add clean-build target (works on all platforms, especially useful for Windows/Visual Studio)
# Usage: cmake --build . --target clean-build
# Note: For Makefile generators, 'make clean' already exists and works. This target
# provides a way to clean everything including CMake cache files, binaries, and intermediate files.
add_custom_target(clean-build
    COMMAND ${CMAKE_COMMAND} -E echo "Cleaning build artifacts..."
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/CMakeFiles
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/CMakeCache.txt
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/cmake_install.cmake
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/Makefile
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/compile_commands.json
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/_deps
    COMMENT "Cleaning all build artifacts, binaries, intermediate files, and CMake cache"
    VERBATIM
)

# Note: The standard 'clean' target is provided by CMake automatically.
# Use 'cmake --build . --target clean' for standard clean, or
# 'cmake --build . --target clean-build' for deep clean (removes everything).
