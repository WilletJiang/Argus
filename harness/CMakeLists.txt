cmake_minimum_required(VERSION 3.20)
project(ah VERSION 0.3.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(AH_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(AH_FUZZ "Build libFuzzer targets (Clang)" OFF)
option(AH_BENCHMARK "Build local orchestration and file benchmarks" OFF)
option(AH_LIVE_EVAL "Build explicitly authorized live evaluation drivers" OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
if(AH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

function(ah_compile_target target)
  target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE=1 _DARWIN_C_SOURCE=1)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

# Vendor code has its own warning policy; it is not compiled with our -Werror.
add_library(ah_json STATIC vendor/yyjson/yyjson.c)
target_include_directories(ah_json PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson>)
set_target_properties(ah_json PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ah STATIC
  src/common.c src/hash.c
  src/core/context.c src/core/engine.c src/core/execute.c src/core/mailbox.c src/core/observations.c
  src/core/operations.c src/core/parallel.c src/core/recovery.c src/core/response.c
  src/core/results.c src/core/runtime.c src/core/session.c src/core/store.c src/core/telemetry.c
  src/core/task_budget.c src/core/task_store.c src/core/task_verify.c src/core/task.c
  src/providers/responses.c src/providers/responses_http.c src/providers/responses_metrics.c
  src/providers/stream.c src/providers/stream_items.c
  src/tools/source.c src/tools/walk.c src/tools/search.c src/tools/changes.c src/tools/check.c src/tools/check_registry.c src/tools/diagnostics.c
  src/tools/exec.c src/tools/file.c src/tools/file_read.c src/tools/local.c src/tools/schema.c)
add_library(ah::ah ALIAS ah)
ah_compile_target(ah)
target_include_directories(ah PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(ah PRIVATE src)
target_link_libraries(ah PRIVATE ah_json CURL::libcurl SQLite::SQLite3 Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(ah PRIVATE m)
endif()

add_executable(ah_cli
  src/cli/main.c src/cli/output.c src/cli/rpc.c src/cli/credential.c src/cli/signals.c)
set_target_properties(ah_cli PROPERTIES OUTPUT_NAME ah)
ah_compile_target(ah_cli)
target_include_directories(ah_cli PRIVATE src)
target_link_libraries(ah_cli PRIVATE ah ah_json Threads::Threads)
add_executable(ah_embed examples/embed.c)
target_link_libraries(ah_embed PRIVATE ah)

if(AH_LIVE_EVAL OR BUILD_TESTING)
  add_subdirectory(evals)
endif()
if(BUILD_TESTING OR AH_FUZZ)
  add_subdirectory(tests)
endif()
if(AH_BENCHMARK)
  add_subdirectory(benchmarks)
endif()

install(TARGETS ah ah_json ah_cli EXPORT ahTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ahTargets NAMESPACE ah:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ah)
configure_package_config_file(cmake/ahConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/ahConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ah)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ahConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ahConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/ahConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ah)
