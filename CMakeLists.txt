cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(ZSend 
    VERSION 0.1.0 
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(ENABLE_GRPC "Enable gRPC support" OFF)
option(ENABLE_BREAKPAD "Enable Google Breakpad crash reporting" OFF)
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(BUILD_TESTING "Build tests" ON)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
    if(WIN32)
        # Force GCC/MinGW to use UTF-8 for both source and execution
        add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
endif()

# ASan support
if(ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        set(_zsend_enable_asan TRUE)
        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(_zsend_libasan_found FALSE)
            foreach(_zsend_asan_candidate libasan.a libasan.dll.a)
                execute_process(
                    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_zsend_asan_candidate}
                    OUTPUT_VARIABLE _zsend_libasan_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(NOT _zsend_libasan_path STREQUAL "${_zsend_asan_candidate}" AND EXISTS "${_zsend_libasan_path}")
                    set(_zsend_libasan_found TRUE)
                    break()
                endif()
            endforeach()
            if(NOT _zsend_libasan_found)
                set(_zsend_enable_asan FALSE)
                message(WARNING "ENABLE_ASAN=ON but GNU runtime libasan is unavailable for ${CMAKE_CXX_COMPILER}. Build continues without ASan.")
            endif()
        endif()
        if(_zsend_enable_asan)
            add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
            add_link_options(-fsanitize=address)
        endif()
    endif()
endif()

include(FetchContent)

# 1. spdlog
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip
)
FetchContent_MakeAvailable(spdlog)

# 2. nlohmann_json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
)
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
endif()

# 3. CLI11
FetchContent_Declare(
    CLI11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.1.zip
)
FetchContent_MakeAvailable(CLI11)

# 4. asio
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-28-0.zip
)
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
endif()

if(NOT TARGET asio::asio)
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE) 
    add_library(asio::asio ALIAS asio)
endif()

# 5. doctest
if(BUILD_TESTING)
    FetchContent_Declare(
        doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.zip
    )
    FetchContent_GetProperties(doctest)
    if(NOT doctest_POPULATED)
        FetchContent_Populate(doctest)
    endif()
    
    if(NOT TARGET doctest::doctest)
        add_library(doctest INTERFACE)
        target_include_directories(doctest INTERFACE ${doctest_SOURCE_DIR})
        add_library(doctest::doctest ALIAS doctest)
    endif()
endif()

# 6. gRPC (Optional)
if(ENABLE_GRPC)
    message(STATUS "gRPC enabled - Note: Building gRPC from source takes a long time.")
    FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG v1.54.0
    )
    FetchContent_MakeAvailable(gRPC)
endif()

# 7. Breakpad (Optional)
if(ENABLE_BREAKPAD)
    FetchContent_Declare(
        breakpad
        GIT_REPOSITORY https://github.com/google/breakpad.git
        GIT_TAG main 
    )
    FetchContent_MakeAvailable(breakpad)
endif()

# Main Executable
add_executable(zsend 
    src/main.cpp 
    src/utils/NicknameGenerator.cpp 
    src/config/ConfigManager.cpp
    src/network/Discovery.cpp
    src/network/Transfer.cpp
)

target_include_directories(zsend PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${json_SOURCE_DIR}/include
)

target_link_libraries(zsend PRIVATE 
    spdlog::spdlog 
    CLI11::CLI11
    asio::asio
)

target_compile_definitions(zsend PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
if(ENABLE_ASAN)
    target_compile_definitions(zsend PRIVATE ZSEND_ASAN_ENABLED=1)
endif()

# Link ws2_32 on Windows for asio
if(WIN32)
    target_link_libraries(zsend PRIVATE ws2_32 mswsock)
endif()

if(ENABLE_GRPC)
    target_link_libraries(zsend PRIVATE gRPC::grpc++)
endif()

if(ENABLE_BREAKPAD)
    target_link_libraries(zsend PRIVATE breakpad_client) 
endif()

# Tests
if(BUILD_TESTING)
    enable_testing()
    add_executable(zsend_tests 
        tests/test_main.cpp 
        src/utils/NicknameGenerator.cpp 
        src/config/ConfigManager.cpp
        src/network/Discovery.cpp
        src/network/Transfer.cpp
    )
    target_include_directories(zsend_tests PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${json_SOURCE_DIR}/include
    )
    target_link_libraries(zsend_tests PRIVATE 
        doctest::doctest 
        spdlog::spdlog
        asio::asio
    )
    target_compile_definitions(zsend_tests PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
    if(ENABLE_ASAN)
        target_compile_definitions(zsend_tests PRIVATE ZSEND_ASAN_ENABLED=1)
    endif()
    if(WIN32)
        target_link_libraries(zsend_tests PRIVATE ws2_32 mswsock)
    endif()
    add_test(NAME unit_tests COMMAND zsend_tests)
endif()
