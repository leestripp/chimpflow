cmake_minimum_required(VERSION 3.20)
project(ChimpFlow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# OmniNet lives as a sibling checkout; override with -DOMNINET_DIR=...
set(OMNINET_DIR "/projects/Websites/OmniNet" CACHE PATH "Path to OmniNet source")
set(OMNINET_BUILD_DIR "${OMNINET_DIR}/build" CACHE PATH "Path to OmniNet build (prebuilt libOmniNet.a)")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(ZLIB)

add_executable(chimpflow src/main.cpp)

target_include_directories(chimpflow PRIVATE
  "${OMNINET_DIR}/src"
  "${OMNINET_DIR}/include"
  "${OMNINET_BUILD_DIR}/_deps/nlohmann_json-src/single_include"
)

# Prefer the prebuilt static lib; fall back to building OmniNet via add_subdirectory.
find_library(OMNINET_LIB NAMES OmniNet libOmniNet.a
  PATHS "${OMNINET_BUILD_DIR}" NO_DEFAULT_PATH)

if(OMNINET_LIB)
  message(STATUS "Linking prebuilt OmniNet: ${OMNINET_LIB}")
  target_link_libraries(chimpflow PRIVATE
    "${OMNINET_LIB}"
    OpenSSL::SSL OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    dl
  )
  if(ZLIB_FOUND)
    target_link_libraries(chimpflow PRIVATE ZLIB::ZLIB)
  endif()
else()
  message(STATUS "No prebuilt libOmniNet.a; building OmniNet from source")
  set(OMNINET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(OMNINET_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
  set(OMNINET_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(OMNINET_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${OMNINET_DIR}" "${CMAKE_BINARY_DIR}/_omninet")
  target_link_libraries(chimpflow PRIVATE OmniNet)
endif()

target_compile_options(chimpflow PRIVATE -Wall -Wextra)

# Install layout: binary and web/ land side by side (~/.local/lib/chimpflow with
# prefix=$HOME/.local) — webRoot() resolves web/ relative to the executable.
# Use: cmake --install build --prefix <dir>  (DESTDIR honored)
install(TARGETS chimpflow RUNTIME DESTINATION lib/chimpflow)
install(DIRECTORY web/ DESTINATION lib/chimpflow/web)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
