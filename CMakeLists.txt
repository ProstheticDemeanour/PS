include (FetchContent)

cmake_minimum_required(VERSION 3.16)

project(PS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CNAKE_CXX_EXTENSIONS OFF)

add_executable(PS 
	src/core/ps.cpp

)

target_include_directories(PS
PRIVATE
	src
)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    FIND_PACKAGE_ARGS 3.11)          # uses a system install first (CMake >= 3.24)
FetchContent_MakeAvailable(nlohmann_json)

target_link_libraries(PS PRIVATE nlohmann_json::nlohmann_json)