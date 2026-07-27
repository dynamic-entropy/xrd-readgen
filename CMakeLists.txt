cmake_minimum_required(VERSION 3.24)

project(xrd-readgen
  VERSION 0.1.0
  DESCRIPTION "XRootD remote-read traffic generator for CMS / WLCG data challenges"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# XrdCl from the installed XRootD (or a local build tree via -DXRootD_DIR=...)
find_package(XRootD REQUIRED COMPONENTS CLIENT)

include(FetchContent)
FetchContent_Declare(cli11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.5.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(cli11)

add_executable(xrd-readgen
  src/main.cc
  src/read_command.cc)

target_include_directories(xrd-readgen PRIVATE include)

target_compile_definitions(xrd-readgen PRIVATE
  READGEN_VERSION="${PROJECT_VERSION}")

target_compile_options(xrd-readgen PRIVATE -Wall -Wextra)

target_link_libraries(xrd-readgen PRIVATE
  XRootD::XrdCl
  CLI11::CLI11)

install(TARGETS xrd-readgen RUNTIME DESTINATION bin)

# Tests arrive with the Phase 1 engine (scheduler / rate limiter / error classifier).
enable_testing()
