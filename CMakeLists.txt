cmake_minimum_required(VERSION 3.12)
project(us VERSION 0.0.1)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set compiler flags based on compiler type
if(MSVC)
    # /O2: Optimize for speed
    # /W4: Warning level 4
    # /bigobj: Increase section limit in object files (required for Boost)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /W4 /bigobj")
else()
    # GCC/Clang
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -pipe -Wall")
endif()

# Set library name
set(LIB_NAME "us_http")
set(FRAMEWORK_NAME "us")

# ===== Boost Library Configuration (Header-Only) =====

# Set Boost root directory
if(DEFINED ENV{BOOST_ROOT})
    set(BOOST_ROOT $ENV{BOOST_ROOT})
    message(STATUS "Using BOOST_ROOT from environment: ${BOOST_ROOT}")
elseif(NOT DEFINED BOOST_ROOT)
    set(BOOST_ROOT "C:/local/boost_1_91_0" CACHE PATH "Boost installation root")
endif()

message(STATUS "BOOST_ROOT: ${BOOST_ROOT}")

# Set Boost include directory
set(Boost_INCLUDE_DIR "${BOOST_ROOT}/include" CACHE PATH "Boost include directory")

message(STATUS "Boost include directory: ${Boost_INCLUDE_DIR}")

# Check if include directory exists
if(NOT EXISTS ${Boost_INCLUDE_DIR})
    message(FATAL_ERROR "Boost include directory not found: ${Boost_INCLUDE_DIR}")
endif()

# Add Boost include directory (header-only libraries)
include_directories(${Boost_INCLUDE_DIR})

# Include directory (project headers)
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/inc")
include_directories(${INCLUDE_DIR})

# Source files
set(SOURCES
    src/HttpServer.cpp
    src/WebSocketServer.cpp
    src/WebSocketClient.cpp
)

# Header files list
set(HEADERS
    inc/${FRAMEWORK_NAME}/HttpServer.hpp
    inc/${FRAMEWORK_NAME}/HttpClient.hpp
    inc/${FRAMEWORK_NAME}/WebSocketServer.hpp
    inc/${FRAMEWORK_NAME}/WebSocketClient.hpp
)

# Create shared library
add_library(${LIB_NAME} SHARED ${SOURCES})

# Additional Windows system libraries (for socket/network functions)
if(WIN32)
    target_link_libraries(${LIB_NAME} 
        ws2_32    # Windows Socket API
        # crypt32  # If you need encryption
        # bcrypt   # Windows Cryptography API
    )
else()
    # Linux/Mac - pthread for threading
    target_link_libraries(${LIB_NAME} 
        pthread
    )
endif()

# Set position independent code for shared library
if(UNIX AND NOT APPLE)
    set_target_properties(${LIB_NAME} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# Set output directory
set_target_properties(${LIB_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build"
)

# Set suffix for Windows
if(WIN32)
    set_target_properties(${LIB_NAME} PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
    )
else()
    set_target_properties(${LIB_NAME} PROPERTIES
        PREFIX "lib"
        SUFFIX ".so"
    )
endif()

# Install targets
install(TARGETS ${LIB_NAME}
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

# Install headers
install(FILES ${HEADERS}
    DESTINATION include/${FRAMEWORK_NAME}
)

# Clean target
add_custom_target(clear
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/build"
    COMMENT "Cleaning build directory..."
)

# Print configuration info
message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Library: ${LIB_NAME}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
if(MSVC)
    message(STATUS "MSVC version: ${MSVC_VERSION}")
endif()
message(STATUS "BOOST_ROOT: ${BOOST_ROOT}")
message(STATUS "Boost include: ${Boost_INCLUDE_DIR}")
message(STATUS "Boost libraries: Header-only (no linking required)")
message(STATUS "========================================")