cmake_minimum_required(VERSION 3.20)
project(web_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Debug 模式 + 生成 compile_commands.json（供 clangd 使用）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)

set(SOURCES
    src/main.cpp
    src/core/connection.cpp
    src/core/event_loop.cpp
    src/http/http_request.cpp
    src/http/http_response.cpp
    src/server/file_server.cpp
)

add_executable(web_server ${SOURCES})
target_include_directories(web_server PRIVATE src)
target_link_libraries(web_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(web_server PRIVATE -O3 -flto -DNDEBUG)
    target_link_options(web_server PRIVATE -flto)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(web_server PRIVATE -O0 -g)
else()
    # 默认（未指定构建类型）视为 Debug
    target_compile_options(web_server PRIVATE -O0 -g)
endif()

target_compile_options(web_server PRIVATE -Wall -Wextra -Wpedantic)

# 测试
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
    add_test(
        NAME smoke_test
        COMMAND ${CMAKE_SOURCE_DIR}/tests/smoke_test.sh
                $<TARGET_FILE:web_server> 19999
    )
endif()
