cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
project(protorecprep VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
endif()

option(PRP_ASAN "Build with AddressSanitizer" OFF)
option(PRP_TSAN "Build with ThreadSanitizer" OFF)
option(PRP_BUILD_TESTS "Build tests" ON)

find_package(Threads REQUIRED)
find_package(Boost 1.80 REQUIRED)
find_package(protobuf CONFIG REQUIRED)

add_library(prp_core
  src/prp/util/log.cpp
  src/prp/util/crc32c.cpp
  src/prp/util/sha256.cpp
  src/prp/util/base64.cpp
  src/prp/util/fs.cpp
  src/prp/util/text.cpp
  src/prp/core/item_queue.cpp
  src/prp/record/format.cpp
  src/prp/record/recorder.cpp
  src/prp/record/reader.cpp
  src/prp/proto/schema.cpp
  src/prp/json/json_writer.cpp
  src/prp/net/framer.cpp
  src/prp/net/server.cpp
  src/prp/net/client.cpp
  src/prp/net/forwarder.cpp
  src/prp/cli/options.cpp
  src/prp/app/record_app.cpp
  src/prp/replay/replay_app.cpp
)
target_include_directories(prp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(prp_core PUBLIC
  Boost::headers
  protobuf::libprotobuf
  protobuf::libprotoc
  Threads::Threads
)
target_compile_options(prp_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

if(PRP_ASAN)
  target_compile_options(prp_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(prp_core PUBLIC -fsanitize=address)
endif()
if(PRP_TSAN)
  target_compile_options(prp_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(prp_core PUBLIC -fsanitize=thread)
endif()

add_executable(protorecprep src/main.cpp)
target_link_libraries(protorecprep PRIVATE prp_core)
target_compile_definitions(prp_core PUBLIC PRP_VERSION="${PROJECT_VERSION}")

if(PRP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
