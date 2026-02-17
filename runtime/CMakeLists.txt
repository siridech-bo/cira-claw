cmake_minimum_required(VERSION 3.16)
project(cira VERSION 1.0.0 LANGUAGES C)

# Options
option(CIRA_BUILD_SHARED "Build shared library" ON)
option(CIRA_BUILD_TESTS "Build tests" ON)
option(CIRA_ENABLE_DARKNET "Enable Darknet loader" ON)
option(CIRA_ENABLE_ONNX "Enable ONNX Runtime loader" ON)
option(CIRA_ENABLE_TENSORRT "Enable TensorRT loader" OFF)
option(CIRA_ENABLE_STREAMING "Enable HTTP streaming server" ON)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

# Find dependencies
if(CIRA_ENABLE_ONNX)
    find_package(onnxruntime QUIET)
    if(NOT onnxruntime_FOUND)
        message(STATUS "ONNX Runtime not found, will try pkg-config")
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(ONNXRUNTIME onnxruntime)
        endif()
    endif()
endif()

if(CIRA_ENABLE_TENSORRT)
    find_package(TensorRT QUIET)
endif()

# Source files
set(CIRA_SOURCES
    src/cira.c
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

if(CIRA_ENABLE_STREAMING)
    list(APPEND CIRA_SOURCES
        src/stream_server.c
        src/camera.c
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
if(CIRA_ENABLE_ONNX AND onnxruntime_FOUND)
    target_link_libraries(cira PRIVATE onnxruntime::onnxruntime)
    target_compile_definitions(cira PRIVATE CIRA_ONNX_ENABLED)
elseif(CIRA_ENABLE_ONNX AND ONNXRUNTIME_FOUND)
    target_include_directories(cira PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
    target_link_libraries(cira PRIVATE ${ONNXRUNTIME_LIBRARIES})
    target_compile_definitions(cira PRIVATE CIRA_ONNX_ENABLED)
endif()

if(CIRA_ENABLE_TENSORRT AND TensorRT_FOUND)
    target_link_libraries(cira PRIVATE TensorRT::nvinfer)
    target_compile_definitions(cira PRIVATE CIRA_TRT_ENABLED)
endif()

if(CIRA_ENABLE_DARKNET)
    target_compile_definitions(cira PRIVATE CIRA_DARKNET_ENABLED)
endif()

if(CIRA_ENABLE_STREAMING)
    target_compile_definitions(cira PRIVATE CIRA_STREAMING_ENABLED)
    # Add streaming dependencies (microhttpd for HTTP server)
    find_library(MICROHTTPD_LIB microhttpd)
    if(MICROHTTPD_LIB)
        target_link_libraries(cira PRIVATE ${MICROHTTPD_LIB})
    endif()
endif()

# Math library
target_link_libraries(cira PRIVATE m)

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
endif()

# pkg-config file
configure_file(cira.pc.in cira.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cira.pc
    DESTINATION lib/pkgconfig
)
