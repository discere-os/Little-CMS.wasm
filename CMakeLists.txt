cmake_minimum_required(VERSION 3.20)
project(Little-CMS-WASM VERSION 2.17.0 LANGUAGES C)

# WASM Ecosystem Configuration
# Copyright 2025 Superstruct Ltd, New Zealand
# Licensed under LGPL/GPL/MPL tri-license (same as lcms2)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Emscripten detection
if(EMSCRIPTEN)
    set(WASM_BUILD ON)
    message(STATUS "Building Little-CMS for WebAssembly with Emscripten")
else()
    set(WASM_BUILD OFF)
    message(STATUS "Building Little-CMS for native platform")
endif()

# Build configuration options
option(ENABLE_SIMD "Enable WASM SIMD optimizations" ON)
option(ENABLE_WASM_NATIVE "Enable WASM-native filesystem patterns" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUILD_UTILS "Build utility programs" OFF)
option(BUILD_TESTS "Build test suite" ON)

# Tier 2 System Library Configuration (Little-CMS)
set(WASM_TIER 2)
set(WASM_COMPLEXITY 2)  # Simple color management library

# Core library sources
set(LCMS2_SOURCES
    src/cmsalpha.c
    src/cmscam02.c
    src/cmscgats.c
    src/cmscnvrt.c
    src/cmserr.c
    src/cmsgamma.c
    src/cmsgmt.c
    src/cmshalf.c
    src/cmsintrp.c
    src/cmsio0.c
    src/cmsio1.c
    src/cmslut.c
    src/cmsmd5.c
    src/cmsmtrx.c
    src/cmsnamed.c
    src/cmsopt.c
    src/cmspack.c
    src/cmspcs.c
    src/cmsplugin.c
    src/cmsps2.c
    src/cmssamp.c
    src/cmssm.c
    src/cmstypes.c
    src/cmsvirt.c
    src/cmswtpnt.c
    src/cmsxform.c
)

# WASM-specific sources
if(WASM_BUILD)
    list(APPEND LCMS2_SOURCES
        src/wasm/lcms2_wasm_simd.c
        src/wasm/lcms2_wasm_native.c
        src/wasm/lcms2_wasm_module.c
    )
endif()

