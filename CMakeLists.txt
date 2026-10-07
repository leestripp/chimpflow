cmake_minimum_required(VERSION 3.20)
project(ChimpFlow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# OmniNet is a fetched library maintained upstream (leestrip/OmniNet). We
# ALWAYS build the latest upstream code: GIT_TAG main, and a forced
# fast-forward to the origin/main tip at configure time (FetchContent alone
# only re-syncs a moving tag when its populate step happens to re-run, so
# without this a stale checkout can silently persist across rebuilds).
# Never pin a SHA here, never patch the fetched copy locally.
include(FetchContent)
FetchContent_Declare(
  OmniNet
  GIT_REPOSITORY git@gitlab.com:leestripp/omninet.git
  GIT_TAG main
  GIT_SHALLOW TRUE
)
set(OMNINET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(OmniNet)

# Guarantee "latest upstream": fast-forward the fetched checkout to the
# origin/main tip on every configure. Offline -> warn and build what we have.
FetchContent_GetProperties(OmniNet)
if(omninet_POPULATED AND EXISTS "${omninet_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git fetch --depth 1 origin main
    WORKING_DIRECTORY "${omninet_SOURCE_DIR}"
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE _omninet_fetch_rc)
  if(_omninet_fetch_rc EQUAL 0)
    execute_process(
      COMMAND git reset --hard FETCH_HEAD
      WORKING_DIRECTORY "${omninet_SOURCE_DIR}"
      OUTPUT_QUIET ERROR_QUIET)
  else()
    message(WARNING "OmniNet: git fetch failed (offline?); building existing checkout")
  endif()
endif()

add_executable(chimpflow src/main.cpp)

# OmniNet links OpenSSL/SQLite3/pthread/nlohmann_json PUBLIC, so its
# interface propagates everything main.cpp needs.
target_link_libraries(chimpflow PRIVATE OmniNet)

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
