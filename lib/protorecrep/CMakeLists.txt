cmake_minimum_required(VERSION 3.20)

project(protorecrep VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROTORECREP_BUILD_INSTALL "Generate install and CMake export rules" ON)
option(PROTORECREP_ASAN "Build with AddressSanitizer" OFF)
option(PROTORECREP_TSAN "Build with ThreadSanitizer" OFF)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

add_library(protorecrep STATIC
  src/capture_item.cpp
  src/format.cpp
  src/reader.cpp
  src/recorder.cpp
  src/util/crc32c.cpp
  src/util/fs.cpp
  src/util/sha256.cpp
  src/util/text.cpp
)
add_library(protorecrep::protorecrep ALIAS protorecrep)

target_compile_features(protorecrep PUBLIC cxx_std_20)
target_include_directories(protorecrep
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(protorecrep PUBLIC Threads::Threads)
target_compile_options(protorecrep PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

if(PROTORECREP_ASAN)
  target_compile_options(protorecrep PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(protorecrep PUBLIC -fsanitize=address)
endif()
if(PROTORECREP_TSAN)
  target_compile_options(protorecrep PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(protorecrep PUBLIC -fsanitize=thread)
endif()

if(PROTORECREP_BUILD_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS protorecrep
    EXPORT protorecrepTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT protorecrepTargets
    FILE protorecrepTargets.cmake
    NAMESPACE protorecrep::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/protorecrep
  )

  configure_package_config_file(
    cmake/protorecrepConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/protorecrepConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/protorecrep
  )
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/protorecrepConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/protorecrepConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/protorecrepConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/protorecrep
  )
endif()
