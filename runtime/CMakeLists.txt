cmake_minimum_required(VERSION 3.16)
project(cira VERSION 1.0.0 LANGUAGES C CXX)

# Options
option(CIRA_BUILD_SHARED "Build shared library" ON)
option(CIRA_BUILD_TESTS "Build tests" ON)
option(CIRA_ENABLE_DARKNET "Enable Darknet loader" ON)
option(CIRA_ENABLE_ONNX "Enable ONNX Runtime loader" ON)
option(CIRA_ENABLE_TENSORRT "Enable TensorRT loader" OFF)
option(CIRA_ENABLE_NCNN "Enable NCNN loader" OFF)
option(CIRA_ENABLE_VULKAN "Enable Vulkan for NCNN" OFF)
option(CIRA_ENABLE_STREAMING "Enable HTTP streaming server" ON)
option(CIRA_ENABLE_OPENCV "Enable OpenCV camera capture" ON)

# Manual paths for libraries (Windows SDK downloads)
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to ONNX Runtime installation (e.g., C:/onnxruntime-win-x64-1.17.0)")

# C/C++ standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

# Find dependencies
if(CIRA_ENABLE_ONNX)
    if(ONNXRUNTIME_ROOT)
        # Manual path specified (Windows SDK download)
        message(STATUS "Looking for ONNX Runtime at: ${ONNXRUNTIME_ROOT}")
        find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_c_api.h
            PATHS ${ONNXRUNTIME_ROOT}/include
            NO_DEFAULT_PATH)
        find_library(ONNXRUNTIME_LIB onnxruntime
            PATHS ${ONNXRUNTIME_ROOT}/lib
            NO_DEFAULT_PATH)
        if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
            set(ONNXRUNTIME_MANUAL_FOUND TRUE)
            message(STATUS "ONNX Runtime found (manual path)")
            message(STATUS "  Include: ${ONNXRUNTIME_INCLUDE_DIR}")
            message(STATUS "  Library: ${ONNXRUNTIME_LIB}")
        else()
            message(WARNING "ONNX Runtime not found at ${ONNXRUNTIME_ROOT}")
        endif()
    else()
        # Try CMake package first
        find_package(onnxruntime QUIET)
        if(NOT onnxruntime_FOUND)
            message(STATUS "ONNX Runtime not found via CMake, trying pkg-config")
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(ONNXRUNTIME onnxruntime)
            endif()
        endif()
    endif()
endif()

if(CIRA_ENABLE_TENSORRT)
    find_package(TensorRT QUIET)
endif()

if(CIRA_ENABLE_NCNN)
    find_package(ncnn QUIET)
    if(NOT ncnn_FOUND)
        # Try pkg-config as fallback
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(NCNN_PKG ncnn)
        endif()
        if(NOT NCNN_PKG_FOUND)
            message(FATAL_ERROR
                "NCNN not found. Install NCNN or disable with -DCIRA_ENABLE_NCNN=OFF\n"
                "  Windows: vcpkg install ncnn:x64-windows\n"
                "  Linux:   sudo apt install libncnn-dev  OR  build from source\n"
                "  macOS:   brew install ncnn\n"
                "  Or set ncnn_DIR to your NCNN install path")
        endif()
    endif()

    if(CIRA_ENABLE_VULKAN)
        find_package(Vulkan QUIET)
    endif()
endif()

# OpenCV for cross-platform camera capture
if(CIRA_ENABLE_STREAMING AND CIRA_ENABLE_OPENCV)
    find_package(OpenCV QUIET COMPONENTS core imgproc videoio imgcodecs)
    if(NOT OpenCV_FOUND)
        message(STATUS "OpenCV not found. Camera capture will be disabled.")
        message(STATUS "  Windows: pacman -S mingw-w64-x86_64-opencv (MSYS2)")
        message(STATUS "  Linux:   sudo apt install libopencv-dev")
        message(STATUS "  macOS:   brew install opencv")
    else()
        message(STATUS "OpenCV found: ${OpenCV_VERSION}")
    endif()
endif()

# Source files
set(CIRA_SOURCES
    src/cira.c
    src/yolo_decoder.c
)

if(CIRA_ENABLE_DARKNET)
    list(APPEND CIRA_SOURCES src/darknet_loader.c)
endif()

if(CIRA_ENABLE_ONNX)
    list(APPEND CIRA_SOURCES src/onnx_loader.c)
endif()

if(CIRA_ENABLE_TENSORRT)
    list(APPEND CIRA_SOURCES src/trt_loader.c)
endif()

if(CIRA_ENABLE_NCNN)
    list(APPEND CIRA_SOURCES src/ncnn_loader.cpp)
endif()

if(CIRA_ENABLE_STREAMING)
    list(APPEND CIRA_SOURCES
        src/stream_server.c
        src/camera.cpp
        src/jpeg_encoder.cpp
        src/annotator.c
    )
endif()

# Library
if(CIRA_BUILD_SHARED)
    add_library(cira SHARED ${CIRA_SOURCES})
