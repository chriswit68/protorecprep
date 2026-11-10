add_executable(prp_tests unit_tests.cpp)
target_link_libraries(prp_tests PRIVATE prp_core)
target_compile_options(prp_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME unit COMMAND prp_tests ${CMAKE_CURRENT_SOURCE_DIR}/../proto/sample.proto)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME integration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.py
            $<TARGET_FILE:protorecprep> ${CMAKE_CURRENT_SOURCE_DIR}/../proto/sample.proto)
  set_tests_properties(integration PROPERTIES TIMEOUT 90)

  add_test(NAME replay
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/replay_test.py
            $<TARGET_FILE:protorecprep> ${CMAKE_CURRENT_SOURCE_DIR}/../proto/sample.proto)
  set_tests_properties(replay PROPERTIES TIMEOUT 120)

  add_test(NAME passthrough
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/passthrough_test.py
            $<TARGET_FILE:protorecprep> ${CMAKE_CURRENT_SOURCE_DIR}/../proto/sample.proto)
  set_tests_properties(passthrough PROPERTIES TIMEOUT 180)

  add_test(NAME typed
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/typed_test.py
            $<TARGET_FILE:protorecprep> ${CMAKE_CURRENT_SOURCE_DIR}/../proto/sample.proto)
  set_tests_properties(typed PROPERTIES TIMEOUT 90)
endif()

# Work around protobuf/absl crashing under ASan/TSan (upstream issue with the
# Homebrew shared libraries, reproducible without any prp code). Sanitizer runs
# exercise everything that does not touch libprotobuf's DescriptorPool.
if(PRP_ASAN OR PRP_TSAN)
  set_tests_properties(unit PROPERTIES ENVIRONMENT "PRP_SKIP_PROTO_TESTS=1")
  if(Python3_Interpreter_FOUND)
    set_tests_properties(integration PROPERTIES
      ENVIRONMENT "PRP_SKIP_PROTO_TESTS=1;ASAN_OPTIONS=detect_leaks=0")
    set_tests_properties(replay PROPERTIES
      ENVIRONMENT "PRP_SKIP_PROTO_TESTS=1;ASAN_OPTIONS=detect_leaks=0")
    set_tests_properties(passthrough PROPERTIES
      ENVIRONMENT "PRP_SKIP_PROTO_TESTS=1;ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()
