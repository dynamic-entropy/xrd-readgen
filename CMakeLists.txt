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

find_package(XRootD REQUIRED COMPONENTS CLIENT)

include(FetchContent)
FetchContent_Declare(cli11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.5.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(cli11)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

set(READGEN_SOURCES
  src/file_session.cc
  src/read_command.cc
  src/run_command.cc
  src/run_config.cc
  src/units.cc
  src/token_bucket.cc
  src/inflight.cc
  src/error_classifier.cc
  src/scheduler.cc
  src/metrics.cc
  src/file_sink.cc)

add_executable(xrd-readgen
  src/main.cc
  ${READGEN_SOURCES})

target_include_directories(xrd-readgen PRIVATE include)

target_compile_definitions(xrd-readgen PRIVATE
  READGEN_VERSION="${PROJECT_VERSION}")

target_compile_options(xrd-readgen PRIVATE -Wall -Wextra)

target_link_libraries(xrd-readgen PRIVATE
  XRootD::XrdCl
  CLI11::CLI11
  nlohmann_json::nlohmann_json)

install(TARGETS xrd-readgen RUNTIME DESTINATION bin)

enable_testing()

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(xrd-readgen-tests
  tests/test_units.cc
  tests/test_token_bucket.cc
  tests/test_error_classifier.cc
  tests/test_auto_max_bytes.cc
  tests/test_metrics.cc
  tests/test_file_sink.cc
  src/units.cc
  src/token_bucket.cc
  src/error_classifier.cc
  src/run_config.cc
  src/metrics.cc
  src/file_sink.cc)

target_include_directories(xrd-readgen-tests PRIVATE include)
target_compile_options(xrd-readgen-tests PRIVATE -Wall -Wextra)
target_link_libraries(xrd-readgen-tests PRIVATE
  GTest::gtest_main
  nlohmann_json::nlohmann_json)

include(GoogleTest)
gtest_discover_tests(xrd-readgen-tests)
