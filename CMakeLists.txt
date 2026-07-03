cmake_minimum_required(VERSION 3.20)
project(web_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(web_server ${SOURCES})
target_include_directories(web_server PRIVATE src)
target_link_libraries(web_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(web_server PRIVATE -O3 -flto -DNDEBUG)
    target_link_options(web_server PRIVATE -flto)
endif()

target_compile_options(web_server PRIVATE -Wall -Wextra -Wpedantic)
