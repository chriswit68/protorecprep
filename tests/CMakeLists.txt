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
endif()

# Work around protobuf/absl crashing under ASan/TSan (upstream issue with the
# Homebrew shared libraries, reproducible without any prp code). Sanitizer runs
# exercise everything that does not touch libprotobuf's DescriptorPool.
if(PRP_ASAN OR PRP_TSAN)
  set_tests_properties(unit PROPERTIES ENVIRONMENT "PRP_SKIP_PROTO_TESTS=1")
  if(Python3_Interpreter_FOUND)
    set_tests_properties(integration PROPERTIES
      ENVIRONMENT "PRP_SKIP_PROTO_TESTS=1;ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()
