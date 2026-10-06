cmake_minimum_required(VERSION 3.20)
project(chimp-echo CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OMNINET_DIR "/projects/Websites/OmniNet" CACHE PATH "Path to OmniNet source")
set(OMNINET_BUILD_DIR "${OMNINET_DIR}/build" CACHE PATH "Path to OmniNet build (prebuilt libOmniNet.a)")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(ZLIB)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_executable(chimp-echo src/main.cpp)
target_include_directories(chimp-echo PRIVATE
  ${GTKMM_INCLUDE_DIRS} ${PIPEWIRE_INCLUDE_DIRS}
  "${OMNINET_DIR}/src" "${OMNINET_DIR}/include"
  "${OMNINET_BUILD_DIR}/_deps/nlohmann_json-src/single_include"
)
find_library(OMNINET_LIB NAMES OmniNet libOmniNet.a
  PATHS "${OMNINET_BUILD_DIR}" NO_DEFAULT_PATH)
target_link_directories(chimp-echo PRIVATE ${GTKMM_LIBRARY_DIRS} ${PIPEWIRE_LIBRARY_DIRS})
target_link_libraries(chimp-echo PRIVATE
  ${GTKMM_LIBRARIES} ${PIPEWIRE_LIBRARIES} "${OMNINET_LIB}"
  OpenSSL::SSL OpenSSL::Crypto SQLite::SQLite3 Threads::Threads dl
)
target_compile_options(chimp-echo PRIVATE ${GTKMM_CFLAGS_OTHER} ${PIPEWIRE_CFLAGS_OTHER})
