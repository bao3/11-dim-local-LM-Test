cmake_minimum_required(VERSION 3.16)

# This image's /usr/bin/c++ can be Clang without a libstdc++ link line.
# The benchmark, and dimension 3, are specified as g++ -std=c++17.
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER g++ CACHE STRING "C++ compiler" FORCE)
endif()

project(ps5ai-11dim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Release CI passes -DPS5AI_STATIC_CXX=ON. Shared libcurl stays the default;
# a fully static curl pulls OpenSSL and friends and is not required for v1.1.
option(PS5AI_STATIC_CXX "Link libstdc++ and libgcc statically" OFF)
option(PS5AI_PREFER_STATIC_CURL "Prefer static libcurl if available (best-effort; v1.1 still uses shared curl)" OFF)

add_executable(ps5ai-11dim
    src/main.cpp
    src/scoring.cpp
    src/long_context.cpp
    src/long_context_data.cpp
    src/prompt_data.cpp
    src/paths.cpp
    src/sandbox.cpp
    src/http.cpp
    src/perf.cpp
    src/report.cpp
    src/selftest.cpp
)

target_include_directories(ps5ai-11dim PRIVATE src)
target_include_directories(ps5ai-11dim SYSTEM PRIVATE third_party)
target_compile_definitions(ps5ai-11dim PRIVATE PS5AI_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
target_compile_options(ps5ai-11dim PRIVATE -Wall -Wextra -Wpedantic -finput-charset=UTF-8 -fexec-charset=UTF-8)
target_link_libraries(ps5ai-11dim PRIVATE CURL::libcurl Threads::Threads)

if(PS5AI_STATIC_CXX)
  target_link_options(ps5ai-11dim PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(PS5AI_PREFER_STATIC_CURL)
  message(STATUS "PS5AI_PREFER_STATIC_CURL=ON: shared CURL::libcurl remains linked. Fully static curl is not required for the v1.1 bundle.")
endif()

enable_testing()
add_test(NAME selftest COMMAND ps5ai-11dim --self-test)