else()
    add_library(cira STATIC ${CIRA_SOURCES})
endif()

target_include_directories(cira PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link dependencies
if(CIRA_ENABLE_ONNX)
    if(ONNXRUNTIME_MANUAL_FOUND)
        # Manual path (Windows SDK download)
        # Use SYSTEM to suppress warnings from external headers
        target_include_directories(cira SYSTEM PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(cira PRIVATE ${ONNXRUNTIME_LIB})
        target_compile_definitions(cira PRIVATE CIRA_ONNX_ENABLED)
        message(STATUS "ONNX Runtime enabled (manual path)")
    elseif(onnxruntime_FOUND)
        # CMake package
        target_link_libraries(cira PRIVATE onnxruntime::onnxruntime)
        target_compile_definitions(cira PRIVATE CIRA_ONNX_ENABLED)
        message(STATUS "ONNX Runtime enabled (CMake package)")
    elseif(ONNXRUNTIME_FOUND)
        # pkg-config - use SYSTEM to suppress warnings from external headers
        target_include_directories(cira SYSTEM PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
        target_link_libraries(cira PRIVATE ${ONNXRUNTIME_LIBRARIES})
        target_compile_definitions(cira PRIVATE CIRA_ONNX_ENABLED)
        message(STATUS "ONNX Runtime enabled (pkg-config)")
    else()
        message(WARNING "ONNX Runtime requested but not found. ONNX loader will be disabled.")
        message(STATUS "  To enable, specify -DONNXRUNTIME_ROOT=/path/to/onnxruntime")
    endif()
endif()

if(CIRA_ENABLE_TENSORRT AND TensorRT_FOUND)
    target_link_libraries(cira PRIVATE TensorRT::nvinfer)
    target_compile_definitions(cira PRIVATE CIRA_TRT_ENABLED)
endif()

if(CIRA_ENABLE_DARKNET)
    target_compile_definitions(cira PRIVATE CIRA_DARKNET_ENABLED)
endif()

if(CIRA_ENABLE_NCNN)
    if(ncnn_FOUND)
        target_link_libraries(cira PRIVATE ncnn)
    elseif(NCNN_PKG_FOUND)
        target_include_directories(cira PRIVATE ${NCNN_PKG_INCLUDE_DIRS})
        target_link_libraries(cira PRIVATE ${NCNN_PKG_LIBRARIES})
    endif()
    target_compile_definitions(cira PRIVATE CIRA_NCNN_ENABLED)

    if(CIRA_ENABLE_VULKAN AND Vulkan_FOUND)
        target_link_libraries(cira PRIVATE Vulkan::Vulkan)
        target_compile_definitions(cira PRIVATE CIRA_VULKAN_ENABLED)
    endif()
endif()

if(CIRA_ENABLE_STREAMING)
    target_compile_definitions(cira PRIVATE CIRA_STREAMING_ENABLED)
    # Add streaming dependencies (microhttpd for HTTP server)
    find_library(MICROHTTPD_LIB microhttpd)
    if(MICROHTTPD_LIB)
        target_link_libraries(cira PRIVATE ${MICROHTTPD_LIB})
    endif()
    # OpenCV for camera capture
    if(OpenCV_FOUND)
        target_include_directories(cira PRIVATE ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(cira PRIVATE ${OpenCV_LIBS})
        target_compile_definitions(cira PRIVATE CIRA_OPENCV_ENABLED)
        message(STATUS "OpenCV camera capture enabled")
    endif()
endif()

# Math library (not needed on Windows)
if(NOT WIN32)
    target_link_libraries(cira PRIVATE m)
endif()

# Thread library
find_package(Threads REQUIRED)
target_link_libraries(cira PRIVATE Threads::Threads)

# Version info
target_compile_definitions(cira PRIVATE
    CIRA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CIRA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CIRA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Install
install(TARGETS cira
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

# Tests
if(CIRA_BUILD_TESTS)
    enable_testing()

    if(CIRA_ENABLE_DARKNET)
        add_executable(test_darknet test/test_darknet.c)
        target_link_libraries(test_darknet PRIVATE cira)
        add_test(NAME test_darknet COMMAND test_darknet)
    endif()

    if(CIRA_ENABLE_ONNX)
        add_executable(test_onnx test/test_onnx.c)
        target_link_libraries(test_onnx PRIVATE cira)
        add_test(NAME test_onnx COMMAND test_onnx)
    endif()

    if(CIRA_ENABLE_STREAMING)
        add_executable(test_stream test/test_stream.c)
        target_link_libraries(test_stream PRIVATE cira)
        add_test(NAME test_stream COMMAND test_stream)
    endif()

    if(CIRA_ENABLE_NCNN)
        add_executable(test_ncnn test/test_ncnn.c)
        target_link_libraries(test_ncnn PRIVATE cira)
        add_test(NAME test_ncnn COMMAND test_ncnn)
    endif()
endif()

# pkg-config file
configure_file(cira.pc.in cira.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cira.pc
    DESTINATION lib/pkgconfig
)
