cmake_minimum_required(VERSION 3.20)
project(axiotick LANGUAGES C CXX)
enable_testing()

# --- Global Compiler Optimization Settings (HFT Profile) ---

# C++ Standard: 20 (Required for concepts, ranges, coroutines)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C Standard: 17
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output Directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Compiler Flags
if(MSVC)
    add_compile_options(/W4 /permissive- /O2 /Oi /Ot /Oy /GL)
    add_link_options(/LTCG)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    # GCC/Clang optimization flags
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror
        -O3 -march=native -mtune=native 
        -funroll-loops -fno-exceptions -fno-rtti # HFT: No exceptions/RTTI for speed (optional, strict)
    )
endif()

# Dependencies (FetchContent or FindPackage can be added here)
# For now, we assume libraries are installed or will be added as submodules.

# Subdirectories
option(ARGENTUM_USE_FLATBUFFERS "Enable FlatBuffers serialization" OFF)

add_subdirectory(backend)
