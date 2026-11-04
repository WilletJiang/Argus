if(BUILD_TESTING)
  foreach(test core stream stream_items tools alloc parallel context results recovery cache)
    add_executable(test_${test} test_${test}.c)
    ah_compile_target(test_${test})
    target_compile_options(test_${test} PRIVATE -UNDEBUG)
    target_include_directories(test_${test} PRIVATE
      ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/providers)
    target_link_libraries(test_${test} PRIVATE ah ah_json Threads::Threads SQLite::SQLite3)
    add_test(NAME ${test} COMMAND test_${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 45)
  endforeach()
  target_sources(test_core PRIVATE core_fixture.c)
  target_sources(test_tools PRIVATE file_cases.c)
  target_sources(test_parallel PRIVATE parallel_recovery_cases.c)

  add_executable(http_fixture http_fixture.c)
  target_link_libraries(http_fixture PRIVATE ah)
  add_test(NAME http COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_http.py $<TARGET_FILE:http_fixture>)
  add_test(NAME cli COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py $<TARGET_FILE:ah_cli>)
  add_test(NAME rpc COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_rpc.py $<TARGET_FILE:ah_cli>)
  set_tests_properties(http cli rpc PROPERTIES TIMEOUT 60)
  add_test(NAME embed COMMAND ah_embed)
  add_test(NAME host_auth COMMAND ${Python3_EXECUTABLE} -B -m unittest
    host.tests.test_session host.tests.test_login)
  add_test(NAME python_entrypoints COMMAND ${Python3_EXECUTABLE} -B -m unittest tests.test_entrypoints)
  set_tests_properties(host_auth python_entrypoints PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} TIMEOUT 30)
endif()

if(AH_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AH_FUZZ requires Clang libFuzzer")
  endif()
  add_executable(fuzz_stream fuzz_stream.c
    ${PROJECT_SOURCE_DIR}/src/providers/stream.c ${PROJECT_SOURCE_DIR}/src/providers/stream_items.c
    ${PROJECT_SOURCE_DIR}/src/common.c ${PROJECT_SOURCE_DIR}/vendor/yyjson/yyjson.c)
  target_include_directories(fuzz_stream PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/providers ${PROJECT_SOURCE_DIR}/vendor/yyjson)
  target_compile_definitions(fuzz_stream PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE=1)
  target_compile_options(fuzz_stream PRIVATE -fsanitize=fuzzer,undefined)
  target_link_options(fuzz_stream PRIVATE -fsanitize=fuzzer,undefined)
endif()
