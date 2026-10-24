cmake_minimum_required(VERSION 3.28)
project(JamNNTPd VERSION 2.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)
option(JAM_ASAN_UBSAN "Enable address and undefined behavior sanitizers" OFF)
option(JAM_TSAN "Enable thread sanitizer" OFF)
option(JAM_COVERAGE "Enable Clang source coverage" OFF)
if((JAM_ASAN_UBSAN AND JAM_TSAN) OR (JAM_TSAN AND JAM_COVERAGE))
  message(FATAL_ERROR "ASan, TSan and coverage require compatible separate profiles")
endif()
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <expected>\nint main() { std::expected<int, int> x{1}; return *x - 1; }" JAM_HAS_EXPECTED)
if(NOT JAM_HAS_EXPECTED)
  message(FATAL_ERROR "C++23 toolchain with std::expected is required")
endif()
add_library(jam_options INTERFACE)
if(MSVC)
  target_compile_options(jam_options INTERFACE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(jam_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion
    -Wsign-conversion -Wshadow -Wformat=2 -Werror)
  if(JAM_ASAN_UBSAN)
    target_compile_options(jam_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(jam_options INTERFACE -fsanitize=address,undefined)
  elseif(JAM_TSAN)
    target_compile_options(jam_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(jam_options INTERFACE -fsanitize=thread)
  endif()
  if(JAM_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "Coverage preset requires Clang")
    endif()
    target_compile_options(jam_options INTERFACE -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic)
    target_link_options(jam_options INTERFACE -fprofile-instr-generate)
  endif()
endif()
find_package(spdlog 1.17 CONFIG REQUIRED)
find_package(ICU 78 REQUIRED COMPONENTS uc)
find_package(Threads REQUIRED)
set(JAM_CORE_SOURCES
  src/core/server.cpp
  src/core/post_builder.cpp
  src/core/backend.cpp
  src/core/ftn.cpp
  src/core/text.cpp
  src/core/jam_format.cpp
  src/core/session.cpp
  src/core/signals.cpp
  src/core/workers.cpp
  src/core/message_values.cpp
  src/core/formatting.cpp
  src/core/charset_rules.cpp
  src/core/codec.cpp
  src/core/config.cpp
  src/core/storage.cpp
  src/core/network.cpp
  src/core/map_generator.cpp
  src/core/mime.cpp
  src/core/logging.cpp)
add_library(jam_core ${JAM_CORE_SOURCES})
target_include_directories(jam_core PUBLIC src)
target_link_libraries(jam_core PUBLIC jam_options spdlog::spdlog ICU::uc Threads::Threads)
if(WIN32)
  target_compile_definitions(jam_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(jam_core PUBLIC ws2_32)
endif()
add_executable(jamnntpd src/jamnntpd.cpp)
target_link_libraries(jamnntpd PRIVATE jam_core)
add_executable(makechs src/makechs.cpp)
target_link_libraries(makechs PRIVATE jam_core)
if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_executable(jam_unit tests/unit/ftn.cpp tests/unit/backend.cpp tests/unit/session.cpp tests/unit/workers.cpp tests/unit/message_values.cpp tests/unit/formatting.cpp tests/unit/charset_rules.cpp tests/unit/text.cpp tests/unit/jam_format.cpp tests/unit/codec.cpp tests/unit/config.cpp tests/unit/storage.cpp tests/unit/network.cpp tests/unit/map_generator.cpp tests/unit/mime.cpp tests/unit/logging.cpp tests/unit/corpus.cpp)
  target_link_libraries(jam_unit PRIVATE jam_core Catch2::Catch2WithMain)
  target_compile_definitions(jam_unit PRIVATE JAM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_executable(jam_lifecycle_probe tests/integration/lifecycle_probe.cpp)
  target_link_libraries(jam_lifecycle_probe PRIVATE jam_core)
  add_test(NAME server-stress COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server_stress.py --executable $<TARGET_FILE:jamnntpd>
    --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures --rounds 100)
  set_tests_properties(server-stress PROPERTIES TIMEOUT 240)
  add_test(NAME server-e2e COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py --executable $<TARGET_FILE:jamnntpd>
    --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
  set_tests_properties(server-e2e PROPERTIES TIMEOUT 90)
  add_test(NAME lifecycle-native COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_lifecycle.py --probe $<TARGET_FILE:jam_lifecycle_probe>)
  set_tests_properties(lifecycle-native PROPERTIES TIMEOUT 20)
  add_executable(jam_storage_probe tests/integration/storage_probe.cpp)
  target_link_libraries(jam_storage_probe PRIVATE jam_core)
  add_test(NAME storage-process-lock COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_storage.py
    --probe $<TARGET_FILE:jam_storage_probe>
    --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/jam)
  set_tests_properties(storage-process-lock PROPERTIES TIMEOUT 15)
  add_test(NAME makechs-cli COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_makechs.py --executable $<TARGET_FILE:makechs>)
  set_tests_properties(makechs-cli PROPERTIES TIMEOUT 15)
  add_test(NAME unit COMMAND jam_unit "~[native-tz]")
  add_test(NAME timezone-native COMMAND jam_unit "[native-tz]")
  set_tests_properties(timezone-native PROPERTIES ENVIRONMENT "TZ=EST5EDT" TIMEOUT 15)
  set_tests_properties(unit PROPERTIES TIMEOUT 60)
  add_test(NAME baseline COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests/baseline -v)
  set_tests_properties(baseline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

find_program(JAM_CLANG_TIDY NAMES clang-tidy)
if(JAM_CLANG_TIDY)
  add_custom_target(clang-tidy
    COMMAND ${JAM_CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${JAM_CORE_SOURCES} src/makechs.cpp src/jamnntpd.cpp
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM)
endif()
