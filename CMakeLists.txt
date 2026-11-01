cmake_minimum_required(VERSION 3.20)
project(TailTrace LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
#   TT_NATIVE   optimise for this machine's CPU (ON)
# ---------------------------------------------------------------------------
option(TT_NATIVE "Compile with -march=native" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "TailTrace is Linux-only (network namespaces, SO_TIMESTAMPING, recvmmsg)")
endif()

if(TT_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-march=native)
endif()
add_compile_options(-Wall -Wextra)

# Header-only core: wire format, histogram, arrivals, sequence tracker, I/O
add_library(tt_headers INTERFACE)
target_include_directories(tt_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(tt_gen src/tt_gen.cpp)
target_link_libraries(tt_gen PRIVATE tt_headers)
add_executable(tt_recv src/tt_recv.cpp)
target_link_libraries(tt_recv PRIVATE tt_headers)

enable_testing()
foreach(t test_wire test_histogram test_seq test_arrivals)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE tt_headers)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
