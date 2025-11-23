cmake_minimum_required(VERSION 3.20)

project(hft_mm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(nlohmann_json)

# Fetch libwebsockets for WebSocket support
FetchContent_Declare(
    libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
    GIT_TAG v4.3-stable
)

set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_PING ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
set(LWS_IPV6 ON CACHE BOOL "" FORCE)
set(LWS_UNIX_SOCK ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libwebsockets)

# Fetch Protobuf
FetchContent_Declare(
    protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
    GIT_TAG v25.1
)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(protobuf)

# Generate Protobuf C++ files from all .proto files
file(GLOB PROTO_ALL_FILES "${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto")
set(PROTO_SRCS)
set(PROTO_HDRS)

# Set proto include path to find imported proto files
set(PROTO_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto")

foreach(PROTO_FILE ${PROTO_ALL_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    get_filename_component(PROTO_ABS_PATH ${PROTO_FILE} ABSOLUTE)
    
    list(APPEND PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc")
    list(APPEND PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h")
    
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc"
               "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h"
        COMMAND protobuf::protoc
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --proto_path=${PROTO_INCLUDE_PATH}
             ${PROTO_ABS_PATH}
        DEPENDS ${PROTO_ABS_PATH}
        COMMENT "Running C++ protocol buffer compiler on ${PROTO_NAME}.proto"
        VERBATIM
    )
endforeach()

add_library(mexc
    src/mexc/http_client.cpp
    src/mexc/util.cpp
    src/mexc/client_base.cpp
    src/mexc/spot_client.cpp
    src/mexc/ws_client.cpp
    src/mexc/ws_spot_client.cpp
    src/strategy/trade_ledger.cpp
    src/strategy/market_maker.cpp
    src/strategy/orderbook.cpp
    src/strategy/orderbook_manager.cpp
    src/strategy/orderbook_display.cpp
    ${PROTO_SRCS}
)

target_include_directories(mexc
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(mexc
    PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
        CURL::libcurl
        nlohmann_json::nlohmann_json
        websockets
        protobuf::libprotobuf
)

# Add libwebsockets include directories
target_include_directories(mexc
    PUBLIC
        $<TARGET_PROPERTY:websockets,INTERFACE_INCLUDE_DIRECTORIES>
)

add_executable(hft_mm
    src/main.cpp
)

target_link_libraries(hft_mm
    PRIVATE
        mexc
)

add_executable(orderbook_viewer
    src/orderbook_viewer.cpp
)

target_link_libraries(orderbook_viewer
    PRIVATE
        mexc
)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.4.0
)

FetchContent_MakeAvailable(Catch2)

enable_testing()

add_executable(mexc_tests
    tests/test_util.cpp
    tests/test_integration.cpp
)

target_link_libraries(mexc_tests
    PRIVATE
        mexc
        Catch2::Catch2WithMain
)

add_test(NAME mexc_tests COMMAND mexc_tests)

