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
option(PRP_STATIC "Linux: fully static executable; macOS: static third-party libraries" OFF)

if(PRP_STATIC AND (PRP_ASAN OR PRP_TSAN))
  message(FATAL_ERROR "PRP_STATIC cannot be combined with PRP_ASAN or PRP_TSAN")
endif()

# Prefer static archives on Linux. On macOS the system libraries only exist as
# .tbd stubs, so the suffix restriction must not be applied there.
if(PRP_STATIC AND NOT APPLE)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()

find_package(Threads REQUIRED)
find_package(Boost 1.80 REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# Fail early when a static build silently picks up shared protobuf libraries:
# a fully static Linux link would fail later with a confusing linker error and
# macOS would produce a binary that still depends on Homebrew dylibs.
if(PRP_STATIC)
  foreach(_prp_target protobuf::libprotobuf protobuf::libprotoc)
    set(_prp_locations)
    get_target_property(_prp_configs ${_prp_target} IMPORTED_CONFIGURATIONS)
    if(_prp_configs)
      foreach(_prp_config ${_prp_configs})
        get_target_property(_prp_location ${_prp_target} IMPORTED_LOCATION_${_prp_config})
        if(_prp_location)
          list(APPEND _prp_locations "${_prp_location}")
        endif()
      endforeach()
    endif()
    get_target_property(_prp_location ${_prp_target} IMPORTED_LOCATION)
    if(_prp_location)
      list(APPEND _prp_locations "${_prp_location}")
    endif()
    if(NOT _prp_locations)
      message(FATAL_ERROR "Could not determine the imported location of ${_prp_target}")
    endif()
    foreach(_prp_location ${_prp_locations})
      if(_prp_location MATCHES "\\.(dylib|so)(\\.|$)")
        message(FATAL_ERROR
          "${_prp_target} is shared (${_prp_location}). Configure against a static "
          "dependency prefix, e.g. -DCMAKE_PREFIX_PATH=/opt/prp-static "
          "(see doc/static-build.md).")
      endif()
    endforeach()
  endforeach()
endif()

if(PRP_STATIC AND NOT APPLE)
  add_link_options(-static)
elseif(PRP_STATIC AND APPLE)
  message(STATUS "PRP_STATIC: third-party libraries are static; libc++/libSystem remain dynamic")
endif()

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
  # Protobuf/Abseil first: their include directories must win over a Homebrew
  # prefix that also contains newer abseil headers (static builds).
  protobuf::libprotobuf
  protobuf::libprotoc
  Boost::headers
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
