cmake_minimum_required(VERSION 3.16)
project(edgeflow CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -O3 -march=native)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)
find_package(CURL REQUIRED)

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(edgeflow
    src/main.cpp
    src/order_book.cpp
    src/signal_engine.cpp
    src/signal_validator.cpp
    src/execution_simulator.cpp
    src/binance_depth_feed.cpp
)

target_include_directories(edgeflow PRIVATE include ${LWS_INCLUDE_DIRS})
target_link_libraries(edgeflow PRIVATE
    ${LWS_LIBRARIES} CURL::libcurl nlohmann_json::nlohmann_json)
