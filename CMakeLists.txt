cmake_minimum_required(VERSION 3.20)
project(ChimpFlow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# OmniNet is a fetched library maintained upstream (leestrip/OmniNet). Track
# main so fixes flow down as soon as they land; never patch it locally.
# Note: a moving tag means updates require a fresh configure (FETCHCONTENT_FULLY_DISCONNECTED=OFF).
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
