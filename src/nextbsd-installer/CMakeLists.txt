# nextbsd-installer — FTXUI (C++) front-end over a /bin/sh engine.
#
# Builds two ways from one tree:
#   * Local dev (native): `cmake -S src/nextbsd-installer -B build && cmake --build build`
#     then `./build/nextbsd-installer --demo` to drive the whole UI with synthetic
#     disks (no root, no real hardware touched).
#   * CI cross (amd64/arm64): pass -DCMAKE_TOOLCHAIN_FILE=cmake/cross-<arch>.cmake.
#
# FTXUI is the vendored sibling at ../ftxui, built lib-only and statically linked.
cmake_minimum_required(VERSION 3.16)
project(nextbsd_installer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Vendored FTXUI, lib-only (no examples/tests/docs/install of its own).
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET          ON  CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ftxui ${CMAKE_BINARY_DIR}/ftxui EXCLUDE_FROM_ALL)

add_executable(nextbsd-installer
  src/main.cpp
  src/engine.cpp
  src/screen_mode.cpp
  src/screen_disk.cpp
  src/screen_install.cpp
  src/screen_finish.cpp
)
# ../libnbui carries the shared amber look (#304); src/theme.hpp is the
# installer's masthead bound to it.
target_include_directories(nextbsd-installer PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../libnbui)
# component transitively pulls dom + screen — link just it (avoids ld's
# "ignoring duplicate libraries" warning).
target_link_libraries(nextbsd-installer PRIVATE ftxui::component)

# Where the binary expects the /bin/sh engine at runtime (overridable via the
# NEXTBSD_INSTALLER_LIBEXEC env var; defaults to the installed location).
target_compile_definitions(nextbsd-installer PRIVATE
  NBI_LIBEXEC_DEFAULT="/usr/libexec/nextbsd-installer")

# Staged layout (DESTDIR=/stage in CI): /usr/sbin/nextbsd-installer + the engine.
install(TARGETS nextbsd-installer RUNTIME DESTINATION usr/sbin)
install(DIRECTORY engine/ DESTINATION usr/libexec/nextbsd-installer
        USE_SOURCE_PERMISSIONS
        FILES_MATCHING PATTERN "*.sh")
