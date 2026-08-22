cmake_minimum_required(VERSION 3.20)
project(lumen VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # computed goto needs the GNU labels-as-values extension

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LUMEN_WERROR "Treat warnings as errors" ON)
# Computed-goto dispatch is a GNU extension, so it is a build option rather than
# something the code assumes. Both paths are built and tested in CI.
option(LUMEN_COMPUTED_GOTO "Use computed-goto dispatch in the VM (GNU extension)" ON)
option(LUMEN_SANITIZE "Build with address and UB sanitizers" OFF)

add_library(lumen_core
  src/lexer.cpp
  src/parser.cpp
  src/optimizer.cpp
  src/compiler.cpp
  src/chunk.cpp
  src/value.cpp
  src/gc.cpp
  src/vm.cpp
  src/lumen.cpp
)
target_include_directories(lumen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(lumen_core PRIVATE /W4)
  if(LUMEN_WERROR)
    target_compile_options(lumen_core PRIVATE /WX)
  endif()
else()
  target_compile_options(lumen_core PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  if(LUMEN_WERROR)
    target_compile_options(lumen_core PRIVATE -Werror)
  endif()
endif()

if(LUMEN_COMPUTED_GOTO)
  target_compile_definitions(lumen_core PRIVATE LUMEN_USE_COMPUTED_GOTO=1)
endif()

if(LUMEN_SANITIZE AND NOT MSVC)
  target_compile_options(lumen_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lumen_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(lumen src/main.cpp)
target_link_libraries(lumen PRIVATE lumen_core)

enable_testing()

file(GLOB LUMEN_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(lumen_tests ${LUMEN_TEST_SOURCES})
target_link_libraries(lumen_tests PRIVATE lumen_core)
target_include_directories(lumen_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(lumen_tests PRIVATE
  LUMEN_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
add_test(NAME unit COMMAND lumen_tests)

add_executable(lumen_bench bench/bench.cpp)
target_link_libraries(lumen_bench PRIVATE lumen_core)