# Include directories
include_directories(
    include
    src
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

# Compiler definitions
add_definitions(
    -DHAVE_FUNC_ATTRIBUTE_VISIBILITY=1
    -DHasTHREADS=0  # Single-threaded WASM build
)

# WASM-specific configuration
if(WASM_BUILD)
    # Emscripten-specific settings
    set(CMAKE_EXECUTABLE_SUFFIX ".js")
    
    # Base WASM flags (Tier 2 System Library)
    set(WASM_BASE_FLAGS
        -s WASM=1
        -s MODULARIZE=1 
        -s EXPORT_ES6=1
        -s USE_ES6_IMPORT_META=0
        -s ENVIRONMENT=web,webview,worker,node
        -s ALLOW_MEMORY_GROWTH=1
        -s INITIAL_MEMORY=64MB
        -s MAXIMUM_MEMORY=512MB
        -s STACK_SIZE=5MB
        -s ASSERTIONS=0
        -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8']
    )
    
    # Performance optimization flags
    set(WASM_PERF_FLAGS
        -O3
        -flto
        --closure 1
        -DNDEBUG
    )
    
    # SIMD flags (modern Emscripten 4.0+ compatible)
    if(ENABLE_SIMD)
        list(APPEND WASM_PERF_FLAGS
            -msimd128
            -DLCMS2_WASM_SIMD_ENABLED=1
        )
        message(STATUS "WASM SIMD optimizations enabled")
    endif()
    
    # WASM-native filesystem support
    if(ENABLE_WASM_NATIVE)
        list(APPEND WASM_BASE_FLAGS
            -s FORCE_FILESYSTEM=1
            -lidbfs.js
            --preload-file icc-profiles@/icc-profiles
            -DLCMS2_WASM_NATIVE=1
        )
        message(STATUS "WASM-native filesystem patterns enabled")
    endif()
    
    # Function exports for color transforms
    set(WASM_EXPORTS
        -s EXPORTED_FUNCTIONS=['_malloc','_free',
                               '_cmsCreateContext','_cmsDeleteContext',
                               '_cmsOpenProfileFromFile','_cmsOpenProfileFromMem',
                               '_cmsCloseProfile','_cmsCreateTransform',
                               '_cmsDeleteTransform','_cmsDoTransform',
                               '_cmsWasmsIMDTransform','_cmsWasmNativeLoadProfile',
                               '_cmsWasmGetSIMDSupport','_cmsWasmBenchmarkTransform']
    )
    
    # Combine all WASM flags
    string(REPLACE ";" " " WASM_FLAGS "${WASM_BASE_FLAGS} ${WASM_PERF_FLAGS} ${WASM_EXPORTS}")
    
    # Apply to compiler and linker
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${WASM_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${WASM_FLAGS}")
    
    # Disable features not needed in WASM
    add_definitions(
        -DCMS_NO_PTHREADS=1
        -DCMS_DONT_USE_FAST_FLOOR=1  # Ensure compatibility
    )
    
    # Check for threaded build (optional)
    if(ENABLE_THREADING)
        add_definitions(-DHasTHREADS=1)
        list(APPEND WASM_BASE_FLAGS
            -s USE_PTHREADS=1
            -s PTHREAD_POOL_SIZE=4
        )
    endif()
endif()

# Native build configuration
if(NOT WASM_BUILD)
    find_library(M_LIB m)
    find_package(Threads)
    
    if(Threads_FOUND)
        add_definitions(-DHasTHREADS=1)
    endif()
endif()

# Create the main library
add_library(lcms2 ${LCMS2_SOURCES})

# Link dependencies
if(NOT WASM_BUILD)
    if(M_LIB)
        target_link_libraries(lcms2 ${M_LIB})
    endif()
    if(Threads_FOUND)
        target_link_libraries(lcms2 ${CMAKE_THREAD_LIBS_INIT})
    endif()
endif()

# Library properties
set_target_properties(lcms2 PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "include/lcms2.h"
)

if(WASM_BUILD)
    set_target_properties(lcms2 PROPERTIES OUTPUT_NAME "lcms2")
endif()

# WASM module target
if(WASM_BUILD)
    add_executable(lcms2-wasm src/wasm/lcms2_wasm_main.c)
    target_link_libraries(lcms2-wasm lcms2)
    
    # Dual build strategy - create both SIMD and fallback versions
    if(ENABLE_SIMD)
        # SIMD-optimized build
        add_executable(lcms2-wasm-simd src/wasm/lcms2_wasm_main.c)
        target_link_libraries(lcms2-wasm-simd lcms2)
        target_compile_definitions(lcms2-wasm-simd PRIVATE LCMS2_WASM_SIMD_ENABLED=1)
        
        # Fallback build (no SIMD)
        add_executable(lcms2-wasm-fallback src/wasm/lcms2_wasm_main.c)
        target_link_libraries(lcms2-wasm-fallback lcms2)
        target_compile_definitions(lcms2-wasm-fallback PRIVATE LCMS2_WASM_SIMD_ENABLED=0)
    endif()
endif()

# Install configuration
if(NOT WASM_BUILD)
    install(TARGETS lcms2
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        PUBLIC_HEADER DESTINATION include
    )
    
    # Install pkg-config file
    configure_file(lcms2.pc.in lcms2.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lcms2.pc 
        DESTINATION lib/pkgconfig)
endif()

# Build utilities (disabled for WASM)
if(BUILD_UTILS AND NOT WASM_BUILD)
    add_subdirectory(utils)
endif()

# Build tests
if(BUILD_TESTS)
    add_subdirectory(testbed)
endif()