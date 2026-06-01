cmake_minimum_required(VERSION 3.14)
project(test-tools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_tools test_tools.cpp ../src/tools.cpp ../src/permissions.cpp ../src/patches.cpp)

# For httplib in fetch tool via curl
target_compile_definitions(test_tools PRIVATE CURL_ENABLED)

target_include_directories(test_tools PRIVATE ../src)

# Token roundtrip test (needs llama.cpp)
add_executable(test_token_roundtrip test_token_roundtrip.cpp)
target_include_directories(test_token_roundtrip PRIVATE
    ../src
    ../build/_deps/llama-src/include
    ../build/_deps/llama-src/common
    ../build/_deps/llama-src/ggml/include
)
target_link_directories(test_token_roundtrip PRIVATE ../build/bin)
target_link_libraries(test_token_roundtrip
    llama
    llama-common
    ggml
    ggml-cpu
    ggml-base
    pthread
    dl
    m
)
