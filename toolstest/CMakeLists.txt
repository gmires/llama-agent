cmake_minimum_required(VERSION 3.14)
project(test-tools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_tools test_tools.cpp ../src/tools.cpp ../src/permissions.cpp)

# For httplib in fetch tool via curl
target_compile_definitions(test_tools PRIVATE CURL_ENABLED)

target_include_directories(test_tools PRIVATE ../src)
